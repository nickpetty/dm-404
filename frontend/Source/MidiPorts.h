#pragma once

#include "EmulatorLink.h"
#include "UsbMidi.h"

// The unit's MIDI IN and OUT jacks, as the computer's MIDI ports (a
// hardware interface, or loopMIDI and the like): what arrives at the chosen
// input plays the unit; what the unit sends to its MIDI OUT goes to the
// chosen output. The choices are remembered.
class MidiPorts : private juce::MidiInputCallback
{
public:
    MidiPorts (EmulatorLink& l, juce::PropertiesFile& s) : link (l), settings (s)
    {
        setInput (settings.getValue ("midiIn"));
        setOutput (settings.getValue ("midiOut"));
    }

    ~MidiPorts() override
    {
        if (input != nullptr)
            input->stop();
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

    // The unit's MIDI out (link thread): the packets for its OUT jack.
    void unitMidi (const uint8_t* packet)
    {
        if (((packet[0] >> 4) & usbmidi::dinBit) == 0)
            return;
        juce::MidiMessage m;
        if (! decoder.add (packet, m))
            return;
        const juce::ScopedLock sl (outLock);
        if (output != nullptr)
            output->sendMessageNow (m);
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        usbmidi::encode (m, usbmidi::dinCable, [this] (const usbmidi::Packet& p) { link.sendBmc (p.data()); });
    }

    EmulatorLink& link;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    juce::CriticalSection outLock;
    usbmidi::Decoder decoder;
};
