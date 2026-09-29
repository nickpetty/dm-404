#pragma once

#include "EmulatorLink.h"

// What a panel control does in the emulated hardware. Which physical
// control sits where in the key matrix, on which analog input, or which
// BMC packet a pad sends is still being worked out, so bindings are data
// (panel.json), set by "learn": right-click a control, then use the matching
// raw input in the debug drawer.
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
    bool pressed = false;
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

class PanelComponent : public juce::Component
{
public:
    explicit PanelComponent (EmulatorLink& link);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    OledView& getOled() { return oled; }

    // Learn: the control waiting for a binding, if any.
    PanelControl* getLearning() { return learning; }
    void learn (const Binding&);

    void setLedState (int index, int value);

    static juce::File bindingsFile();
    void loadBindings();
    void saveBindings() const;

private:
    juce::Rectangle<float> toScreen (juce::Rectangle<float>) const;
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
    bool encoderMoved = false;
    // LED levels as the firmware sends them to the BMC, "01 00 idx value":
    // 0x00-0x2f the pads as RGB triplets, 0x30 on the buttons.
    std::array<uint8_t, 128> leds {};
};
