#pragma once

// The link between the Doom-404 app and its DAW plugin: a file both map
// into memory (daw-link.shm in the data folder), holding lock-free rings.
//
//   toUnit    the DAW's audio into the unit (USB audio in): plugin writes,
//             app reads and sends it to the emulator
//   fromUnit  the unit's output (USB audio out), 8 channels: the main mix,
//             then DRY, BUS 1 and BUS 2 before the master effects (the
//             plugin's extra outputs): app writes, plugin reads
//   midiToUnit    MIDI into the unit: plugin writes, app sends it on
//   midiFromUnit  the unit's MIDI out: app writes, plugin reads
//
// Audio is 48 kHz stereo, 16-bit, the unit's own rate; the plugin converts
// to and from the DAW's rate. MIDI is USB-MIDI packets (UsbMidi.h), each
// stamped with the frame of the audio ring it belongs with (the toUnit
// frame it is due at, the fromUnit frame it happened at), so it keeps its
// timing against the audio both ways. Each ring has one writer and one
// reader, and free-running 32-bit indices. The app stamps appAlive, shares
// its VOLUME gain, and bumps session when it (re)starts; a plugin instance
// claims the link (pluginOwner) and stamps pluginAlive, so only one
// instance at a time carries the unit.

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>

namespace dawlink
{
    constexpr uint32_t magic = 0x34303444;      // "D404"
    constexpr uint32_t version = 3;
    constexpr uint32_t rate = 48000;
    constexpr uint32_t ringFrames = 32768;      // 0.68 s; a power of two
    constexpr uint32_t midiPackets = 4096;
    constexpr int64_t staleMs = 1000;           // a heartbeat older than this: gone
    // MIDI to the unit is stamped this far (20 ms) beyond the plugin's
    // output cushion, so it arrives before the unit gets there despite the
    // cushion's wander and the trip: part of the plugin's latency.
    constexpr uint32_t midiLead = 960;

    // Interleaved 16-bit frames of `Ch` channels.
    template <int Ch>
    struct AudioRing
    {
        static constexpr int channels = Ch;
        uint32_t write, read;
        uint32_t pad[14];
        int16_t data[ringFrames * Ch];
    };

    // The unit's side: main L/R, DRY L/R, BUS 1 L/R, BUS 2 L/R.
    constexpr int unitChannels = 8;

    struct MidiEvent
    {
        uint32_t frame;                         // audio ring frame it goes with
        uint8_t packet[4];                      // USB-MIDI
    };

    struct MidiRing
    {
        uint32_t write, read;
        uint32_t pad[14];
        MidiEvent data[midiPackets];
    };

    struct Shared
    {
        uint32_t magic, version, session, rate;
        int64_t appAlive, pluginAlive;          // juce::Time::currentTimeMillis()
        uint64_t pluginOwner;                   // the instance holding the link, 0 = none
        float volume;                           // the app's VOLUME, as a gain on the unit's output
        uint32_t pad[7];
        AudioRing<2> toUnit;
        AudioRing<unitChannels> fromUnit;
        MidiRing midiToUnit, midiFromUnit;
    };

    template <typename T>
    inline std::atomic_ref<T> at (T& v) { return std::atomic_ref<T> (v); }

    inline juce::File file()
    {
#if JUCE_WINDOWS
        auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
#else
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#endif
        return base.getChildFile ("Doom-404").getChildFile ("daw-link.shm");
    }

    // The mapping. The app creates the file; the plugin only opens it.
    class Map
    {
    public:
        bool open (bool create)
        {
            auto f = file();
            if (create && f.getSize() != (juce::int64) sizeof (Shared))
            {
                f.getParentDirectory().createDirectory();
                f.deleteFile();
                juce::FileOutputStream os (f);
                if (! os.openedOk())
                    return false;
                std::vector<char> zeros (sizeof (Shared));
                os.write (zeros.data(), zeros.size());
            }
            if (f.getSize() != (juce::int64) sizeof (Shared))
                return false;
            mapped = std::make_unique<juce::MemoryMappedFile> (f, juce::MemoryMappedFile::readWrite, false);
            if (mapped->getData() == nullptr || mapped->getSize() < sizeof (Shared))
            {
                mapped.reset();
                return false;
            }
            return true;
        }

        Shared* get() const { return mapped != nullptr ? static_cast<Shared*> (mapped->getData()) : nullptr; }

    private:
        std::unique_ptr<juce::MemoryMappedFile> mapped;
    };

    // Ring helpers: frames ready to read, room to write, and the copies.
    template <typename Ring>
    inline uint32_t ready (Ring& r) { return at (r.write).load (std::memory_order_acquire) - at (r.read).load (std::memory_order_relaxed); }
    template <typename Ring>
    inline uint32_t room (Ring& r) { return ringFrames - (at (r.write).load (std::memory_order_relaxed) - at (r.read).load (std::memory_order_acquire)); }

    // Writes up to `frames` interleaved frames; returns how many fit.
    template <typename Ring>
    inline uint32_t write (Ring& r, const int16_t* d, uint32_t frames)
    {
        constexpr int ch = Ring::channels;
        frames = juce::jmin (frames, room (r));
        const uint32_t w = at (r.write).load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i)
            std::memcpy (&r.data[((w + i) & (ringFrames - 1)) * ch], d + i * ch, sizeof (int16_t) * ch);
        at (r.write).store (w + frames, std::memory_order_release);
        return frames;
    }

    // Reads up to `frames`; returns how many were there.
    template <typename Ring>
    inline uint32_t read (Ring& r, int16_t* d, uint32_t frames)
    {
        constexpr int ch = Ring::channels;
        frames = juce::jmin (frames, ready (r));
        const uint32_t rd = at (r.read).load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i)
            std::memcpy (d + i * ch, &r.data[((rd + i) & (ringFrames - 1)) * ch], sizeof (int16_t) * ch);
        at (r.read).store (rd + frames, std::memory_order_release);
        return frames;
    }

    // The reader jumps so that `keep` frames remain (after a stall, or to
    // start at a set latency).
    template <typename Ring>
    inline void skipTo (Ring& r, uint32_t keep)
    {
        const uint32_t w = at (r.write).load (std::memory_order_acquire);
        const uint32_t rd = at (r.read).load (std::memory_order_relaxed);
        if (w - rd > keep)
            at (r.read).store (w - keep, std::memory_order_release);
    }

    inline bool fresh (int64_t stamp)
    {
        return juce::Time::currentTimeMillis() - stamp < staleMs;
    }

    // MIDI: one event in (false when full), and the oldest out, in order.
    inline bool push (MidiRing& r, const MidiEvent& e)
    {
        const uint32_t w = at (r.write).load (std::memory_order_relaxed);
        if (w - at (r.read).load (std::memory_order_acquire) >= midiPackets)
            return false;
        r.data[w & (midiPackets - 1)] = e;
        at (r.write).store (w + 1, std::memory_order_release);
        return true;
    }

    inline const MidiEvent* peek (MidiRing& r)
    {
        const uint32_t rd = at (r.read).load (std::memory_order_relaxed);
        if (at (r.write).load (std::memory_order_acquire) == rd)
            return nullptr;
        return &r.data[rd & (midiPackets - 1)];
    }

    inline void pop (MidiRing& r)
    {
        at (r.read).fetch_add (1, std::memory_order_release);
    }

    inline void drain (MidiRing& r)
    {
        at (r.read).store (at (r.write).load (std::memory_order_acquire), std::memory_order_release);
    }

    // Frame numbers wrap: a is before b.
    inline bool before (uint32_t a, uint32_t b) { return (int32_t) (a - b) < 0; }
}
