#pragma once

#include "DawLink.h"
#include "EmulatorLink.h"

// The app's end of the DAW link (DawLink.h): the unit's output goes to the
// plugin, the plugin's audio goes to the unit's USB audio input.
class DawBridge : private juce::Thread,
                  private juce::Timer
{
public:
    explicit DawBridge (EmulatorLink&);
    ~DawBridge() override;

    // The unit's output (48 kHz stereo s16), from the link thread.
    void unitOutput (const int16_t* lr, int frames);

    // A plugin instance holds the link and is running.
    bool pluginConnected() const;

private:
    void run() override;
    void timerCallback() override;

    EmulatorLink& link;
    dawlink::Map map;
    dawlink::Shared* shared = nullptr;
};
