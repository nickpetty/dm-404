#include "DebugPanel.h"
#include "SdCardWindow.h"
#include "Storage.h"
#include "DawBridge.h"
#include "MidiPorts.h"

// First run: Roland's System Program is not ours to ship, so the user
// points the app at the one they downloaded.
class SetupComponent : public juce::Component
{
public:
    std::function<void (const juce::File&)> onChosen;

    SetupComponent()
    {
        text.setJustificationType (juce::Justification::centred);
        text.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8d0));
        text.setFont (juce::FontOptions (17.0f));
        text.setText ("Doom-404 runs Roland's own SP-404MKII firmware, which is not included.\n\n"
                      "Download the SP-404MKII System Program (version 5.52) from roland.com, "
                      "then choose the zip you downloaded, or the SP404MKII_APP1.bin inside it.\n\n"
                      "A blank internal drive and SD card are made for you.",
                      juce::dontSendNotification);
        addAndMakeVisible (text);
        addAndMakeVisible (choose);
        addAndMakeVisible (error);
        error.setJustificationType (juce::Justification::centred);
        error.setColour (juce::Label::textColourId, juce::Colour (0xffff5a1f));
        choose.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("SP-404MKII System Program",
                                                           juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                                               .getChildFile ("Downloads"),
                                                           "*.zip;*.bin");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      if (fc.getResult() != juce::File() && onChosen)
                                          onChosen (fc.getResult());
                                  });
        };
    }

    void setError (const juce::String& e) { error.setText (e, juce::dontSendNotification); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xf01c1d20)); }

    void resized() override
    {
        auto r = getLocalBounds().withSizeKeepingCentre (juce::jmin (560, getWidth() - 40), 320);
        text.setBounds (r.removeFromTop (190));
        choose.setBounds (r.removeFromTop (36).withSizeKeepingCentre (260, 36));
        r.removeFromTop (12);
        error.setBounds (r);
    }

private:
    juce::Label text, error;
    juce::TextButton choose { "Choose System Program..." };
    std::unique_ptr<juce::FileChooser> chooser;
};

// The window: the SP-404MKII panel, with the debug drawer beside it.
class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      private juce::AudioIODeviceCallback,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent() : panel (link), debug (link, panel)
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName = "Doom-404";
        opts.filenameSuffix = ".settings";
        opts.folderName = "Doom-404";
        opts.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile> (opts);
        sdSlot = std::make_unique<SdSlot> (link, *settings);
        sdSlot->onChange = [this] { menuItemsChanged(); };

        addAndMakeVisible (panel);
        addAndMakeVisible (debug);
        addAndMakeVisible (status);
        status.setColour (juce::Label::textColourId, juce::Colours::grey);

        link.onBmcPacket = [this] (const uint8_t* p)
        {
            debug.logBmc (p);
            if ((p[0] & 0x0f) == 1 && p[1] < 0x10)
            {
                const int page = p[1], idx = p[2], value = p[3];
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PanelComponent> (&panel), page, idx, value]
                {
                    if (safe != nullptr)
                        safe->setLedState (page, idx, value);
                });
            }
        };

        link.onSdCard = [this] (bool in, const juce::String& e)
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), in, e]
            {
                if (safe != nullptr)
                    safe->sdSlot->handleReply (in, e);
            });
        };

        restoreBackground();

        // The DAW plugin's link: the unit's output to it, its audio in.
        daw = std::make_unique<DawBridge> (link);
        muteForDaw = settings->getBoolValue ("muteForDaw", true);
        link.onAudioOut = [this] (const int16_t* lr, int frames, uint32_t after) { daw->unitOutput (lr, frames, after); };
        link.onBuses = [this] (const int16_t* buses, int frames) { daw->unitBuses (buses, frames); };
        // The unit's MIDI out: USB to the plugin, the OUT jack to a MIDI port.
        // MIDI learn: MIDI devices on the panel's controls.
        midiLearn = std::make_unique<MidiLearn> (*settings);
        midiLearn->onControl = [this] (const juce::String& c, bool note, int value, MidiLearn::Mode mode)
        {
            panel.fromMidi (c, note, value, mode);
        };
        midiLearn->onLearnt = [this] (const juce::String& c, const juce::String& what)
        {
            panel.showMidiLearnt (c, what);
        };
        panel.onMidiLearn = [this] (const juce::String& c)
        {
            midiLearn->learn (c);
            panel.setMidiLearning (c);
            if (c.isNotEmpty() && midiPorts->inputIds().isEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "MIDI learn",
                                                        "No MIDI input is chosen: pick your MIDI device in "
                                                        "Options > MIDI IN, then move a control on it.");
        };
        panel.onMidiForget = [this] (const juce::String& c) { midiLearn->forget (c); };
        panel.midiMapping = [this] (const juce::String& c) { return midiLearn->describe (c); };
        midiPorts = std::make_unique<MidiPorts> (link, *settings, *midiLearn);
        link.onUnitMidi = [this] (uint32_t frame, const uint8_t* p)
        {
            if ((p[0] >> 4) & usbmidi::usbBit)
                daw->unitMidi (frame, p);
            midiPorts->unitMidi (p);
        };

        // A development checkout's firmware and drives come over once; with
        // no firmware yet, the setup screen asks for it.
        adopted = Storage::adoptDevFiles();
        addChildComponent (setup);
        setup.onChosen = [this] (const juce::File& f) { installFirmware (f); };
        if (Storage::firmware().existsAsFile())
            startEmulator();
        else
            setup.setVisible (true);

        // Audio: the device settings saved last time, else the default
        // device at the emulator's own 48 kHz.
        auto saved = settings->getXmlValue ("audioDevice");
        audio.initialise (2, 2, saved.get(), true);
        if (saved == nullptr)
        {
            auto deviceSetup = audio.getAudioDeviceSetup();
            deviceSetup.sampleRate = 48000.0;
            deviceSetup.bufferSize = 256;
            audio.setAudioDeviceSetup (deviceSetup, true);
        }
        audio.addChangeListener (this);
        audio.addAudioCallback (this);

        audioButton.onClick = [this] { showAudioSettings(); };
        addAndMakeVisible (audioButton);

        startTimerHz (60);
        setSize (1280, 900);
        setWantsKeyboardFocus (true);
        panel.setBindingLearn (settings->getBoolValue ("debugShown", true));
        if (! settings->getBoolValue ("debugShown", true))
            setDebugShown (false);
    }

    ~MainComponent() override
    {
        sdWindow.reset();
        audio.removeChangeListener (this);
        audio.removeAudioCallback (this);
        link.stop();
        daw.reset();            // after the link thread, which feeds it
        midiPorts.reset();
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    void resized() override
    {
        auto r = getLocalBounds();
        setup.setBounds (r);
        if (! debugShown)
        {
            // Just the panel, centred, at the unit's 100:160 proportions.
            const int w = juce::jmin (r.getWidth(), r.getHeight() * 100 / 160);
            panel.setBounds (r.withSizeKeepingCentre (w, juce::jmin (r.getHeight(), w * 160 / 100)));
            return;
        }
        auto bottom = r.removeFromBottom (24);
        audioButton.setBounds (bottom.removeFromRight (140).reduced (2));
        status.setBounds (bottom);
        // The panel keeps the unit's 100:160 proportions.
        const int panelW = juce::jmin (r.getWidth() / 2, r.getHeight() * 100 / 160);
        panel.setBounds (r.removeFromLeft (panelW));
        debug.setBounds (r);
    }

    // ` shows or hides the debug drawer and the status bar; the window
    // narrows to the panel and widens again (unless maximised).
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getTextCharacter() != '`')
            return false;
        setDebugShown (! debugShown);
        return true;
    }

    void setDebugShown (bool shown)
    {
        debugShown = shown;
        panel.setBindingLearn (shown);
        for (juce::Component* c : { (juce::Component*) &debug, (juce::Component*) &status, (juce::Component*) &audioButton })
            c->setVisible (shown);
        settings->setValue ("debugShown", shown);
        settings->saveIfNeeded();
        auto* top = getTopLevelComponent();
        auto* window = dynamic_cast<juce::ResizableWindow*> (top);
        // At startup (restoring the setting) there is no window yet: sizing
        // the content is enough, the window is made to fit it.
        if (window == nullptr || (! window->isFullScreen() && ! window->isMinimised()))
        {
            const int panelW = getHeight() * 100 / 160;
            if (! shown)
            {
                debugWidth = juce::jmax (0, getWidth() - panelW);
                setSize (panelW, getHeight());
            }
            else
                setSize (panelW + (debugWidth > 0 ? debugWidth : panelW), getHeight());
        }
        resized();
        if (isShowing())
            grabKeyboardFocus();
    }

    //==========================================================================
    // Starting the unit, and the setup that comes before it.
    void startEmulator()
    {
        juce::String error = Storage::ensureDrives();
        if (error.isEmpty())
        {
            auto paths = EmulatorLink::defaultPaths();
            paths.sdInserted = sdSlot->isInserted();
            error = link.start (paths);
        }
        status.setText (error.isEmpty() ? "Starting emulator..." : error, juce::dontSendNotification);
        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Doom-404", error);
        else if (! adopted.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Doom-404",
                "Brought over from the development checkout:\n" + adopted.joinIntoString ("\n")
                    + "\n\nThe unit now lives in " + Storage::dataDir().getFullPathName());
        adopted.clear();
    }

    void installFirmware (const juce::File& f)
    {
        bool wrongVersion = false;
        if (auto e = Storage::importFirmware (f, wrongVersion); e.isNotEmpty())
        {
            setup.setError (e);
            return;
        }
        setup.setVisible (false);
        if (wrongVersion)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Doom-404",
                "That is not System Program 5.52, the version Doom-404 is made for. It may not work.");
        restartEmulator();
    }

    void restartEmulator()
    {
        link.stop();
        startEmulator();
    }

    //==========================================================================
    // The menu bar.
    enum MenuIds { sdWindowId = 1, sdToggleId, restartId, backupId, restoreId, openDataId, chooseFirmwareId,
                   debugDrawerId, audioSettingsId, muteForDawId, backgroundId, resetBackgroundId,
                   textColourId, resetTextColourId, virtualMidiId, midiServicesId, forgetMidiId,
                   midiInBase = 1000, midiOutBase = 2000 };     // + device index + 1 (0: none)

    juce::StringArray getMenuBarNames() override { return { "Unit", "View", "Options" }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        if (index == 0)
        {
            m.addItem (sdWindowId, "SD card...");
            m.addItem (sdToggleId, sdSlot->isInserted() ? "Take SD card out" : "Put SD card in");
            m.addSeparator();
            m.addItem (backupId, "Back up internal drive to a folder...");
            m.addItem (restoreId, "Restore internal drive from a folder...");
            m.addSeparator();
            m.addItem (restartId, "Restart");
            m.addItem (chooseFirmwareId, "Choose System Program...");
            m.addItem (openDataId, "Open data folder");
        }
        else if (index == 1)
        {
            m.addItem (debugDrawerId, "Debug drawer\t`", true, debugShown);
            m.addSeparator();
            m.addItem (backgroundId, "Background image...");
            m.addItem (resetBackgroundId, "Plain background", panel.hasCustomBackground());
            m.addItem (textColourId, "Text colour...");
            m.addItem (resetTextColourId, "Default text colour", panel.getTextColour() != PanelComponent::defaultTextColour());
        }
        else
        {
            m.addItem (audioSettingsId, "Audio settings...");
            m.addItem (muteForDawId, "Mute this app while a DAW plugin plays the unit", true, muteForDaw.load());
            m.addSeparator();
            // The unit's MIDI jacks, as the computer's MIDI ports.
            // Inputs: any number (a keyboard and a controller); output: one.
            auto ports = [] (juce::PopupMenu& sub, int base, const juce::Array<juce::MidiDeviceInfo>& list,
                             const juce::StringArray& current)
            {
                sub.addItem (base, "None", true, current.isEmpty());
                for (int i = 0; i < list.size(); ++i)
                    sub.addItem (base + 1 + i, list[i].name, true, current.contains (list[i].identifier));
            };
            midiInList = juce::MidiInput::getAvailableDevices();
            midiOutList = juce::MidiOutput::getAvailableDevices();
            juce::PopupMenu in, out;
            ports (in, midiInBase, midiInList, midiPorts->inputIds());
            const auto outId = midiPorts->outputId();
            ports (out, midiOutBase, midiOutList, outId.isEmpty() ? juce::StringArray() : juce::StringArray (outId));
            m.addSubMenu ("MIDI IN (plays the unit; MIDI learn)", in);
            m.addSubMenu ("MIDI OUT (from the unit)", out);
            m.addItem (forgetMidiId, "Forget all MIDI learn mappings", ! midiLearn->empty());
            // Its USB MIDI as a MIDI device of its own; what is wrong, if anything.
            const auto portStatus = midiPorts->virtualStatus();
            m.addItem (virtualMidiId, "MIDI port \"Doom-404\"" + (portStatus.isEmpty() ? juce::String() : " (" + portStatus + ")"),
                       true, midiPorts->wantsVirtual());
            if (portStatus.contains ("Windows MIDI Services"))
                m.addItem (midiServicesId, "Get Windows MIDI Services...");
        }
        return m;
    }

    void menuItemSelected (int id, int) override
    {
        if (id >= midiInBase && id < midiOutBase)
        {
            const int i = id - midiInBase - 1;
            if (i >= 0 && i < midiInList.size())
                midiPorts->toggleInput (midiInList[i].identifier);
            else
                midiPorts->setInputs ({});
            return;
        }
        if (id >= midiOutBase)
        {
            const int i = id - midiOutBase - 1;
            midiPorts->setOutput (i >= 0 && i < midiOutList.size() ? midiOutList[i].identifier : juce::String());
            return;
        }
        switch (id)
        {
            case sdWindowId:       showSdCard(); break;
            case sdToggleId:
                sdSlot->setInserted (! sdSlot->isInserted(), [] (const juce::String& e)
                {
                    if (e.isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SD card", e);
                });
                break;
            case backupId:         backupInternal(); break;
            case restoreId:        restoreInternal(); break;
            case restartId:        restartEmulator(); break;
            case chooseFirmwareId: setup.setError ({}); setup.setVisible (true); setup.toFront (false); break;
            case openDataId:       Storage::dataDir().startAsProcess(); break;
            case debugDrawerId:    setDebugShown (! debugShown); break;
            case audioSettingsId:  showAudioSettings(); break;
            case backgroundId:     chooseBackground(); break;
            case resetBackgroundId:
                panel.setCustomBackground ({});
                settings->removeValue ("background");
                settings->saveIfNeeded();
                break;
            case textColourId:     chooseTextColour(); break;
            case virtualMidiId:    midiPorts->setVirtual (! midiPorts->wantsVirtual()); break;
            case forgetMidiId:     midiLearn->forgetAll(); break;
            case midiServicesId:   juce::URL ("https://aka.ms/midi").launchInDefaultBrowser(); break;
            case resetTextColourId:
                panel.setTextColour (PanelComponent::defaultTextColour());
                settings->removeValue ("textColour");
                settings->saveIfNeeded();
                break;
            case muteForDawId:
                muteForDaw = ! muteForDaw.load();
                settings->setValue ("muteForDaw", muteForDaw.load());
                settings->saveIfNeeded();
                break;
            default:               break;
        }
    }

    // A picture for the panel, copied into the data folder. (The text
    // printed on the panel has its own colour: chooseTextColour.)
    void chooseBackground()
    {
        chooser = std::make_unique<juce::FileChooser> ("Background image",
                                                       juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                                       "*.png;*.jpg;*.jpeg;*.gif;*.bmp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            const auto src = fc.getResult();
            auto image = juce::ImageFileFormat::loadFrom (src);
            if (! image.isValid())
            {
                if (src != juce::File())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Background",
                                                            "Could not read that image.");
                return;
            }
            const auto dest = Storage::dataDir().getChildFile ("background" + src.getFileExtension().toLowerCase());
            for (auto& old : Storage::dataDir().findChildFiles (juce::File::findFiles, false, "background.*"))
                if (old != src)
                    old.deleteFile();
            if (src != dest)
                src.copyFileTo (dest);
            panel.setCustomBackground (image);
            settings->setValue ("background", dest.getFullPathName());
            settings->saveIfNeeded();
        });
    }

    void chooseTextColour()
    {
        struct Picker : juce::Component, juce::ChangeListener
        {
            Picker (PanelComponent& p, juce::PropertiesFile& s) : panel (p), settings (s)
            {
                selector.setCurrentColour (panel.getTextColour());
                selector.addChangeListener (this);
                addAndMakeVisible (selector);
                addAndMakeVisible (note);
                note.setText ("The colour of the text printed on the panel.", juce::dontSendNotification);
                setSize (360, 420);
            }
            void resized() override
            {
                auto r = getLocalBounds().reduced (8);
                note.setBounds (r.removeFromTop (24));
                selector.setBounds (r);
            }
            void changeListenerCallback (juce::ChangeBroadcaster*) override
            {
                panel.setTextColour (selector.getCurrentColour());
                settings.setValue ("textColour", selector.getCurrentColour().toString());
                settings.saveIfNeeded();
            }
            PanelComponent& panel;
            juce::PropertiesFile& settings;
            juce::ColourSelector selector { juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders
                                            | juce::ColourSelector::showColourspace };
            juce::Label note;
        };
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new Picker (panel, *settings));
        o.dialogTitle = "Text colour";
        o.dialogBackgroundColour = juce::Colour (0xff1c1d20);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void restoreBackground()
    {
        if (settings->containsKey ("textColour"))
            panel.setTextColour (juce::Colour::fromString (settings->getValue ("textColour")));
        const juce::File f (settings->getValue ("background"));
        if (settings->getValue ("background").isEmpty() || ! f.existsAsFile())
            return;
        auto image = juce::ImageFileFormat::loadFrom (f);
        if (image.isValid())
            panel.setCustomBackground (image);
    }

    void showSdCard()
    {
        if (sdWindow == nullptr)
        {
            sdWindow = std::make_unique<SdCardWindow> (*sdSlot, Storage::sdCard(), [this]
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                {
                    if (safe != nullptr)
                        safe->sdWindow.reset();
                });
            });
            sdWindow->centreWithSize (sdWindow->getWidth(), sdWindow->getHeight());
            sdWindow->setVisible (true);
        }
        sdWindow->toFront (true);
        sdWindow->content().refresh();
    }

    // Backups are the drive's files, copied to a folder (the image is a
    // mostly empty 16 GB); restoring makes a fresh drive from such a folder.
    // The unit is off meanwhile.
    void backupInternal()
    {
        chooser = std::make_unique<juce::FileChooser> ("Back up the internal drive into",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest == juce::File())
                return;
            dest = dest.getChildFile ("SP-404MKII backup " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M"));
            runDriveJob ("Backing up the internal drive", [dest] (juce::ThreadWithProgressWindow& j) -> juce::String
            {
                fatimg::Volume v;
                if (auto e = v.open (Storage::toPath (Storage::internalDrive()), true); ! e.empty())
                    return juce::String (e);
                std::vector<fatimg::Entry> top;
                if (auto e = v.list ("/", top); ! e.empty())
                    return juce::String (e);
                dest.createDirectory();
                for (auto& t : top)
                    if (auto e = v.extract ("/" + t.name, Storage::toPath (dest), [&j] (const std::string& p)
                        {
                            j.setStatusMessage (juce::String::fromUTF8 (p.c_str()));
                            return ! j.threadShouldExit();
                        });
                        ! e.empty())
                        return juce::String::fromUTF8 (e.c_str());
                dest.revealToUser();
                return {};
            });
        });
    }

    void restoreInternal()
    {
        chooser = std::make_unique<juce::FileChooser> ("Restore the internal drive from (a backup folder)",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
        {
            const auto src = fc.getResult();
            if (src == juce::File())
                return;
            auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Restore",
                "Replace everything on the internal drive with the contents of\n" + src.getFullPathName() + "?",
                "Replace", "Cancel", this);
            juce::AlertWindow::showAsync (opts, [this, src] (int result)
            {
                if (result != 1)
                    return;
                runDriveJob ("Restoring the internal drive", [src] (juce::ThreadWithProgressWindow& j) -> juce::String
                {
                    // Into a new image first: the old one stays if this fails.
                    const auto fresh = Storage::internalDrive().getSiblingFile ("internal.new.img");
                    if (auto e = fatimg::create (Storage::toPath (fresh), (uint64_t) Storage::internalBytes,
                                                 fatimg::Format::exfat, "SP-404MKII", false);
                        ! e.empty())
                        return juce::String (e);
                    {
                        fatimg::Volume v;
                        if (auto e = v.open (Storage::toPath (fresh), false); ! e.empty())
                            return juce::String (e);
                        for (auto& child : src.findChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false))
                            if (auto e = v.add (Storage::toPath (child), "/", [&j] (const std::string& p)
                                {
                                    j.setStatusMessage (juce::String::fromUTF8 (p.c_str()));
                                    return ! j.threadShouldExit();
                                });
                                ! e.empty())
                            {
                                v.close();
                                fresh.deleteFile();
                                return juce::String::fromUTF8 (e.c_str());
                            }
                    }
                    if (! fresh.moveFileTo (Storage::internalDrive()))
                        return "Could not replace " + Storage::internalDrive().getFullPathName();
                    return {};
                });
            });
        });
    }

    // Stops the unit, runs `job` behind a progress window, starts it again.
    void runDriveJob (const juce::String& title, std::function<juce::String (juce::ThreadWithProgressWindow&)> job)
    {
        struct Job : juce::ThreadWithProgressWindow
        {
            Job (const juce::String& t, std::function<juce::String (juce::ThreadWithProgressWindow&)> w,
                 std::function<void (juce::String)> d)
                : juce::ThreadWithProgressWindow (t, true, true), work (std::move (w)), done (std::move (d))
            {
                setProgress (-1.0);
            }
            void run() override { result = work (*this); }
            void threadComplete (bool cancelled) override
            {
                done (cancelled && result.isEmpty() ? juce::String ("Stopped.") : result);
                delete this;
            }
            std::function<juce::String (juce::ThreadWithProgressWindow&)> work;
            std::function<void (juce::String)> done;
            juce::String result;
        };
        link.stop();
        status.setText ("Unit off: " + title, juce::dontSendNotification);
        (new Job (title, std::move (job), [safe = juce::Component::SafePointer<MainComponent> (this), title] (juce::String e)
        {
            if (e.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, e);
            if (safe != nullptr)
                safe->startEmulator();
        }))->launchThread();
    }

public:
    PanelComponent& getPanel() { return panel; }
    SdCardWindow* getSdWindow() { return sdWindow.get(); }

    void modifierKeysChanged (const juce::ModifierKeys& mods) override
    {
        panel.setShiftFromKeyboard (mods.isShiftDown());
    }

    // --test-audio: pads 13 and 14 twice each, then pad 3 once, left to
    // play for 10 s; what went to the audio device is saved as a WAV and
    // the counts shown. For checking the audio path unattended.
    void runAudioTest (const juce::File& out, std::function<void()> done)
    {
        recording = true;
        underruns = 0;
        skips = 0;
        lateCallbacks = 0;
        auto hit = [this] (int at, int ch, int mux)
        {
            juce::Timer::callAfterDelay (at, [this, ch, mux] { link.sendKnob (0, ch, mux, 300); });
            juce::Timer::callAfterDelay (at + 100, [this, ch, mux] { link.sendKnob (0, ch, mux, 4095); });
        };
        hit (300, 5, 2);        // pad 13
        hit (800, 5, 1);        // pad 14
        hit (1300, 5, 2);
        hit (1800, 5, 1);
        hit (2500, 4, 5);       // pad 3
        juce::Timer::callAfterDelay (13000, [this, out, done]
        {
            recording = false;
            juce::AudioBuffer<float> buf;
            {
                const juce::SpinLock::ScopedLockType sl (recordLock);
                buf.setSize (2, (int) recorded[0].size());
                buf.copyFrom (0, 0, recorded[0].data(), buf.getNumSamples());
                buf.copyFrom (1, 0, recorded[1].data(), buf.getNumSamples());
            }
            out.deleteFile();
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (new juce::FileOutputStream (out), deviceRate.load(), 2, 16, {}, 0))
            {
                w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
                delete w;
            }
            out.withFileExtension ("txt").replaceWithText ("underruns " + juce::String (underruns.load())
                                                           + "\nskips " + juce::String (skips.load())
                                                           + "\nlate callbacks " + juce::String (lateCallbacks.load())
                                                           + "\ncushion ms " + juce::String ((int) (cushion.load() / 48.0)) + "\n");
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Audio test finished",
                "Saved " + out.getFullPathName() + "\n\nUnderruns (ran dry): " + juce::String (underruns.load())
                    + "\nSkips (jumped ahead): " + juce::String (skips.load()));
            done();
        });
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
        // The Doom-404 MIDI port's troubles go to the log (for bug reports).
        if (const auto s = midiPorts->virtualStatus(); s != lastPortStatus)
        {
            lastPortStatus = s;
            log ("MIDI port \"Doom-404\": " + (s.isEmpty() ? juce::String (midiPorts->wantsVirtual() ? "up" : "off") : s));
        }
        // After 10 s without running dry, give latency back: the cushion
        // shrinks by 6 ms a second towards 60 ms (it grows while booting,
        // when the emulator is busy loading samples).
        if (juce::Time::getMillisecondCounter() - lastUnderrunMs.load() > 10000 && cushion.load() > 2880.0)
            cushion = juce::jmax (2880.0, cushion.load() - 4.8);
        if (link.getScreenCount() != lastScreen)
        {
            lastScreen = link.getScreenCount();
            panel.getOled().setScreen (link.getScreen());
        }
        auto* dev = audio.getCurrentAudioDevice();
        status.setText (juce::String (link.isConnected() ? "Running" : "Not connected")
                            + "   screens " + juce::String (lastScreen)
                            + "   audio backlog " + juce::String (link.audioBacklog())
                            + "   cushion " + juce::String ((int) (cushion.load() / 48.0)) + " ms"
                            + "   underruns " + juce::String (underruns.load())
                            + "   skips " + juce::String (skips.load())
                            + "   late callbacks " + juce::String (lateCallbacks.load())
                            + (daw != nullptr && daw->pluginConnected() ? "   DAW plugin connected" : "")
                            + "   device " + (dev ? dev->getName() + " @ " + juce::String (dev->getCurrentSampleRate()) : juce::String ("none"))
                            + "   right-click a control: MIDI learn, or learn its hardware binding",
                        juce::dontSendNotification);
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override
    {
        // Count callbacks that use more than half their time: the device
        // glitches when one overruns, whatever the samples were.
        const auto t0 = juce::Time::getHighResolutionTicks();
        const juce::ScopeGuard timing { [&]
        {
            const double used = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            if (used > 0.5 * numSamples / deviceRate.load())
                ++lateCallbacks;
        } };
        juce::ignoreUnused (ctx);
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

        // The emulator makes 48 kHz audio, but not at an even pace (it waits
        // on the firmware), and the device's clock is not the emulator's.
        // Everything goes through a resampler whose rate is steered, by at
        // most 0.5%, to hold a cushion of about 30 ms in the FIFO: no clicks
        // from running dry or from skipping ahead. Input is staged, so no
        // sample is ever thrown away between callbacks.
        const double base = 48000.0 / deviceRate.load();
        // The cushion starts at 60 ms and grows by 20 ms (to 200 ms at most)
        // each time it runs dry, settling at what this machine needs.
        const double target = cushion.load();
        const double fill = link.audioBacklog() + (double) staged;

        // Well over the cushion (more than ~110 ms over): drop back at once.
        // Steering only closes 0.5% a second, so a surplus left from boot
        // would otherwise sit there as delay (heard on the inputs through
        // EXT SOURCE) for a minute or more.
        if (fill > target * 2 + 2400)
        {
            ++skips;
            // Far behind (the device stalled, or the emulator burst ahead):
            // one jump back to the cushion is better than seconds of lag.
            const int drop = (int) (fill - target) - staged;
            scratchL.resize ((size_t) juce::jmax (0, drop));
            scratchR.resize ((size_t) juce::jmax (0, drop));
            link.readAudio (scratchL.data(), scratchR.data(), juce::jmax (0, drop));
        }
        // Too full: consume a little faster; too low: a little slower.
        const double want = juce::jlimit (-1.0, 1.0, (fill - target) / target) * 0.005;
        steer += (want - steer) * 0.02;
        const double ratio = base * (1.0 + steer);

        const int needed = (int) std::ceil (numSamples * ratio) + 4;
        if (staged < needed)
        {
            inL.resize ((size_t) needed);
            inR.resize ((size_t) needed);
            staged += link.readAudio (inL.data() + staged, inR.data() + staged, needed - staged);
        }
        if (staged < needed)
        {
            // Dry: wait for the cushion to build up again rather than play
            // fragments.
            if (primed)
            {
                ++underruns;
                cushion = juce::jmin (9600.0, cushion.load() + 960.0);
                lastUnderrunMs = juce::Time::getMillisecondCounter();
            }
            primed = false;
            return;
        }
        if (! primed && fill < target)
            return;
        primed = true;

        const int used = resampleL.process (ratio, inL.data(), out[0], numSamples, staged, 0);
        resampleR.process (ratio, inR.data(), out[1], numSamples, staged, 0);
        if (muteForDaw.load() && daw != nullptr && daw->pluginConnected())
        {
            // The DAW plays the unit through the plugin: not twice.
            juce::FloatVectorOperations::clear (out[0], numSamples);
            juce::FloatVectorOperations::clear (out[1], numSamples);
        }
        if (recording.load())
        {
            const juce::SpinLock::ScopedLockType sl (recordLock);
            recorded[0].insert (recorded[0].end(), out[0], out[0] + numSamples);
            recorded[1].insert (recorded[1].end(), out[1], out[1] + numSamples);
        }
        std::memmove (inL.data(), inL.data() + used, sizeof (float) * (size_t) (staged - used));
        std::memmove (inR.data(), inR.data() + used, sizeof (float) * (size_t) (staged - used));
        staged -= used;
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        deviceRate = dev->getCurrentSampleRate();
        resampleL.reset();
        resampleR.reset();
        staged = 0;
        steer = 0.0;
        primed = false;
        captureL.reset();
        captureR.reset();
    }
    void audioDeviceStopped() override {}

    EmulatorLink link;
    std::unique_ptr<DawBridge> daw;
    std::unique_ptr<MidiLearn> midiLearn;         // before midiPorts, which uses it
    std::unique_ptr<MidiPorts> midiPorts;
    juce::String lastPortStatus { "?" };

    // A line in doom-404.log in the data folder.
    static void log (const juce::String& line)
    {
        Storage::dataDir().getChildFile ("doom-404.log")
            .appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  " + line + "\n");
    }
    juce::Array<juce::MidiDeviceInfo> midiInList, midiOutList;     // as last shown in the menu
    std::atomic<bool> muteForDaw { true };      // silence the app while a DAW plugin plays the unit
    PanelComponent panel;
    DebugPanel debug;
    juce::Label status;
    SetupComponent setup;
    std::unique_ptr<SdSlot> sdSlot;
    std::unique_ptr<SdCardWindow> sdWindow;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::StringArray adopted;
    juce::TooltipWindow tooltips { this, 600 };
    juce::AudioDeviceManager audio;
    std::unique_ptr<juce::PropertiesFile> settings;
    juce::TextButton audioButton { "Audio settings..." };
    std::atomic<double> deviceRate { 48000.0 };
    juce::LagrangeInterpolator resampleL, resampleR, captureL, captureR;
    std::vector<float> inL, inR, scratchL, scratchR, capL, capR;
    int staged = 0;                 // resampler input not yet consumed
    std::atomic<bool> recording { false };
    juce::SpinLock recordLock;
    std::vector<float> recorded[2];
    double steer = 0.0;             // rate correction holding the cushion
    bool primed = false;
    std::atomic<int> underruns { 0 }, skips { 0 }, lateCallbacks { 0 };
    std::atomic<double> cushion { 2880.0 };     // frames at 48 kHz
    std::atomic<juce::uint32> lastUnderrunMs { 0 };
    uint32_t lastScreen = 0;
    bool debugShown = true;
    int debugWidth = 0;             // what the drawer had before it was hidden
};

class Doom404Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Doom-404"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String& commandLine) override
    {
        // A crash leaves a stack trace in the temp folder (doom404-crash.txt).
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("doom404-crash.txt")
                .replaceWithText (juce::SystemStats::getStackBacktrace());
        });

        window = std::make_unique<Window>();

        // --snapshot FILE: render the window to a PNG once the emulator has
        // had time to boot, then quit (for testing without a screen grab).
        auto args = juce::StringArray::fromTokens (commandLine, true);
        if (const int i = args.indexOf ("--test-audio"); i >= 0 && i + 1 < args.size())
        {
            const juce::File out (args[i + 1].unquoted());
            juce::Timer::callAfterDelay (25000, [this, out]
            {
                if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
                    mc->runAudioTest (out, [] {});
            });
        }
        // --press NAME (repeatable): tap those panel controls 20 s in, one a
        // second, before a --snapshot is taken.
        for (int i = 0; (i = args.indexOf ("--press", false, i)) >= 0 && i + 1 < args.size(); i += 2)
        {
            const auto name = args[i + 1].unquoted();
            const int n = i;
            juce::Timer::callAfterDelay (20000 + 1000 * n, [this, name]
            {
                if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
                    mc->getPanel().tap (name);
            });
        }
        // --sd-window: open the SD card window 20 s in (a --snapshot then
        // also saves it, as NAME-sd.png).
        if (args.contains ("--sd-window"))
            juce::Timer::callAfterDelay (20000, [this]
            {
                if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
                    mc->showSdCard();
            });
        if (const int i = args.indexOf ("--snapshot"); i >= 0 && i + 1 < args.size())
        {
            const juce::File out (args[i + 1].unquoted());
            juce::Timer::callAfterDelay (25000, [this, out]
            {
                if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
                    if (auto* sd = mc->getSdWindow())
                    {
                        auto f = out.getSiblingFile (out.getFileNameWithoutExtension() + "-sd.png");
                        f.deleteFile();
                        juce::FileOutputStream os (f);
                        juce::PNGImageFormat().writeImageToStream (sd->createComponentSnapshot (sd->getLocalBounds()), os);
                    }
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
            auto* main = new MainComponent();
            setContentOwned (main, true);
            setMenuBar (main);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        ~Window() override { setMenuBar (nullptr); }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (Doom404Application)
