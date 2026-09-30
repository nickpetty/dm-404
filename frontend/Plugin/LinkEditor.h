#pragma once

#include "LinkProcessor.h"

// Says whether the plugin is carrying the unit's audio, and if not, why.
class LinkEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    explicit LinkEditor (LinkProcessor& p) : juce::AudioProcessorEditor (p), proc (p)
    {
        for (auto* s : { &output, &input })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
            s->setTextValueSuffix (" dB");
            s->setColour (juce::Slider::thumbColourId, juce::Colour (0xffff5a1f));
            addAndMakeVisible (*s);
        }
        for (auto* l : { &outputLabel, &inputLabel })
        {
            l->setColour (juce::Label::textColourId, juce::Colour (0xffd8d8d0));
            addAndMakeVisible (*l);
        }
        clockButton.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffd8d8d0));
        clockButton.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xffff5a1f));
        addAndMakeVisible (clockButton);
        setSize (440, 290);
        startTimerHz (4);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16).removeFromBottom (128).withTrimmedBottom (28);
        auto row = r.removeFromTop (30);
        outputLabel.setBounds (row.removeFromLeft (110));
        output.setBounds (row);
        r.removeFromTop (4);
        row = r.removeFromTop (30);
        inputLabel.setBounds (row.removeFromLeft (110));
        input.setBounds (row);
        r.removeFromTop (6);
        clockButton.setBounds (r.removeFromTop (26));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1d20));
        auto r = getLocalBounds().reduced (16);
        g.setColour (juce::Colour (0xffff5a1f));
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText ("DM-404 LINK", r.removeFromTop (30), juce::Justification::left);

        juce::String head, body;
        juce::Colour dot;
        switch (proc.getState())
        {
            case LinkProcessor::State::noApp:
                head = "DM-404 is not running";
                body = "Start the DM-404 app; this plugin connects to it.";
                dot = juce::Colours::grey;
                break;
            case LinkProcessor::State::otherInstance:
                head = "Another DM-404 Link has the unit";
                body = "One instance at a time carries the unit's audio.";
                dot = juce::Colours::orange;
                break;
            case LinkProcessor::State::offline:
                head = "Offline render";
                body = "The unit plays in real time only: bounce in real time.";
                dot = juce::Colours::orange;
                break;
            case LinkProcessor::State::waiting:
                head = "Connecting...";
                body = "Waiting for the unit's audio.";
                dot = juce::Colours::yellow;
                break;
            case LinkProcessor::State::running:
                head = "Connected";
                body = "This track's audio and MIDI go into the unit; its output and MIDI out come out here.";
                dot = juce::Colour (0xff3ad06b);
                break;
        }
        r.removeFromTop (8);
        auto line = r.removeFromTop (24);
        g.setColour (dot);
        g.fillEllipse (line.removeFromLeft (14).withSizeKeepingCentre (12, 12).toFloat());
        line.removeFromLeft (8);
        g.setColour (juce::Colour (0xffd8d8d0));
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (head, line, juce::Justification::left);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText (body, r.removeFromTop (40), juce::Justification::topLeft, 2);
        g.setColour (juce::Colour (0xffd8d8d0).withAlpha (0.6f));
        g.drawText ("Latency " + juce::String (proc.getLatencyMs(), 0) + " ms (compensated)   dropouts "
                        + juce::String (proc.getUnderruns()),
                    r.removeFromBottom (20), juce::Justification::left);
    }

private:
    void timerCallback() override { repaint(); }
    LinkProcessor& proc;
    juce::Slider output, input;
    juce::Label outputLabel { {}, "Output trim" }, inputLabel { {}, "Into the unit" };
    juce::SliderParameterAttachment outputAttach { *proc.outputDb, output }, inputAttach { *proc.inputDb, input };
    juce::ToggleButton clockButton { "Send tempo and transport (MIDI clock) to the unit" };
    juce::ButtonParameterAttachment clockAttach { *proc.sendClock, clockButton };
};
