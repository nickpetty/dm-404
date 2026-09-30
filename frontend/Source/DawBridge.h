#pragma once

#include "DawLink.h"
#include "EmulatorLink.h"
#include "UsbMidi.h"

// The app's end of the DAW link (DawLink.h): the unit's output and USB MIDI
// out go to the plugin; the plugin's audio and MIDI go to the unit's USB
// input. The app's VOLUME goes along, for the plugin's output level.
class DawBridge : private juce::Thread,
                  private juce::Timer
{
public:
    explicit DawBridge (EmulatorLink&);
    ~DawBridge() override;

    // From the link thread: the unit's output (48 kHz stereo s16), with the
    // emulator's frame count after it, and its USB MIDI out, stamped with
    // that count.
    void unitOutput (const int16_t* lr, int frames, uint32_t framesAfter);
    void unitBuses (const int16_t* buses, int frames);
    void unitMidi (uint32_t frame, const uint8_t* packet);

    // A plugin instance holds the link and is running.
    bool pluginConnected() const;

private:
    void run() override;
    void timerCallback() override;

    EmulatorLink& link;
    dawlink::Map map;
    dawlink::Shared* shared = nullptr;
    std::atomic<uint32_t> fromDelta { 0 };      // fromUnit ring frame = emulator output frame + this
    void flushPending (const int16_t* buses);
    // The main block waiting for its buses (link thread).
    int16_t pending[1024 * 2];
    int pendingFrames = 0;
    std::atomic<bool> busesWanted { false };
    uint32_t busesStart = 0;
};
