#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

// Runs the emulator core (qemu-system-arm with the sp404mk2 machine) as a
// child process and speaks its frontend link over TCP. See
// core/qemu/hw/arm/sp404/sp404-link.c for the protocol.
class EmulatorLink : private juce::Thread
{
public:
    struct Paths
    {
        juce::File qemu, firmware, flash, emmc, sd;
        bool sdInserted = true;         // start with the card in the slot
    };

    // The data folder's files (Storage.h).
    static Paths defaultPaths();

    EmulatorLink();
    ~EmulatorLink() override;

    // Starts (or restarts) the emulator. Returns an error message, or empty.
    juce::String start (const Paths& paths);
    void stop();
    bool isConnected() const { return connected.load(); }

    // Panel input.
    void sendKey (int row, int col, bool pressed);
    void sendKnob (int adc, int channel, int mux, int value);
    void sendBmc (const uint8_t packet[4]);
    void sendEncoder (int detents);
    // The unit's inputs: 48 kHz stereo. Safe on the audio thread: it only
    // queues the frames; a separate thread sends them to the emulator.
    void sendAudioIn (const float* left, const float* right, int frames);
    // USB audio from the computer (the DAW plugin): 48 kHz stereo s16.
    void sendUsbAudio (const int16_t* lr, int frames);
    // USB MIDI from the computer, each packet due at a frame of the unit's
    // output since this start (it reaches the firmware just before then).
    struct UsbMidiEvent
    {
        uint32_t frame;
        uint8_t packet[4];
    };
    void sendUsbMidi (const std::vector<UsbMidiEvent>&);
    // Counts emulator starts (connections): frame counts begin again at each.
    uint32_t getStartCount() const { return startCount.load(); }

    // The unit's output as it arrives (48 kHz stereo s16), on the link
    // thread, before the volume: what the unit sends over USB; with the
    // number of frames since this start, after these.
    std::function<void (const int16_t* lr, int frames, uint32_t framesAfter)> onAudioOut;
    // The separate buses (DRY, BUS 1, BUS 2 before the master effects, as
    // 6 s16 a frame), each block right after its onAudioOut, while asked for.
    void sendBusesWanted (bool on);
    std::function<void (const int16_t* buses, int frames)> onBuses;
    // The unit's MIDI out (a USB-MIDI packet whose cable bits say where:
    // UsbMidi.h), stamped with the output frame it happened at; link thread.
    std::function<void (uint32_t frame, const uint8_t* packet)> onUnitMidi;

    // Gain applied to the emulator's audio: the unit's VOLUME knob is an
    // analog pot after the DAC, which the firmware never sees.
    std::atomic<float> outputGain { 8.0f };

    // The latest screen: 128x64, one bit per pixel, 16 bytes a row.
    using Screen = std::array<uint8_t, 1024>;
    Screen getScreen() const;
    uint32_t getScreenCount() const { return screenCount.load(); }

    // Audio from the emulator, 48 kHz stereo; pulled by the audio callback.
    // Returns the number of frames written (the rest is left untouched).
    int readAudio (float* left, float* right, int frames);
    int audioBacklog() const { return audioFifo.getNumReady(); }

    // Packets the firmware sent the BMC, for LED and MIDI decoding.
    std::function<void (const uint8_t* packet)> onBmcPacket;

    // The SD slot: take the card out (an empty file) or put an image in.
    // The answer comes to onSdCard, on the link thread: whether a card is
    // in, and an error (empty if it worked).
    void sendSdCard (const juce::File& image);
    std::function<void (bool inserted, const juce::String& error)> onSdCard;

private:
    void run() override;
    void handleMessage (uint8_t type, const uint8_t* data, int len);
    void send (uint8_t type, const void* data, int len);

    juce::ChildProcess process;
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection sendLock;
    std::atomic<bool> connected { false };
    std::atomic<uint32_t> startCount { 0 };
    uint32_t unitFrames = 0;            // output frames received since this start

    mutable juce::SpinLock screenLock;
    Screen screen {};
    std::atomic<uint32_t> screenCount { 0 };

    static constexpr int fifoFrames = 48000;
    juce::AbstractFifo audioFifo { fifoFrames };
    std::vector<int16_t> audioBuf = std::vector<int16_t> (fifoFrames * 2);

    int port = 5404;

    // Input audio waiting to be sent (s16 stereo), and the thread sending it.
    static constexpr int inFifoFrames = 48000;
    juce::AbstractFifo inFifo { inFifoFrames };
    std::vector<int16_t> inBuf = std::vector<int16_t> (inFifoFrames * 2);
    juce::WaitableEvent inReady;
    class InputSender : public juce::Thread
    {
    public:
        explicit InputSender (EmulatorLink& l) : juce::Thread ("emulator input"), link (l) {}
        void run() override;
        EmulatorLink& link;
    };
    InputSender inputSender { *this };
};
