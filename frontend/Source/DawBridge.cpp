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
    at (shared->toUnit.read).store (at (shared->toUnit.write).load());
    at (shared->midiToUnit.read).store (at (shared->midiToUnit.write).load());
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
}

void DawBridge::unitOutput (const int16_t* lr, int frames)
{
    // Only while a plugin reads it (it would only fill up otherwise; if the
    // plugin stalls, what does not fit is dropped).
    if (shared != nullptr && pluginConnected())
        write (shared->fromUnit, lr, (uint32_t) frames);
}

void DawBridge::run()
{
    // The plugin's audio, on to the emulator in small packets as it comes.
    std::vector<int16_t> buf (1024 * 2);
    while (! threadShouldExit())
    {
        wait (2);
        if (! pluginConnected() || ! link.isConnected())
        {
            // Nothing to hear it: keep the ring from holding stale audio.
            at (shared->toUnit.read).store (at (shared->toUnit.write).load());
            continue;
        }
        uint32_t n;
        while ((n = read (shared->toUnit, buf.data(), 1024)) > 0)
            link.sendUsbAudio (buf.data(), (int) n);
    }
}
