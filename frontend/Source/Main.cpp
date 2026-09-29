#include "DebugPanel.h"

// The window: the SP-404MKII panel, with the debug drawer beside it.
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::Timer
{
public:
    MainComponent() : panel (link), debug (link, panel)
    {
        addAndMakeVisible (panel);
        addAndMakeVisible (debug);
        addAndMakeVisible (status);
        status.setColour (juce::Label::textColourId, juce::Colours::grey);

        link.onBmcPacket = [this] (const uint8_t* p) { debug.logBmc (p); };

        const auto error = link.start (EmulatorLink::defaultPaths());
        status.setText (error.isEmpty() ? "Starting emulator..." : error, juce::dontSendNotification);

        audio.initialiseWithDefaultDevices (0, 2);
        auto setup = audio.getAudioDeviceSetup();
        setup.sampleRate = 48000.0;
        setup.bufferSize = 256;
        audio.setAudioDeviceSetup (setup, true);
        audio.addAudioCallback (this);

        startTimerHz (60);
        setSize (1280, 900);
    }

    ~MainComponent() override
    {
        audio.removeAudioCallback (this);
        link.stop();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        status.setBounds (r.removeFromBottom (22));
        // The panel keeps the unit's 100:160 proportions.
        const int panelW = juce::jmin (r.getWidth() / 2, r.getHeight() * 100 / 160);
        panel.setBounds (r.removeFromLeft (panelW));
        debug.setBounds (r);
    }

private:
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

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        if (numOut < 2)
            return;
        // Keep a small cushion; if the emulator runs ahead, skip the excess
        // rather than drift further behind.
        const int backlog = link.audioBacklog();
        if (backlog > 9600)
        {
            std::vector<float> scratch ((size_t) (backlog - 2400) * 2);
            link.readAudio (scratch.data(), scratch.data() + (backlog - 2400), backlog - 2400);
        }
        const int got = link.readAudio (out[0], out[1], numSamples);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::clear (out[ch] + got, numSamples - got);
        for (int ch = 2; ch < numOut; ++ch)
            juce::FloatVectorOperations::clear (out[ch], numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    EmulatorLink link;
    PanelComponent panel;
    DebugPanel debug;
    juce::Label status;
    juce::AudioDeviceManager audio;
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
