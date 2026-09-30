#pragma once

#include "../Source/DawLink.h"
#include "../Source/UsbMidi.h"

// Doom-404 Link: the SP-404MKII emulator in a DAW track. The track's audio
// and MIDI go into the unit (as USB audio and MIDI from a computer would),
// and the unit's output and MIDI out come back, from the running Doom-404
// app over shared memory (DawLink.h). MIDI keeps its place against the
// audio both ways; the DAW's transport can drive the unit as MIDI clock.
//
// The unit runs on its own clock, so the plugin keeps a cushion of its
// output (reported as latency, for delay compensation) and steers its rate
// converters by up to 0.5% to hold it, both ways. It cannot render faster
// than real time: offline bounces come out silent.
class LinkProcessor : public juce::AudioProcessor,
                      private juce::Timer
{
public:
    LinkProcessor();
    ~LinkProcessor() override;

    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Doom-404 Link"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Levels: the unit's output follows the app's VOLUME knob, then this
    // trim; the input goes into the unit as it is, then this level.
    juce::AudioParameterFloat* outputDb = nullptr;
    juce::AudioParameterFloat* inputDb = nullptr;
    // MIDI clock, Song Position and Start/Continue/Stop from the DAW's
    // transport, so the unit can follow its tempo (with its sync setting).
    juce::AudioParameterBool* sendClock = nullptr;

    enum class State { noApp, otherInstance, waiting, running, offline };
    State getState() const { return state.load(); }
    int getUnderruns() const { return underruns.load(); }
    double getLatencyMs() const { return (targetFrames + dawlink::midiLead) * 1000.0 / dawlink::rate; }

private:
    void timerCallback() override;
    bool claim (dawlink::Shared&);
    void toUnit (const juce::AudioBuffer<float>&, int n);
    // Fills the block from the unit; says which ring frame its first sample
    // came from, and the frames per sample.
    bool fromUnit (juce::AudioBuffer<float>&, int n, uint32_t& base, double& ratio);
    void clock (int n);

    usbmidi::Decoder fromDecoder;
    juce::MidiBuffer clockEvents;
    bool wasPlaying = false;
    double expectedPpq = 0.0;

    dawlink::Map map;
    std::atomic<dawlink::Shared*> shared { nullptr };
    const uint64_t id = (uint64_t) juce::Random::getSystemRandom().nextInt64() | 1;
    uint32_t session = 0;

    std::atomic<State> state { State::noApp };
    std::atomic<int> underruns { 0 };

    double rate = 48000.0;
    static constexpr int targetFrames = 2880;   // 60 ms at 48 kHz, as the app keeps
    double steer = 0.0;
    bool primed = false;

    // Unit to DAW: 48 kHz frames staged for the converters.
    juce::LagrangeInterpolator outL, outR;
    std::vector<float> stageL, stageR;
    int staged = 0;
    std::vector<int16_t> raw;

    // DAW to unit.
    juce::LagrangeInterpolator inL, inR;
    std::vector<float> inStageL, inStageR, conv[2];
    int inStaged = 0;
    std::vector<int16_t> rawIn;
};
