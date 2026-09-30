#include "LinkProcessor.h"
#include "LinkEditor.h"

using namespace dawlink;

LinkProcessor::LinkProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("DRY", juce::AudioChannelSet::stereo(), false)
                                             .withOutput ("BUS 1", juce::AudioChannelSet::stereo(), false)
                                             .withOutput ("BUS 2", juce::AudioChannelSet::stereo(), false))
{
    const auto db = juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f);
    addParameter (outputDb = new juce::AudioParameterFloat (juce::ParameterID { "trim", 1 }, "Output trim", db, 0.0f,
                                                            juce::AudioParameterFloatAttributes().withLabel ("dB")));
    addParameter (inputDb = new juce::AudioParameterFloat (juce::ParameterID { "input", 1 }, "Input level", db, 0.0f,
                                                           juce::AudioParameterFloatAttributes().withLabel ("dB")));
    addParameter (sendClock = new juce::AudioParameterBool (juce::ParameterID { "clock", 1 }, "Send MIDI clock", true));
    timerCallback();
    startTimerHz (4);
}

void LinkProcessor::getStateInformation (juce::MemoryBlock& out)
{
    juce::XmlElement x ("DM404Link");
    x.setAttribute ("trim", outputDb->get());
    x.setAttribute ("input", inputDb->get());
    x.setAttribute ("clock", sendClock->get());
    copyXmlToBinary (x, out);
}

void LinkProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        *outputDb = (float) x->getDoubleAttribute ("trim", 0.0);
        *inputDb = (float) x->getDoubleAttribute ("input", 0.0);
        *sendClock = x->getBoolAttribute ("clock", true);
    }
}

LinkProcessor::~LinkProcessor()
{
    stopTimer();
    releaseResources();
}

void LinkProcessor::timerCallback()
{
    // The app makes the file; until it has, keep looking. Once mapped, the
    // mapping stays (the app reuses the file when it restarts).
    if (shared.load() == nullptr && map.open (false))
        shared = map.get();
}

bool LinkProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    for (int i = 1; i < l.outputBuses.size(); ++i)
        if (! l.outputBuses[i].isDisabled() && l.outputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            || l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            || l.getMainInputChannelSet().isDisabled());
}

void LinkProcessor::prepareToPlay (double sampleRate, int maxBlock)
{
    rate = sampleRate;
    // The cushion and MIDI's lead: what the DAW compensates, so a note's
    // sound lands where the note was written.
    setLatencySamples ((int) std::lround ((targetFrames + dawlink::midiLead) * sampleRate / dawlink::rate));
    // Room for blocks up to twice the promised size (some hosts overshoot).
    const int block = maxBlock * 2;
    const int maxAt48 = (int) std::ceil (block * dawlink::rate / sampleRate * 1.01) + 16;
    for (auto& s : stage)
        s.assign ((size_t) maxAt48 + targetFrames * 4, 0.0f);
    raw.assign (stage[0].size() * unitCh, 0);
    inStageL.assign ((size_t) block * 2 + 16, 0.0f);
    inStageR.assign (inStageL.size(), 0.0f);
    conv[0].assign ((size_t) maxAt48 * 2, 0.0f);
    conv[1].assign (conv[0].size(), 0.0f);
    rawIn.assign (conv[0].size() * 2, 0);
    for (auto& c : outConv)
        c.reset();
    inL.reset();
    inR.reset();
    staged = inStaged = 0;
    steer = 0.0;
    primed = false;
    wasPlaying = false;
    clockEvents.ensureSize (4096);
}

void LinkProcessor::releaseResources()
{
    if (auto* s = shared.load())
    {
        uint64_t mine = id;
        at (s->pluginOwner).compare_exchange_strong (mine, 0);
    }
}

bool LinkProcessor::claim (Shared& s)
{
    auto owner = at (s.pluginOwner);
    uint64_t current = owner.load();
    if (current != id)
    {
        // Free, or its holder went quiet (removed, crashed, bypassed).
        if (current != 0 && fresh (at (s.pluginAlive).load()))
            return false;
        if (! owner.compare_exchange_strong (current, id))
            return false;
        primed = false;
    }
    at (s.pluginAlive).store (juce::Time::currentTimeMillis());
    return true;
}

void LinkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    auto* s = shared.load();

    if (s == nullptr || at (s->magic).load() != magic || at (s->version).load() != version
        || ! fresh (at (s->appAlive).load()))
    {
        state = State::noApp;
        buffer.clear();
        midi.clear();
        primed = false;
        return;
    }
    if (! claim (*s))
    {
        state = State::otherInstance;
        buffer.clear();
        midi.clear();
        return;
    }
    if (isNonRealtime())
    {
        state = State::offline;
        buffer.clear();
        midi.clear();
        primed = false;
        return;
    }
    if (const uint32_t sess = at (s->session).load(); sess != session)
    {
        // The app restarted: start over.
        session = sess;
        primed = false;
    }

    toUnit (buffer, n);

    uint32_t base = 0;
    double ratio = 0.0;
    const bool running = fromUnit (buffer, n, base, ratio);
    if (! running)
    {
        // Not playing the unit yet: MIDI still goes, about as soon as it can.
        base = at (s->fromUnit.write).load() - (uint32_t) targetFrames;
        ratio = dawlink::rate / rate;
    }

    // MIDI in, stamped with the unit's output frame it should sound at: the
    // one this plugin plays a latency after the event. It reaches the
    // firmware just before the unit makes that frame (midiLead ahead of
    // where the unit is, give or take the cushion's wander).
    auto stamp = [&] (int sample)
    {
        return base + (uint32_t) std::lround (juce::jlimit (0, n, sample) * ratio) + (uint32_t) targetFrames
             + dawlink::midiLead;
    };
    auto send = [&] (const juce::MidiMessage& m, int sample)
    {
        usbmidi::encode (m, 0, [&] (const usbmidi::Packet& p)
        {
            push (s->midiToUnit, MidiEvent { stamp (sample), { p[0], p[1], p[2], p[3] } });
        });
    };
    for (const auto meta : midi)
        send (meta.getMessage(), meta.samplePosition);
    clockEvents.clear();
    if (sendClock->get())
        clock (n);
    for (const auto meta : clockEvents)
        send (meta.getMessage(), meta.samplePosition);
    midi.clear();

    if (running)
    {
        // The unit's MIDI out, at the samples its audio comes out at.
        while (auto* e = peek (s->midiFromUnit))
        {
            const double at48 = (double) (int32_t) (e->frame - base);
            if (at48 >= n * ratio)
                break;
            juce::MidiMessage m;
            if (fromDecoder.add (e->packet, m))
                midi.addEvent (m, juce::jlimit (0, n - 1, (int) (at48 / ratio)));
            pop (s->midiFromUnit);
        }
        // The app's VOLUME, and the trim.
        const float volume = at (s->volume).load();
        buffer.applyGain (0, n, (volume > 0.0f ? volume : 8.0f) * juce::Decibels::decibelsToGain (outputDb->get()));
    }
    else
    {
        buffer.clear();
        drain (s->midiFromUnit);
    }
}

void LinkProcessor::clock (int n)
{
    auto send = [this] (const juce::MidiMessage& m, int sample) { clockEvents.addEvent (m, sample); };
    // MIDI clock from the DAW's transport: Song Position and Continue (or
    // Start at the top) when it plays, 24 ticks a beat at their samples,
    // Stop when it stops; a jump (a loop) re-cues.
    auto* head = getPlayHead();
    const auto pos = head != nullptr ? head->getPosition() : std::nullopt;
    const bool playing = pos.hasValue() && pos->getIsPlaying() && pos->getPpqPosition().hasValue();
    if (! playing)
    {
        if (wasPlaying)
            send (juce::MidiMessage::midiStop(), 0);
        wasPlaying = false;
        return;
    }
    const double ppq = *pos->getPpqPosition();
    const double bpm = pos->getBpm().orFallback (120.0);
    const bool jumped = wasPlaying && std::abs (ppq - expectedPpq) > 1.0 / 48.0;
    if (! wasPlaying || jumped)
    {
        if (jumped)
            send (juce::MidiMessage::midiStop(), 0);
        const int sixteenths = juce::jmax (0, (int) std::floor (ppq * 4.0 + 1e-6));
        send (juce::MidiMessage::songPositionPointer (sixteenths), 0);
        send (ppq <= 1e-6 ? juce::MidiMessage::midiStart() : juce::MidiMessage::midiContinue(), 0);
    }
    const double samplesPerTick = rate * 60.0 / bpm / 24.0;
    for (double k = std::ceil (ppq * 24.0 - 1e-6);; k += 1.0)
    {
        const double at = (k / 24.0 - ppq) * 24.0 * samplesPerTick;
        if (at >= n)
            break;
        send (juce::MidiMessage::midiClock(), (int) at);
    }
    expectedPpq = ppq + n / samplesPerTick / 24.0;
    wasPlaying = true;
}

void LinkProcessor::toUnit (const juce::AudioBuffer<float>& buffer, int n)
{
    // The track's audio, converted to 48 kHz at the rate the unit's output
    // is being consumed (the same steer), so neither side drifts.
    const int chans = getTotalNumInputChannels();
    if ((size_t) (inStaged + n) > inStageL.size())
        inStaged = 0;
    n = juce::jmin (n, (int) inStageL.size());      // a block beyond the promised size
    for (int i = 0; i < n; ++i)
    {
        inStageL[(size_t) (inStaged + i)] = chans > 0 ? buffer.getSample (0, i) : 0.0f;
        inStageR[(size_t) (inStaged + i)] = chans > 1 ? buffer.getSample (1, i) : inStageL[(size_t) (inStaged + i)];
    }
    inStaged += n;

    const double ratio = rate / dawlink::rate / (1.0 + steer);
    const int produce = juce::jmin ((int) ((inStaged - 4) / ratio), (int) conv[0].size());
    if (produce <= 0)
        return;
    const int used = inL.process (ratio, inStageL.data(), conv[0].data(), produce, inStaged, 0);
    inR.process (ratio, inStageR.data(), conv[1].data(), produce, inStaged, 0);
    std::memmove (inStageL.data(), inStageL.data() + used, sizeof (float) * (size_t) (inStaged - used));
    std::memmove (inStageR.data(), inStageR.data() + used, sizeof (float) * (size_t) (inStaged - used));
    inStaged -= used;

    const float g = juce::Decibels::decibelsToGain (inputDb->get()) * 32767.0f;
    for (int i = 0; i < produce; ++i)
    {
        rawIn[(size_t) i * 2] = (int16_t) juce::jlimit (-32768.0f, 32767.0f, conv[0][(size_t) i] * g);
        rawIn[(size_t) i * 2 + 1] = (int16_t) juce::jlimit (-32768.0f, 32767.0f, conv[1][(size_t) i] * g);
    }
    write (shared.load()->toUnit, rawIn.data(), (uint32_t) produce);
}

bool LinkProcessor::fromUnit (juce::AudioBuffer<float>& buffer, int n, uint32_t& base, double& ratio)
{
    auto& ring = shared.load()->fromUnit;
    const double fill = (double) ready (ring) + staged;

    if (! primed)
    {
        // Start with the cushion in hand.
        if (fill < targetFrames)
        {
            state = State::waiting;
            return false;
        }
        skipTo (ring, (uint32_t) targetFrames);
        staged = 0;
        steer = 0.0;
        for (auto& c : outConv)
            c.reset();
        primed = true;
    }
    else if (fill > targetFrames * 4)
    {
        // Far behind (a stall): jump back to the cushion.
        skipTo (ring, (uint32_t) targetFrames);
        staged = 0;
    }
    state = State::running;

    const double want = juce::jlimit (-1.0, 1.0, (fill - targetFrames) / targetFrames) * 0.005;
    steer += (want - steer) * 0.02;
    ratio = dawlink::rate / rate * (1.0 + steer);

    const int needed = (int) std::ceil (n * ratio) + 4;
    if (staged < needed)
    {
        const int want48 = juce::jmin (needed - staged, (int) stage[0].size() - staged);
        const auto got = (int) read (ring, raw.data(), (uint32_t) want48);
        for (int i = 0; i < got; ++i)
            for (int c = 0; c < unitCh; ++c)
                stage[c][(size_t) (staged + i)] = raw[(size_t) (i * unitCh + c)] / 32768.0f;
        staged += got;
    }
    if (staged < needed)
    {
        // Ran dry: silence until the cushion is back.
        ++underruns;
        primed = false;
        return false;
    }
    // The ring frame this block's first sample comes from.
    base = at (ring.read).load() - (uint32_t) staged;
    // The main output and whichever of DRY, BUS 1, BUS 2 the DAW has on
    // (every converter runs, so a bus turned on later is in step).
    int used = 0;
    float scratch[4096];
    for (int bus = 0; bus < unitCh / 2; ++bus)
    {
        auto* out = bus < getBusCount (false) && getBus (false, bus)->isEnabled() ? getBus (false, bus) : nullptr;
        for (int c = 0; c < 2; ++c)
        {
            const int ch = out != nullptr ? out->getChannelIndexInProcessBlockBuffer (c) : -1;
            float* dest = ch >= 0 && ch < buffer.getNumChannels() && n <= 4096 ? buffer.getWritePointer (ch) : scratch;
            used = outConv[bus * 2 + c].process (ratio, stage[bus * 2 + c].data(), dest, juce::jmin (n, 4096), staged, 0);
        }
    }
    for (auto& s : stage)
        std::memmove (s.data(), s.data() + used, sizeof (float) * (size_t) (staged - used));
    staged -= used;
    return true;
}

juce::AudioProcessorEditor* LinkProcessor::createEditor()
{
    return new LinkEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LinkProcessor();
}
