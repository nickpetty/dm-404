#include "DebugPanel.h"

// The window: the SP-404MKII panel, with the debug drawer beside it.
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent() : panel (link), debug (link, panel)
    {
        addAndMakeVisible (panel);
        addAndMakeVisible (debug);
        addAndMakeVisible (status);
        status.setColour (juce::Label::textColourId, juce::Colours::grey);

        link.onBmcPacket = [this] (const uint8_t* p)
        {
            debug.logBmc (p);
            if ((p[0] & 0x0f) == 1 && p[1] <= 0x01)
            {
                const int page = p[1], idx = p[2], value = p[3];
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PanelComponent> (&panel), page, idx, value]
                {
                    if (safe != nullptr)
                        safe->setLedState (page, idx, value);
                });
            }
        };

        const auto error = link.start (EmulatorLink::defaultPaths());
        status.setText (error.isEmpty() ? "Starting emulator..." : error, juce::dontSendNotification);

        // Audio: the device settings saved last time, else the default
        // device at the emulator's own 48 kHz.
        juce::PropertiesFile::Options opts;
        opts.applicationName = "Doom-404";
        opts.filenameSuffix = ".settings";
        opts.folderName = "Doom-404";
        opts.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile> (opts);
        auto saved = settings->getXmlValue ("audioDevice");
        audio.initialise (2, 2, saved.get(), true);
        if (saved == nullptr)
        {
            auto setup = audio.getAudioDeviceSetup();
            setup.sampleRate = 48000.0;
            setup.bufferSize = 256;
            audio.setAudioDeviceSetup (setup, true);
        }
        audio.addChangeListener (this);
        audio.addAudioCallback (this);

        audioButton.onClick = [this] { showAudioSettings(); };
        addAndMakeVisible (audioButton);

        startTimerHz (60);
        setSize (1280, 900);
    }

    ~MainComponent() override
    {
        audio.removeChangeListener (this);
        audio.removeAudioCallback (this);
        link.stop();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (24);
        audioButton.setBounds (bottom.removeFromRight (140).reduced (2));
        status.setBounds (bottom);
        // The panel keeps the unit's 100:160 proportions.
        const int panelW = juce::jmin (r.getWidth() / 2, r.getHeight() * 100 / 160);
        panel.setBounds (r.removeFromLeft (panelW));
        debug.setBounds (r);
    }

private:
    void showAudioSettings()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent (audio, 0, 2, 2, 2, false, false, true, false);
        selector->setSize (520, 460);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (selector);
        o.dialogTitle = "Audio settings";
        o.dialogBackgroundColour = juce::Colour (0xff1c1d20);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        // Settings changed in the dialog: keep them for next time.
        if (auto xml = audio.createStateXml())
            settings->setValue ("audioDevice", xml.get());
        settings->saveIfNeeded();
    }

    void timerCallback() override
    {
        if (link.getScreenCount() != lastScreen)
        {
            lastScreen = link.getScreenCount();
            panel.getOled().setScreen (link.getScreen());
        }
        auto* dev = audio.getCurrentAudioDevice();
        status.setText (juce::String (link.isConnected() ? "Running" : "Not connected")
                            + "   screens " + juce::String (lastScreen)
                            + "   audio backlog " + juce::String (link.audioBacklog())
                            + "   device " + (dev ? dev->getName() + " @ " + juce::String (dev->getCurrentSampleRate()) : juce::String ("none"))
                            + "   right-click a control to learn its binding",
                        juce::dontSendNotification);
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        // Inputs: the first two enabled channels (one is used for both
        // sides), at 48 kHz, to the unit's inputs.
        if (numIn > 0 && link.isConnected())
        {
            const float* l = in[0];
            const float* r = numIn > 1 ? in[1] : in[0];
            const double inRatio = deviceRate.load() / 48000.0;
            if (std::abs (inRatio - 1.0) < 1.0e-6)
                link.sendAudioIn (l, r, numSamples);
            else
            {
                const int outFrames = (int) (numSamples / inRatio);
                capL.resize ((size_t) outFrames + 1);
                capR.resize ((size_t) outFrames + 1);
                captureL.process (inRatio, l, capL.data(), outFrames, numSamples, 0);
                captureR.process (inRatio, r, capR.data(), outFrames, numSamples, 0);
                link.sendAudioIn (capL.data(), capR.data(), outFrames);
            }
        }

        for (int ch = 0; ch < numOut; ++ch)
            juce::FloatVectorOperations::clear (out[ch], numSamples);
        if (numOut < 2)
            return;

        // The emulator makes 48 kHz; other device rates are resampled. The
        // FIFO is kept a little ahead of the device: when the emulator runs
        // fast, the surplus is dropped rather than letting latency grow.
        const double ratio = 48000.0 / deviceRate.load();
        const int target = juce::jmax (1024, (int) (numSamples * ratio) * 3);
        if (const int backlog = link.audioBacklog(); backlog > target * 4)
        {
            const int drop = backlog - target;
            scratchL.resize ((size_t) drop);
            scratchR.resize ((size_t) drop);
            link.readAudio (scratchL.data(), scratchR.data(), drop);
        }

        if (std::abs (ratio - 1.0) < 1.0e-6)
        {
            link.readAudio (out[0], out[1], numSamples);
            return;
        }
        const int need = (int) std::ceil (numSamples * ratio) + 4;
        if (link.audioBacklog() < need)
            return;                                     // underrun: silence
        inL.resize ((size_t) need);
        inR.resize ((size_t) need);
        const int got = link.readAudio (inL.data(), inR.data(), need);
        const int usedL = resampleL.process (ratio, inL.data(), out[0], numSamples, got, 0);
        resampleR.process (ratio, inR.data(), out[1], numSamples, got, 0);
        juce::ignoreUnused (usedL);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        deviceRate = dev->getCurrentSampleRate();
        resampleL.reset();
        resampleR.reset();
        captureL.reset();
        captureR.reset();
    }
    void audioDeviceStopped() override {}

    EmulatorLink link;
    PanelComponent panel;
    DebugPanel debug;
    juce::Label status;
    juce::AudioDeviceManager audio;
    std::unique_ptr<juce::PropertiesFile> settings;
    juce::TextButton audioButton { "Audio settings..." };
    std::atomic<double> deviceRate { 48000.0 };
    juce::LagrangeInterpolator resampleL, resampleR, captureL, captureR;
    std::vector<float> inL, inR, scratchL, scratchR, capL, capR;
    uint32_t lastScreen = 0;
};

class Doom404Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Doom-404"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<Window>();

        // --snapshot FILE: render the window to a PNG once the emulator has
        // had time to boot, then quit (for testing without a screen grab).
        auto args = juce::StringArray::fromTokens (commandLine, true);
        if (const int i = args.indexOf ("--snapshot"); i >= 0 && i + 1 < args.size())
        {
            const juce::File out (args[i + 1].unquoted());
            juce::Timer::callAfterDelay (25000, [this, out]
            {
                if (auto* c = window->getContentComponent())
                {
                    out.deleteFile();
                    juce::FileOutputStream os (out);
                    juce::PNGImageFormat().writeImageToStream (c->createComponentSnapshot (c->getLocalBounds()), os);
                }
                quit();
            });
        }
    }

    void shutdown() override { window.reset(); }

    void systemRequestedQuit() override { quit(); }

private:
    class Window : public juce::DocumentWindow
    {
    public:
        Window() : juce::DocumentWindow ("Doom-404: SP-404MKII emulator", juce::Colours::black, allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (Doom404Application)
