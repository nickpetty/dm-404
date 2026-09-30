#pragma once

#include "EmulatorLink.h"
#include "UsbMidi.h"

// The unit's MIDI, as the computer's MIDI ports.
//
// - Its MIDI IN and OUT jacks: a chosen input and output (a hardware
//   interface, or loopMIDI and the like). What arrives at the input plays
//   the unit (cable 9); what the unit sends to its OUT jack goes out.
// - Its USB MIDI: a virtual port pair the app makes itself, "Doom-404",
//   that other programs see as a MIDI device (cable 8 in, the USB bit out).
//   macOS and Linux make these for any app; Windows cannot yet (its MIDI
//   Services SDK, which can, is not shipping), so there it is absent and
//   the jacks or the DAW plugin are the way in.
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

    // The "Doom-404" virtual port pair: on or off. Returns whether it is on
    // (false where the system cannot make one).
    bool setVirtual (bool on)
    {
        if (virtualIn != nullptr)
            virtualIn->stop();
        virtualIn.reset();
        {
            const juce::ScopedLock sl (outLock);
            virtualOut.reset();
        }
        if (on)
        {
            if ((virtualIn = juce::MidiInput::createNewDevice (virtualName, this)) != nullptr)
                virtualIn->start();
            auto out = juce::MidiOutput::createNewDevice (virtualName);
            const juce::ScopedLock sl (outLock);
            virtualOut = std::move (out);
        }
        settings.setValue ("midiVirtual", on);
        settings.saveIfNeeded();
        return hasVirtual();
    }

    bool hasVirtual() const { return virtualIn != nullptr; }
    bool wantsVirtual() const { return settings.getBoolValue ("midiVirtual", true); }

    // Whether this system can make virtual ports at all.
    static bool virtualSupported()
    {
#if JUCE_WINDOWS
        return false;
#else
        return true;
#endif
    }

    // The unit's MIDI out (link thread): the OUT jack's packets to the
    // chosen output, the USB ones to the virtual port.
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
        }
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        const auto cable = source == virtualIn.get() ? usbmidi::usbCable : usbmidi::dinCable;
        usbmidi::encode (m, cable, [this] (const usbmidi::Packet& p) { link.sendBmc (p.data()); });
    }

    EmulatorLink& link;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::MidiInput> input, virtualIn;
    std::unique_ptr<juce::MidiOutput> output, virtualOut;
    juce::CriticalSection outLock;
    usbmidi::Decoder dinDecoder, usbDecoder;
};
