#pragma once

#include "EmulatorLink.h"
#include "Storage.h"
#include "UsbMidi.h"
#include "WinMidi.h"

// The unit's MIDI, as the computer's MIDI ports.
//
// - Its MIDI IN and OUT jacks: a chosen input and output (a hardware
//   interface, or loopMIDI and the like). What arrives at the input plays
//   the unit (cable 9); what the unit sends to its OUT jack goes out.
// - Its USB MIDI: a MIDI device of the app's own, "Doom-404", that other
//   programs see (cable 8 in, the USB bit out). macOS and Linux make one
//   for any app (JUCE); on Windows it goes through Windows MIDI Services
//   (WinMidi.h), when that is there.
//
// The choices are remembered.
class MidiPorts : private juce::MidiInputCallback
{
public:
    static constexpr const char* virtualName = "Doom-404";

    MidiPorts (EmulatorLink& l, juce::PropertiesFile& s) : link (l), settings (s)
    {
        setInput (settings.getValue ("midiIn"));
        setOutput (settings.getValue ("midiOut"));
        setVirtual (settings.getBoolValue ("midiVirtual", true));
    }

    ~MidiPorts() override
    {
        if (input != nullptr)
            input->stop();
        if (virtualIn != nullptr)
            virtualIn->stop();
        winPort.reset();
    }

    juce::String inputId() const { return input != nullptr ? input->getIdentifier() : juce::String(); }
    juce::String outputId() const
    {
        const juce::ScopedLock sl (outLock);
        return output != nullptr ? output->getIdentifier() : juce::String();
    }

    // Empty: none.
    void setInput (const juce::String& id)
    {
        if (input != nullptr)
            input->stop();
        input.reset();
        if (id.isNotEmpty())
            if ((input = juce::MidiInput::openDevice (id, this)) != nullptr)
                input->start();
        settings.setValue ("midiIn", inputId());
        settings.saveIfNeeded();
    }

    void setOutput (const juce::String& id)
    {
        auto out = id.isNotEmpty() ? juce::MidiOutput::openDevice (id) : nullptr;
        {
            const juce::ScopedLock sl (outLock);
            output = std::move (out);
        }
        settings.setValue ("midiOut", outputId());
        settings.saveIfNeeded();
    }

    // The "Doom-404" port: on or off (remembered).
    void setVirtual (bool on)
    {
        if (virtualIn != nullptr)
            virtualIn->stop();
        virtualIn.reset();
        {
            const juce::ScopedLock sl (outLock);
            virtualOut.reset();
            winPort.reset();
        }
        if (on)
        {
#if JUCE_WINDOWS
            auto port = std::make_unique<WinMidiPort> (juce::String (virtualName).toWideCharPointer(),
                                                       Storage::dataDir().getFullPathName().toWideCharPointer(),
                                                       [this] (const uint8_t* bytes, size_t n)
                                                       {
                                                           fromPort (juce::MidiMessage (bytes, (int) n), usbmidi::usbCable);
                                                       });
            const juce::ScopedLock sl (outLock);
            winPort = std::move (port);
#else
            if ((virtualIn = juce::MidiInput::createNewDevice (virtualName, this)) != nullptr)
                virtualIn->start();
            auto out = juce::MidiOutput::createNewDevice (virtualName);
            const juce::ScopedLock sl (outLock);
            virtualOut = std::move (out);
#endif
        }
        settings.setValue ("midiVirtual", on);
        settings.saveIfNeeded();
    }

    bool wantsVirtual() const { return settings.getBoolValue ("midiVirtual", true); }

    // How the port is doing, for the menu: empty when it is up (or off).
    juce::String virtualStatus() const
    {
        if (! wantsVirtual())
            return {};
#if JUCE_WINDOWS
        const juce::ScopedLock sl (outLock);
        if (winPort == nullptr)
            return "off";
        switch (winPort->state())
        {
            case WinMidiPort::State::ready:    return {};
            case WinMidiPort::State::starting:
                // Microsoft's issue #1047: after an earlier virtual device the
                // service can hang until it is restarted (or Windows is).
                return winPort->secondsStarting() > 10.0 ? "Windows MIDI Services is not answering: restart it or Windows"
                                                         : "starting";
            case WinMidiPort::State::failed:   return juce::String (winPort->why());
        }
        return {};
#else
        return virtualIn != nullptr ? juce::String() : juce::String ("not available");
#endif
    }

    // The unit's MIDI out (link thread): the OUT jack's packets to the
    // chosen output, the USB ones to the Doom-404 port.
    void unitMidi (const uint8_t* packet)
    {
        const int cables = packet[0] >> 4;
        juce::MidiMessage m;
        if ((cables & usbmidi::dinBit) && dinDecoder.add (packet, m))
        {
            const juce::ScopedLock sl (outLock);
            if (output != nullptr)
                output->sendMessageNow (m);
        }
        if ((cables & usbmidi::usbBit) && usbDecoder.add (packet, m))
        {
            const juce::ScopedLock sl (outLock);
            if (virtualOut != nullptr)
                virtualOut->sendMessageNow (m);
            if (winPort != nullptr)
                winPort->send (m.getRawData(), (size_t) m.getRawDataSize());
        }
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        fromPort (m, source == virtualIn.get() ? usbmidi::usbCable : usbmidi::dinCable);
    }

    void fromPort (const juce::MidiMessage& m, uint8_t cable)
    {
        usbmidi::encode (m, cable, [this] (const usbmidi::Packet& p) { link.sendBmc (p.data()); });
    }

    EmulatorLink& link;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::MidiInput> input, virtualIn;
    std::unique_ptr<juce::MidiOutput> output, virtualOut;
    std::unique_ptr<WinMidiPort> winPort;
    juce::CriticalSection outLock;
    usbmidi::Decoder dinDecoder, usbDecoder;
};
