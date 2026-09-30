#include "DawBridge.h"

using namespace dawlink;

DawBridge::DawBridge (EmulatorLink& l) : juce::Thread ("daw link"), link (l)
{
    if (! map.open (true))
        return;
    shared = map.get();
    // A new session: the plugin sees it and starts over.
    at (shared->magic).store (magic);
    at (shared->version).store (version);
    at (shared->rate).store (rate);
    at (shared->volume).store (link.outputGain.load());
    at (shared->toUnit.read).store (at (shared->toUnit.write).load());
    drain (shared->midiToUnit);
    at (shared->session).fetch_add (1);
    at (shared->appAlive).store (juce::Time::currentTimeMillis());
    startTimerHz (20);
    startThread (juce::Thread::Priority::high);
}

DawBridge::~DawBridge()
{
    stopTimer();
    stopThread (1000);
    if (shared != nullptr)
        at (shared->appAlive).store (0);
}

bool DawBridge::pluginConnected() const
{
    return shared != nullptr && at (shared->pluginOwner).load() != 0 && fresh (at (shared->pluginAlive).load());
}

void DawBridge::timerCallback()
{
    at (shared->appAlive).store (juce::Time::currentTimeMillis());
    at (shared->volume).store (link.outputGain.load());
}

void DawBridge::unitOutput (const int16_t* lr, int frames, uint32_t framesAfter)
{
    if (shared == nullptr)
        return;
    // Only while a plugin reads it (it would only fill up otherwise; if the
    // plugin stalls, what does not fit is dropped).
    if (pluginConnected())
        write (shared->fromUnit, lr, (uint32_t) frames);
    // Where the unit's frames land in the ring, for stamping its MIDI.
    fromDelta = at (shared->fromUnit.write).load() - framesAfter;
}

void DawBridge::unitMidi (uint32_t frame, const uint8_t* packet)
{
    if (shared == nullptr || ! pluginConnected())
        return;
    MidiEvent e { frame + fromDelta.load(), { (uint8_t) (packet[0] & 0x0f), packet[1], packet[2], packet[3] } };
    push (shared->midiFromUnit, e);
}

void DawBridge::run()
{
    // The plugin's audio and MIDI, on to the emulator as they come. MIDI is
    // stamped with the fromUnit frame it should sound at; the emulator gets
    // it as its own output frame count.
    std::vector<int16_t> buf (1024 * 2);
    std::vector<EmulatorLink::UsbMidiEvent> midi;
    while (! threadShouldExit())
    {
        wait (2);
        if (shared == nullptr)
            continue;
        if (! pluginConnected() || ! link.isConnected())
        {
            // Nothing to hear it: keep the rings from holding stale input.
            at (shared->toUnit.read).store (at (shared->toUnit.write).load());
            drain (shared->midiToUnit);
            continue;
        }
        uint32_t n;
        while ((n = read (shared->toUnit, buf.data(), 1024)) > 0)
            link.sendUsbAudio (buf.data(), (int) n);
        const uint32_t delta = fromDelta.load();
        midi.clear();
        while (auto* e = peek (shared->midiToUnit))
        {
            midi.push_back ({ e->frame - delta, { (uint8_t) (usbmidi::usbCable << 4 | (e->packet[0] & 0x0f)),
                                                  e->packet[1], e->packet[2], e->packet[3] } });
            pop (shared->midiToUnit);
        }
        if (! midi.empty())
            link.sendUsbMidi (midi);
    }
}
