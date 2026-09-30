#pragma once

// A MIDI device of the app's own on Windows, through Windows MIDI Services
// (a virtual device: other programs see it as a MIDI port, MIDI 1.0 ones
// included). Kept apart from JUCE: plain bytes in and out.
//
// It needs the Windows MIDI Services App SDK, which Doom-404 does not ship:
// either built into Windows (as it will be), or Windows.Devices.Midi2.dll
// from Microsoft's release put in `dllDir` by the user. Otherwise state()
// says failed, and why(). It is set up on a thread of its own; if the MIDI
// service never answers, the port is let go without waiting for it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class WinMidiPort
{
public:
    enum class State { starting, ready, failed };
    // MIDI 1.0 bytes from the port (one whole message a call), on a
    // service thread.
    using Receive = std::function<void (const uint8_t* bytes, size_t n)>;

    WinMidiPort (const std::wstring& name, const std::wstring& dllDir, Receive receive);
    ~WinMidiPort();

    State state() const;
    std::string why() const;
    // Seconds since it began starting (for "not answering").
    double secondsStarting() const;

    // One MIDI 1.0 message (bytes) out of the port; dropped unless ready.
    void send (const uint8_t* bytes, size_t n);

    // Whether this build can do it at all.
    static bool available();

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};
