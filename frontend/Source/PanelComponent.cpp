#include "PanelComponent.h"

namespace
{
    constexpr float panelW = 100.0f, panelH = 160.0f;

    juce::Colour body() { return juce::Colour (0xff1c1d20); }
    juce::Colour ink() { return juce::Colour (0xffd8d8d0); }
    juce::Colour accent() { return juce::Colour (0xffff5a1f); }

    juce::var packetVar (const uint8_t* p)
    {
        return juce::String::toHexString (p, 4, 0);
    }

    void packetFrom (const juce::var& v, uint8_t* p)
    {
        juce::MemoryBlock mb;
        mb.loadFromHexString (v.toString());
        for (size_t i = 0; i < 4; ++i)
            p[i] = i < mb.getSize() ? (uint8_t) mb[i] : 0;
    }
}

//==============================================================================
juce::var Binding::toVar() const
{
    auto* o = new juce::DynamicObject();
    switch (kind)
    {
        case Kind::key:    o->setProperty ("key", juce::Array<juce::var> { row, col }); break;
        case Kind::analog: o->setProperty ("analog", juce::Array<juce::var> { adc, channel, mux }); break;
        case Kind::bmc:    o->setProperty ("bmcDown", packetVar (down));
                           o->setProperty ("bmcUp", packetVar (up)); break;
        case Kind::none:   break;
    }
    return o;
}

Binding Binding::fromVar (const juce::var& v)
{
    Binding b;
    if (auto* a = v["key"].getArray(); a != nullptr && a->size() == 2)
    {
        b.kind = Kind::key;
        b.row = (*a)[0];
        b.col = (*a)[1];
    }
    else if (auto* an = v["analog"].getArray(); an != nullptr && an->size() == 3)
    {
        b.kind = Kind::analog;
        b.adc = (*an)[0];
        b.channel = (*an)[1];
        b.mux = (*an)[2];
    }
    else if (v.hasProperty ("bmcDown"))
    {
        b.kind = Kind::bmc;
        packetFrom (v["bmcDown"], b.down);
        packetFrom (v["bmcUp"], b.up);
    }
    return b;
}

juce::String Binding::describe() const
{
    switch (kind)
    {
        case Kind::key:    return "key r" + juce::String (row) + " c" + juce::String (col);
        case Kind::analog: return "adc" + juce::String (adc + 1) + " ch" + juce::String (channel) + " mux" + juce::String (mux);
        case Kind::bmc:    return "bmc " + juce::String::toHexString (down, 4, 1);
        case Kind::none:   break;
    }
    return "unbound";
}

//==============================================================================
void OledView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    const float px = getWidth() / 128.0f, py = getHeight() / 64.0f;
    const juce::Colour on (0xffe8f0ff);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x)
            if (screen[(size_t) (y * 16 + x / 8)] & (0x80 >> (x & 7)))
            {
                // A soft halo under each lit pixel reads as an OLED.
                g.setColour (on.withAlpha (0.12f));
                g.fillRect (x * px - px * 0.3f, y * py - py * 0.3f, px * 1.6f, py * 1.6f);
                g.setColour (on);
                g.fillRect (x * px + px * 0.06f, y * py + py * 0.06f, px * 0.88f, py * 0.88f);
            }
}

//==============================================================================
PanelComponent::PanelComponent (EmulatorLink& l) : link (l)
{
    using T = PanelControl::Type;
    auto add = [this] (juce::String name, T type, float x, float y, float w, float h)
    {
        controls.push_back ({ name, type, { x, y, w, h } });
    };

    // Top row: the four knobs.
    add ("VOLUME", T::knob, 6, 6, 14, 14);
    add ("CTRL 1", T::knob, 30, 6, 14, 14);
    add ("CTRL 2", T::knob, 50, 6, 14, 14);
    add ("CTRL 3", T::knob, 70, 6, 14, 14);

    // Display row: VALUE encoder and the keys around the screen.
    add ("VALUE", T::knob, 80, 28, 14, 14);
    add ("EXIT", T::button, 80, 46, 14, 6);
    add ("SHIFT", T::button, 4, 28, 14, 6);
    add ("REC", T::button, 4, 36, 14, 6);
    add ("RESAMPLE", T::button, 4, 44, 14, 6);

    // Effects row.
    const char* fx[] = { "MFX", "FX 1", "FX 2", "FX 3", "FX 4", "FX 5" };
    for (int i = 0; i < 6; ++i)
        add (fx[i], T::button, 4 + i * 15.5f, 58, 13, 6);

    // Sequencer and pad-mode row.
    const char* seq[] = { "PATTERN SEL", "TR-REC", "HOLD", "ROLL", "SUB PAD", "BUS FX" };
    for (int i = 0; i < 6; ++i)
        add (seq[i], T::button, 4 + i * 15.5f, 68, 13, 6);

    // Banks A/F ... E/J.
    const char* banks[] = { "A/F", "B/G", "C/H", "D/I", "E/J" };
    for (int i = 0; i < 5; ++i)
        add (banks[i], T::button, 4 + i * 15.5f, 78, 13, 6);
    add ("MARK", T::button, 81.5f, 78, 13, 6);

    // The 16 pads, 1-4 at the top.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            add (juce::String (r * 4 + c + 1), T::pad, 5 + c * 23.0f, 90 + r * 17.0f, 20, 15);

    addAndMakeVisible (oled);
    loadBindings();
}

juce::File PanelComponent::bindingsFile()
{
    auto root = EmulatorLink::defaultPaths().firmware.getParentDirectory().getParentDirectory();
    return root.getChildFile ("frontend/panel.json");
}

void PanelComponent::loadBindings()
{
    auto v = juce::JSON::parse (bindingsFile());
    for (auto& c : controls)
        if (v.hasProperty (juce::Identifier (c.name)))
            c.binding = Binding::fromVar (v[juce::Identifier (c.name)]);
}

void PanelComponent::saveBindings() const
{
    auto* o = new juce::DynamicObject();
    for (auto& c : controls)
        if (c.binding.kind != Binding::Kind::none)
            o->setProperty (c.name, c.binding.toVar());
    bindingsFile().replaceWithText (juce::JSON::toString (juce::var (o)));
}

void PanelComponent::learn (const Binding& b)
{
    if (learning == nullptr)
        return;
    learning->binding = b;
    learning = nullptr;
    saveBindings();
    repaint();
}

void PanelComponent::setLedState (int index, int value)
{
    leds[index] = value;
    repaint();
}

juce::Rectangle<float> PanelComponent::toScreen (juce::Rectangle<float> r) const
{
    const float sx = getWidth() / panelW, sy = getHeight() / panelH;
    return { r.getX() * sx, r.getY() * sy, r.getWidth() * sx, r.getHeight() * sy };
}

void PanelComponent::resized()
{
    oled.setBounds (toScreen ({ 22, 27, 54, 27 }).toNearestInt());
}

void PanelComponent::paint (juce::Graphics& g)
{
    g.fillAll (body());
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (toScreen ({ 21, 26, 56, 29 }), 4.0f);

    for (auto& c : controls)
    {
        auto r = toScreen (c.bounds);
        const bool learningThis = &c == learning;
        switch (c.type)
        {
            case PanelControl::Type::knob:
            {
                auto k = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()),
                                                  juce::jmin (r.getWidth(), r.getHeight()));
                g.setColour (juce::Colour (0xff2e3036));
                g.fillEllipse (k);
                g.setColour (juce::Colour (0xff45474e));
                g.drawEllipse (k.reduced (1.5f), 2.0f);
                const float a = juce::MathConstants<float>::pi * (1.25f - 1.5f * c.value);
                auto centre = k.getCentre();
                const float rad = k.getWidth() * 0.4f;
                g.setColour (ink());
                g.drawLine (centre.x, centre.y, centre.x + rad * std::cos (a), centre.y - rad * std::sin (a), 2.5f);
                g.setFont (juce::FontOptions (r.getHeight() * 0.22f));
                g.drawText (c.name, r.withY (r.getBottom()).withHeight (r.getHeight() * 0.3f),
                            juce::Justification::centred);
                break;
            }
            case PanelControl::Type::button:
            {
                g.setColour (c.pressed ? juce::Colour (0xff5b5e66) : juce::Colour (0xff34363c));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (ink());
                g.setFont (juce::FontOptions (r.getHeight() * 0.42f));
                g.drawText (c.name, r, juce::Justification::centred);
                break;
            }
            case PanelControl::Type::pad:
            {
                g.setColour (c.pressed ? accent() : juce::Colour (0xff3b3d44));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (juce::Colour (0xff55585f));
                g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 1.5f);
                g.setColour (ink().withAlpha (0.6f));
                g.setFont (juce::FontOptions (r.getHeight() * 0.25f));
                g.drawText (c.name, r.reduced (6.0f), juce::Justification::topLeft);
                break;
            }
        }
        if (c.binding.kind == Binding::Kind::none || learningThis)
        {
            // Unbound controls show a dim corner mark; the one learning glows.
            g.setColour (learningThis ? accent() : juce::Colour (0x40ff5a1f));
            g.drawRoundedRectangle (r.expanded (2.0f), 4.0f, learningThis ? 2.5f : 1.0f);
        }
    }
}

PanelControl* PanelComponent::hit (juce::Point<float> p)
{
    for (auto& c : controls)
        if (toScreen (c.bounds).contains (p))
            return &c;
    return nullptr;
}

void PanelComponent::press (PanelControl& c, bool down)
{
    c.pressed = down;
    const auto& b = c.binding;
    if (b.kind == Binding::Kind::key)
        link.sendKey (b.row, b.col, down);
    else if (b.kind == Binding::Kind::bmc)
        link.sendBmc (down ? b.down : b.up);
    repaint();
}

void PanelComponent::setKnob (PanelControl& c, float v)
{
    c.value = juce::jlimit (0.0f, 1.0f, v);
    if (c.binding.kind == Binding::Kind::analog)
    {
        // The firmware reads knobs inverted (0xfff - value).
        link.sendKnob (c.binding.adc, c.binding.channel, c.binding.mux,
                       4095 - (int) std::round (c.value * 4095.0f));
    }
    repaint();
}

void PanelComponent::mouseDown (const juce::MouseEvent& e)
{
    auto* c = hit (e.position);
    if (c == nullptr)
        return;
    if (e.mods.isPopupMenu())
    {
        learning = (learning == c) ? nullptr : c;
        repaint();
        return;
    }
    active = c;
    if (c->type == PanelControl::Type::knob)
        dragStartValue = c->value;
    else
        press (*c, true);
}

void PanelComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (active != nullptr && active->type == PanelControl::Type::knob)
        setKnob (*active, dragStartValue - e.getDistanceFromDragStartY() / 200.0f);
}

void PanelComponent::mouseUp (const juce::MouseEvent&)
{
    if (active != nullptr && active->type != PanelControl::Type::knob)
        press (*active, false);
    active = nullptr;
}
