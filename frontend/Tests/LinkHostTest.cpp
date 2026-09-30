// Loads the Doom-404 Link VST3 as a DAW would and runs it in real time
// against the running app, at 44.1 kHz (so the rate conversion is in play),
// feeding it a tone. Prints, each second, what the plugin says and the
// level of what it returns.
//
//   LinkHostTest "path/to/Doom-404 Link.vst3" [SECONDS [EDITOR.png]]
#include <JuceHeader.h>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2)
    {
        std::printf ("usage: LinkHostTest PLUGIN.vst3 [SECONDS]\n");
        return 2;
    }
    const double seconds = argc > 2 ? std::atof (argv[2]) : 10.0;
    const double rate = 44100.0;
    const int block = 512;

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, juce::String (argv[1]));
    if (found.isEmpty())
    {
        std::printf ("no plugin in %s\n", argv[1]);
        return 1;
    }
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*found[0], rate, block, error);
    if (plugin == nullptr)
    {
        std::printf ("could not load: %s\n", error.toRawUTF8());
        return 1;
    }
    std::printf ("loaded %s, latency %d samples\n", plugin->getName().toRawUTF8(), 0);
    plugin->setPlayConfigDetails (2, 2, rate, block);
    plugin->prepareToPlay (rate, block);
    std::printf ("latency after prepare: %d samples (%.1f ms)\n", plugin->getLatencySamples(),
                 plugin->getLatencySamples() * 1000.0 / rate);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    double phase = 0.0, peak = 0.0, sum = 0.0;
    int64_t n = 0, silentBlocks = 0, blocks = 0;
    int second = 0;
    for (int64_t done = 0; done < (int64_t) (seconds * rate); done += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float v = 0.25f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / rate;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        plugin->processBlock (buf, midi);
        float blockPeak = 0.0f;
        for (int c = 0; c < 2; ++c)
            blockPeak = juce::jmax (blockPeak, buf.getMagnitude (c, 0, block));
        peak = juce::jmax (peak, (double) blockPeak);
        sum += buf.getRMSLevel (0, 0, block);
        silentBlocks += blockPeak == 0.0f;
        ++blocks;
        n += block;
        if (n >= (second + 1) * (int64_t) rate)
        {
            ++second;
            std::printf ("%2ds  peak %.3f  mean rms %.4f  silent blocks %lld/%lld\n", second, peak, sum / blocks,
                         (long long) silentBlocks, (long long) blocks);
            peak = sum = 0.0;
            silentBlocks = blocks = 0;
        }
        // Real time: wait for this block's slot.
        const double due = start + (done + block) * 1000.0 / rate;
        while (juce::Time::getMillisecondCounterHiRes() < due)
            juce::Thread::sleep (1);
    }
    // LinkHostTest PLUGIN SECONDS EDITOR.png: the plugin's window, as it
    // looks at the end of the run.
    if (argc > 3)
        if (std::unique_ptr<juce::AudioProcessorEditor> ed { plugin->createEditorIfNeeded() })
        {
            const juce::File out { juce::String (argv[3]) };
            out.deleteFile();
            juce::FileOutputStream os (out);
            juce::PNGImageFormat().writeImageToStream (ed->createComponentSnapshot (ed->getLocalBounds()), os);
        }
    plugin->releaseResources();
    return 0;
}
