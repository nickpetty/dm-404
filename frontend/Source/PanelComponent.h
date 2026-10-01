#pragma once

#include "EmulatorLink.h"
#include "MidiLearn.h"

// What a panel control does in the emulated hardware. Which physical
// control sits where in the key matrix, on which analog input, or which
// BMC packet a pad sends is still being worked out, so bindings are data
// (panel.json), set by "learn": with the debug drawer open, right-click a
// control, "Learn hardware binding", then use the matching raw input in the
// drawer. (Right-click's "MIDI learn" is MidiLearn.h's, for MIDI devices.)
struct Binding
{
    enum class Kind { none, key, analog, bmc };
    Kind kind = Kind::none;
    int row = 0, col = 0;               // key
    int adc = 0, channel = 0, mux = 0;  // analog
    uint8_t down[4] {}, up[4] {};       // bmc: packets on press / release

    juce::var toVar() const;
    static Binding fromVar (const juce::var&);
    juce::String describe() const;
};

struct PanelControl
{
    enum class Type { button, pad, knob };
    juce::String name;
    Type type;
    juce::Rectangle<float> bounds;      // in panel units (0-100 x 0-160)
    Binding binding;
    int led = -1;                       // button LED index ("01 00 idx v")
    juce::String sub;                   // printed under it (the SHIFT function)
    juce::String legend;                // printed on a pad (its DJ-mode role)
    bool pressed = false;
    bool latched = false;               // ctrl-clicked: held until clicked again
    float value = 0.5f;                 // knobs, 0-1
};

class OledView : public juce::Component
{
public:
    void setScreen (const EmulatorLink::Screen& s) { screen = s; repaint(); }
    void paint (juce::Graphics&) override;

private:
    EmulatorLink::Screen screen {};
};

class PanelComponent : public juce::Component,
                       public juce::TooltipClient,
                       private juce::Timer
{
public:
    // Hover tips on how to hold a key or turn VALUE with the mouse.
    juce::String getTooltip() override;

    explicit PanelComponent (EmulatorLink& link);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    OledView& getOled() { return oled; }

    // Learn: the control waiting for a binding, if any.
    PanelControl* getLearning() { return learning; }
    void learn (const Binding&);
    // Offer "Learn hardware binding" on right-click (the debug drawer is open).
    void setBindingLearn (bool on)
    {
        bindingLearn = on;
        if (! on)
            learning = nullptr;
        repaint();
    }

    // MIDI learn (MidiLearn.h): right-click asks to start it for a control
    // (empty: stop) or to forget a control's mappings; midiMapping says what
    // a control answers to. The panel shows the control learning and, for
    // a moment, what it learnt.
    std::function<void (const juce::String& control)> onMidiLearn, onMidiForget;
    std::function<juce::String (const juce::String& control)> midiMapping;
    void setMidiLearning (const juce::String& control);
    void showMidiLearnt (const juce::String& control, const juce::String& what);
    // A mapped MIDI message: works the control as the mouse would. Keys and
    // pads are held while the note (or a controller at 64 or more) is;
    // pads take the note's velocity; knobs follow a controller; VALUE
    // turns with a controller and pushes with a note.
    void fromMidi (const juce::String& control, bool note, int value, MidiLearn::Mode mode);

    // An LED packet from the firmware: "01 page idx value". Every page
    // writes the same LED and the latest write wins; the page is how it
    // shows: 0 lit (a key, a playing pad), 1 the resting level (0x1f: the
    // backlight; pads: a dim colour), 6 blinking between that value and the
    // resting one, or off if that is as bright (the current pad and option
    // keys in START/END, BUS FX while choosing, the pads in pattern select,
    // a bank key on its second bank), 9 pulsing slowly (MARK
    // once skip back has something), 7 pulsing faster. Pages 4 and 5 come
    // once at boot and are not LEDs (ignored); others are taken as lit.
    void setLedState (int page, int index, int value);

    // SHIFT: a click latches it (the firmware sees it held until the next
    // click, or until the next key or pad has been pressed and let go); the
    // computer's Shift key holds it for as long as it is down.
    // Other keys latch on ctrl-click, and the mouse wheel turns the knob
    // under the pointer, also while another control is held.
    void setShiftFromKeyboard (bool down);

    // Tap a control by its printed name (for scripted tests).
    void tap (const juce::String& name);

    // A picture stretched over the panel in place of its body and
    // backdrops (an invalid image: the plain panel), and the colour of the
    // text printed on the panel.
    void setCustomBackground (const juce::Image& image)
    {
        background = image;
        repaint();
    }
    bool hasCustomBackground() const { return background.isValid(); }
    juce::Colour getTextColour() const { return textColour; }
    void setTextColour (juce::Colour c) { textColour = c; repaint(); }
    static juce::Colour defaultTextColour() { return juce::Colour (defaultText); }

    static juce::File bindingsFile();
    void loadBindings();
    void saveBindings() const;

private:
    juce::Rectangle<float> toScreen (juce::Rectangle<float>) const;
    juce::Rectangle<float> displayDisc() const;
    PanelControl* hit (juce::Point<float>);
    void press (PanelControl&, bool down, float velocity = 1.0f);
    void setKnob (PanelControl&, float v);

    EmulatorLink& link;
    OledView oled;
    std::vector<PanelControl> controls;
    PanelControl* active = nullptr;
    PanelControl* learning = nullptr;
    float dragStartValue = 0;
    int encoderSent = 0;
    bool shiftLatched = false, shiftKeyboard = false, shiftMidi = false;
    bool bindingLearn = false;
    juce::String midiLearning, midiMessage;     // learning; what was learnt, shown a while
    uint32_t midiMessageUntil = 0;
    std::map<juce::String, int> lastMidiValue;  // VALUE from an absolute knob
    void updateShift();
    void releaseShiftLatch();
    bool encoderMoved = false;
    float wheelAccum = 0.0f;
    // LED levels as the firmware sends them to the BMC, "01 00 idx value":
    // 0x00-0x2f the pads as RGB triplets, 0x30 on the buttons.
    std::array<uint8_t, 128> leds {};
    // Steady LED writes not yet shown: the value, when the first and the
    // latest came (ms).
    std::array<uint8_t, 128> ledNext {};
    std::array<bool, 128> ledPending {};
    std::array<uint32_t, 128> ledFirst {}, ledLast {};
    void commitLeds();
    // The sample mode keys, BPM SYNC to ROLL (photo pixels): left edge,
    // width, gap.
    static constexpr float modeLeft = 242.0f, modeW = 46.0f, modeGap = (267.0f - 5 * 46.0f) / 4.0f;
    // Animated LEDs: how (page 6 blink, 9 slow pulse, 7 fast pulse) and the
    // value they swing to from their resting one.
    enum class LedMode : uint8_t { steady, blink, pulseSlow, pulseFast };
    std::array<uint8_t, 128> animValue {};
    std::array<LedMode, 128> ledMode {};
    double animTime = 0.0;          // seconds, advanced by the timer
    uint8_t shown (int idx) const;
    static constexpr juce::uint32 defaultText = 0xffd8d8d0;
    juce::Image background;
    juce::Colour textColour { defaultText };
    void timerCallback() override;
};
