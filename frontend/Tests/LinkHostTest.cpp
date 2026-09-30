// Loads the DM-404 Link VST3 as a DAW would and runs it in real time
// against the running app, at 44.1 kHz (so the rate conversion is in play),
// feeding it a tone. Prints, each second, what the plugin says and the
// level of what it returns.
//
//   LinkHostTest PLUGIN.vst3 [SECONDS [EDITOR.png]] [--notes] [--play BPM]
//
// --notes: a note-on for pad 1 (note 47, channel 1) every 2 s, and when the
//   unit's audio starts after it (how late, against the reported latency).
// --play BPM: a transport playing at that tempo from 4 s in (MIDI clock).
// MIDI the plugin returns (the unit's MIDI out) is printed with where its
// audio starts.
#include <JuceHeader.h>
#include "../Source/DawLink.h"

namespace
{
    struct FakeHead : juce::AudioPlayHead
    {
        double bpm = 120.0, rate = 44100.0;
        int64_t sample = 0, startAt = -1;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm (bpm);
            const bool playing = startAt >= 0 && sample >= startAt;
            p.setIsPlaying (playing);
            p.setPpqPosition (playing ? (sample - startAt) / rate * bpm / 60.0 : 0.0);
            return p;
        }
    };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);
    const bool notes = args.contains ("--notes");
    FakeHead head;
    if (const int i = args.indexOf ("--play"); i >= 0 && i + 1 < args.size())
    {
        head.bpm = args[i + 1].getDoubleValue();
        head.startAt = 4 * 44100;
        args.removeRange (i, 2);
    }
    args.removeString ("--notes");
    if (args.isEmpty())
    {
        std::printf ("usage: LinkHostTest PLUGIN.vst3 [SECONDS [EDITOR.png]] [--notes] [--play BPM]\n");
        return 2;
    }
    const double seconds = args.size() > 1 ? args[1].getDoubleValue() : 10.0;
    const double rate = 44100.0;
    const int block = 512;

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, args[0]);
    if (found.isEmpty())
    {
        std::printf ("no plugin in %s\n", args[0].toRawUTF8());
        return 1;
    }
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*found[0], rate, block, error);
    if (plugin == nullptr)
    {
        std::printf ("could not load: %s\n", error.toRawUTF8());
        return 1;
    }
    plugin->setPlayHead (&head);
    // Every output on: the main one and the separate buses.
    auto layout = plugin->getBusesLayout();
    for (int i = 1; i < layout.outputBuses.size(); ++i)
        layout.outputBuses.getReference (i) = juce::AudioChannelSet::stereo();
    if (! plugin->setBusesLayout (layout))
        std::printf ("could not turn on the extra outputs\n");
    const int outs = plugin->getTotalNumOutputChannels();
    std::printf ("%d output buses, %d channels\n", plugin->getBusCount (false), outs);
    plugin->prepareToPlay (rate, block);
    const int latency = plugin->getLatencySamples();
    std::printf ("latency %d samples (%.1f ms)\n", latency, latency * 1000.0 / rate);

    // The link itself, to watch the plugin's cushion (unit frames on hand).
    dawlink::Map map;
    map.open (false);
    int minFill = 1 << 30, maxFill = 0;

    juce::AudioBuffer<float> buf (juce::jmax (2, outs), block);
    std::vector<float> busPeak ((size_t) buf.getNumChannels() / 2, 0.0f);
    juce::MidiBuffer midi;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    double phase = 0.0, peak = 0.0, sum = 0.0;
    int64_t n = 0, silentBlocks = 0, blocks = 0;
    int second = 0;
    int64_t noteAt = -1, midiOutAt = -1;
    int64_t lastLoud = -1000000;
    for (int64_t done = 0; done < (int64_t) (seconds * rate); done += block)
    {
        head.sample = done;
        for (int i = 0; i < block; ++i)
        {
            const float v = 0.0f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / rate;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        midi.clear();
        if (notes && done > 0 && (done / block) % (int) (2 * rate / block) == 0)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 47, (juce::uint8) 110), 100);
            midi.addEvent (juce::MidiMessage::noteOff (1, 47), 400);
            noteAt = done + 100;
        }
        plugin->processBlock (buf, midi);
        if (auto* s = map.get())
        {
            const int fill = (int) dawlink::ready (s->fromUnit);
            minFill = juce::jmin (minFill, fill);
            maxFill = juce::jmax (maxFill, fill);
        }
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())
                midiOutAt = done + meta.samplePosition;
            if (! m.isMidiClock())
                std::printf ("   MIDI out at %.3f s: %s\n", (done + meta.samplePosition) / rate,
                             m.getDescription().toRawUTF8());
        }
        // Onsets: the first loud sample after a quiet spell.
        for (int i = 0; i < block; ++i)
            if (std::abs (buf.getSample (0, i)) > 0.02f)
            {
                const int64_t t = done + i;
                if (t - lastLoud > (int64_t) (rate * 0.25))
                {
                    if (noteAt >= 0)
                        std::printf ("   note in at %.3f s -> audio at %.3f s: %.1f ms after (latency %.1f ms)\n",
                                     noteAt / rate, t / rate, (t - noteAt) * 1000.0 / rate, latency * 1000.0 / rate);
                    if (midiOutAt >= 0)
                        std::printf ("   unit's note-on MIDI at %.3f s, its audio at %.3f s: %+.1f ms\n",
                                     midiOutAt / rate, t / rate, (t - midiOutAt) * 1000.0 / rate);
                    noteAt = midiOutAt = -1;
                }
                lastLoud = t;
            }
        float blockPeak = 0.0f;
        for (int c = 0; c < 2; ++c)
            blockPeak = juce::jmax (blockPeak, buf.getMagnitude (c, 0, block));
        for (size_t b = 0; b < busPeak.size(); ++b)
            busPeak[b] = juce::jmax (busPeak[b], buf.getMagnitude ((int) b * 2, 0, block),
                                     buf.getMagnitude ((int) b * 2 + 1, 0, block));
        peak = juce::jmax (peak, (double) blockPeak);
        sum += buf.getRMSLevel (0, 0, block);
        silentBlocks += blockPeak == 0.0f;
        ++blocks;
        n += block;
        if (n >= (second + 1) * (int64_t) rate)
        {
            ++second;
            std::printf ("%2ds  peak %.3f  mean rms %.4f  silent blocks %lld/%lld  ring %.1f-%.1f ms\n", second, peak,
                         sum / blocks, (long long) silentBlocks, (long long) blocks, minFill / 48.0, maxFill / 48.0);
            minFill = 1 << 30;
            maxFill = 0;
            if (busPeak.size() > 1)
            {
                std::printf ("     outputs (main, DRY, BUS 1, BUS 2):");
                for (auto& p : busPeak)
                    std::printf (" %.3f", p), p = 0.0f;
                std::printf ("\n");
            }
            peak = sum = 0.0;
            silentBlocks = blocks = 0;
        }
        // Real time: wait for this block's slot.
        const double due = start + (done + block) * 1000.0 / rate;
        while (juce::Time::getMillisecondCounterHiRes() < due)
            juce::Thread::sleep (1);
    }
    // LinkHostTest PLUGIN SECONDS EDITOR.png: the plugin's window.
    if (args.size() > 2)
        if (std::unique_ptr<juce::AudioProcessorEditor> ed { plugin->createEditorIfNeeded() })
        {
            const juce::File out { args[2] };
            out.deleteFile();
            juce::FileOutputStream os (out);
            juce::PNGImageFormat().writeImageToStream (ed->createComponentSnapshot (ed->getLocalBounds()), os);
        }
    plugin->releaseResources();
    return 0;
}
