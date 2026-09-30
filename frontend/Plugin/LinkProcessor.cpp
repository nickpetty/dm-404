#include "LinkProcessor.h"
#include "LinkEditor.h"

using namespace dawlink;

LinkProcessor::LinkProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    const auto db = juce::NormalisableRange<float> (-24.0f, 36.0f, 0.1f);
    addParameter (outputDb = new juce::AudioParameterFloat (juce::ParameterID { "output", 1 }, "Output level", db, 18.0f,
                                                            juce::AudioParameterFloatAttributes().withLabel ("dB")));
    addParameter (inputDb = new juce::AudioParameterFloat (juce::ParameterID { "input", 1 }, "Input level", db, 0.0f,
                                                           juce::AudioParameterFloatAttributes().withLabel ("dB")));
    timerCallback();
    startTimerHz (4);
}

void LinkProcessor::getStateInformation (juce::MemoryBlock& out)
{
    juce::XmlElement x ("Doom404Link");
    x.setAttribute ("output", outputDb->get());
    x.setAttribute ("input", inputDb->get());
    copyXmlToBinary (x, out);
}

void LinkProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        *outputDb = (float) x->getDoubleAttribute ("output", 18.0);
        *inputDb = (float) x->getDoubleAttribute ("input", 0.0);
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
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            || l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            || l.getMainInputChannelSet().isDisabled());
}

void LinkProcessor::prepareToPlay (double sampleRate, int maxBlock)
{
    rate = sampleRate;
    setLatencySamples ((int) std::lround (targetFrames * sampleRate / dawlink::rate));
    // Room for blocks up to twice the promised size (some hosts overshoot).
    const int block = maxBlock * 2;
    const int maxAt48 = (int) std::ceil (block * dawlink::rate / sampleRate * 1.01) + 16;
    stageL.assign ((size_t) maxAt48 + targetFrames * 4, 0.0f);
    stageR.assign (stageL.size(), 0.0f);
    raw.assign (stageL.size() * 2, 0);
    inStageL.assign ((size_t) block * 2 + 16, 0.0f);
    inStageR.assign (inStageL.size(), 0.0f);
    conv[0].assign ((size_t) maxAt48 * 2, 0.0f);
    conv[1].assign (conv[0].size(), 0.0f);
    rawIn.assign (conv[0].size() * 2, 0);
    outL.reset();
    outR.reset();
    inL.reset();
    inR.reset();
    staged = inStaged = 0;
    steer = 0.0;
    primed = false;
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

void LinkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    auto* s = shared.load();

    if (s == nullptr || at (s->magic).load() != magic || at (s->version).load() != version
        || ! fresh (at (s->appAlive).load()))
    {
        state = State::noApp;
        buffer.clear();
        primed = false;
        return;
    }
    if (! claim (*s))
    {
        state = State::otherInstance;
        buffer.clear();
        return;
    }
    if (isNonRealtime())
    {
        state = State::offline;
        buffer.clear();
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
    if (! fromUnit (buffer, n))
        buffer.clear();
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

bool LinkProcessor::fromUnit (juce::AudioBuffer<float>& buffer, int n)
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
        outL.reset();
        outR.reset();
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
    const double ratio = dawlink::rate / rate * (1.0 + steer);

    const int needed = (int) std::ceil (n * ratio) + 4;
    if (staged < needed)
    {
        const int want48 = juce::jmin (needed - staged, (int) stageL.size() - staged);
        const auto got = (int) read (ring, raw.data(), (uint32_t) want48);
        for (int i = 0; i < got; ++i)
        {
            stageL[(size_t) (staged + i)] = raw[(size_t) i * 2] / 32768.0f;
            stageR[(size_t) (staged + i)] = raw[(size_t) i * 2 + 1] / 32768.0f;
        }
        staged += got;
    }
    if (staged < needed)
    {
        // Ran dry: silence until the cushion is back.
        ++underruns;
        primed = false;
        return false;
    }
    const int used = outL.process (ratio, stageL.data(), buffer.getWritePointer (0), n, staged, 0);
    outR.process (ratio, stageR.data(), buffer.getWritePointer (1), n, staged, 0);
    buffer.applyGain (0, n, juce::Decibels::decibelsToGain (outputDb->get()));
    std::memmove (stageL.data(), stageL.data() + used, sizeof (float) * (size_t) (staged - used));
    std::memmove (stageR.data(), stageR.data() + used, sizeof (float) * (size_t) (staged - used));
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
