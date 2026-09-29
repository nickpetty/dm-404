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

    // Laid out from a photo of the unit: positions are pixel centres in a
    // 570 x 915 image of the top panel, scaled into panel units.
    auto at = [&add] (juce::String name, T type, float cx, float cy, float w, float h)
    {
        const float sx = panelW / 570.0f, sy = panelH / 915.0f;
        add (name, type, cx * sx - w * sx / 2, cy * sy - h * sy / 2, w * sx, h * sy);
    };

    // Knobs.
    at ("VOLUME", T::knob, 100, 106, 62, 62);
    at ("CTRL 1", T::knob, 222, 106, 62, 62);
    at ("CTRL 2", T::knob, 345, 106, 62, 62);
    at ("CTRL 3", T::knob, 470, 106, 62, 62);

    // Effect keys around the display.
    at ("FILTER+DRIVE", T::button, 125, 207, 90, 40);
    at ("RESONATOR", T::button, 110, 260, 90, 40);
    at ("DELAY", T::button, 123, 315, 90, 40);
    at ("ISOLATOR", T::button, 447, 207, 90, 40);
    at ("DJFX LOOPER", T::button, 453, 260, 90, 40);
    at ("MFX", T::button, 447, 315, 90, 40);

    // Pattern sequencer and sample edit, with the VALUE encoder.
    const char* row1[] = { "PATTERN SELECT", "PATTERN EDIT", "RECORD SETTING", "START/END", "PITCH/SPEED", "MARK" };
    const float row1x[] = { 85, 145, 205, 285, 345, 405 };
    for (int i = 0; i < 6; ++i)
        at (row1[i], T::button, row1x[i], 400, 50, 30);
    at ("VALUE", T::knob, 483, 393, 48, 48);

    // Sampling and sample mode.
    const char* row2[] = { "DEL", "REC", "RESAMPLE", "BPM SYNC", "GATE", "LOOP", "REVERSE", "ROLL" };
    const float row2x[] = { 85, 145, 205, 264, 309, 361, 423, 483 };
    for (int i = 0; i < 8; ++i)
        at (row2[i], T::button, row2x[i], 454, row2x[i] > 300 && i > 4 ? 52.0f : 44.0f, 30);

    // EXIT, COPY, REMAIN, the banks and SHIFT.
    const char* row3[] = { "EXIT", "COPY", "REMAIN", "A/F", "B/G", "C/H", "D/I", "E/J", "SHIFT" };
    const float row3x[] = { 85, 145, 205, 264, 309, 355, 400, 445, 491 };
    for (int i = 0; i < 9; ++i)
        at (row3[i], T::button, row3x[i], 507, i < 3 ? 50.0f : 36.0f, 30);

    // The right-hand column beside the pads.
    at ("BUS FX", T::button, 472, 571, 72, 62);
    at ("HOLD", T::button, 472, 651, 72, 62);
    at ("EXT SOURCE", T::button, 472, 733, 72, 62);
    at ("SUB PAD", T::button, 472, 813, 72, 62);

    // The 16 pads: 1-4 along the top, 13-16 along the bottom.
    const float padX[] = { 97, 191, 285, 378 }, padY[] = { 585, 665, 747, 827 };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            at (juce::String (r * 4 + c + 1), T::pad, padX[c], padY[r] - 4, 78, 62);

    addAndMakeVisible (oled);
    // What the unit prints under its keys and on its pads.
    const std::pair<const char*, const char*> subs[] = {
        { "VOLUME", "MIN        MAX" }, { "CTRL 1", "CUTOFF" }, { "CTRL 2", "RESONANCE" }, { "CTRL 3", "DRIVE" },
        { "PATTERN SELECT", "UNDO" }, { "START/END", "CHOP" }, { "PITCH/SPEED", "ENVELOPE" },
        { "LOOP", "PING-PONG" }, { "ROLL", "ROLL SET" }, { "EXIT", "PATTERN STOP" },
        { "REMAIN", "CURRENT PAD" }, { "BUS FX", "MUTE BUS" }, { "HOLD", "PAUSE" },
        { "EXT SOURCE", "INPUT SETTING" }, { "SUB PAD", "PROJECT" },
        { "1", "FIXED VELOCITY" }, { "2", "16 VELOCITY" }, { "3", "CUE" }, { "4", "CHROMATIC" },
        { "5", "EXCHANGE" }, { "6", "INIT PARAM" }, { "7", "PAD LINK" }, { "8", "MUTE GROUPS" },
        { "9", "METRONOME" }, { "10", "COUNT-IN" }, { "11", "TAP TEMPO" }, { "12", "GAIN" },
        { "13", "UTILITY" }, { "14", "IMPORT/EXPORT" }, { "15", "PAD SETTING" }, { "16", "EFX SETTING" } };
    const char* legends[] = { "BEND+", "BPM+", "BEND+", "BPM+", "BEND-", "BPM-", "BEND-", "BPM-",
                              "|<<", "SYNC", "|<<", "SYNC", ">/II", "CUE", ">/II", "CUE" };
    for (auto& c : controls)
    {
        for (auto& [name, sub] : subs)
            if (c.name == name)
                c.sub = sub;
        if (c.type == T::pad)
            c.legend = legends[c.name.getIntValue() - 1];
    }

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
        {
            const auto& cv = v[juce::Identifier (c.name)];
            c.binding = Binding::fromVar (cv);
            if (cv.hasProperty ("led"))
                c.led = cv["led"];
        }
}

void PanelComponent::saveBindings() const
{
    auto* o = new juce::DynamicObject();
    for (auto& c : controls)
        if (c.binding.kind != Binding::Kind::none || c.led >= 0)
        {
            auto v = c.binding.toVar();
            if (c.led >= 0)
                v.getDynamicObject()->setProperty ("led", c.led);
            o->setProperty (c.name, v);
        }
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

void PanelComponent::setLedState (int page, int index, int value)
{
    juce::ignoreUnused (page);
    if (index >= 0 && index < (int) leds.size() && leds[(size_t) index] != value)
    {
        leds[(size_t) index] = (uint8_t) value;
        repaint();
    }
}

juce::Rectangle<float> PanelComponent::toScreen (juce::Rectangle<float> r) const
{
    const float sx = getWidth() / panelW, sy = getHeight() / panelH;
    return { r.getX() * sx, r.getY() * sy, r.getWidth() * sx, r.getHeight() * sy };
}

void PanelComponent::resized()
{
    // The OLED sits in a round window: 128x64 at the unit's proportions.
    oled.setBounds (toScreen ({ 34.2f, 40.2f, 31.6f, 18.8f }).toNearestInt());
}

void PanelComponent::paint (juce::Graphics& g)
{
    g.fillAll (body());
    // The round display window and the dark band behind the knobs.
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (toScreen ({ 10, 11, 80, 12 }), 8.0f);
    auto disc = toScreen ({ 30.0f, 27.0f, 40.0f, 0 });
    disc.setHeight (disc.getWidth());
    g.fillEllipse (disc);
    g.setColour (juce::Colour (0xff111214));
    g.fillRoundedRectangle (toScreen ({ 11, 30, 20, 30 }), 10.0f);
    g.fillRoundedRectangle (toScreen ({ 69, 30, 20, 30 }), 10.0f);

    // Section titles printed on the panel (photo pixel coordinates).
    const float ux = getWidth() / 570.0f, uy = getHeight() / 915.0f;
    auto title = [&] (const char* text, float x0, float x1, float y)
    {
        auto t = juce::Rectangle<float> (x0 * ux, y * uy, (x1 - x0) * ux, 12.0f * uy);
        g.setColour (ink().withAlpha (0.8f));
        g.setFont (juce::FontOptions (t.getHeight() * 0.85f));
        g.drawText (text, t, juce::Justification::centred);
        g.drawLine (t.getX(), t.getBottom(), t.getRight(), t.getBottom(), 1.0f);
    };
    title ("PATTERN SEQUENCER", 60, 230, 368);
    title ("SAMPLE EDIT", 260, 430, 368);
    title ("PUSH ENTER", 450, 516, 356);
    title ("SAMPLING", 120, 230, 425);
    title ("SAMPLE MODE", 395, 512, 425);
    title ("BANK", 392, 470, 478);
    title ("DJ MODE", 380, 470, 521);
    title ("CH1", 60, 235, 535);
    title ("CH2", 245, 370, 535);

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
                // Knob names are printed above the knob, as on the unit;
                // VALUE's below it.
                g.drawText (c.name, c.name == "VALUE" ? r.withY (r.getBottom()).withHeight (r.getHeight() * 0.3f)
                                                      : r.withY (r.getY() - r.getHeight() * 0.3f).withHeight (r.getHeight() * 0.3f),
                            juce::Justification::centred);
                break;
            }
            case PanelControl::Type::button:
            {
                g.setColour (c.pressed ? juce::Colour (0xff5b5e66) : juce::Colour (0xff34363c));
                g.fillRoundedRectangle (r, 3.0f);
                if (c.led >= 0 && leds[(size_t) c.led] != 0)
                {
                    // A lit button: its LED glows through the key, faintly
                    // at the backlight level, fully when active.
                    const float level = leds[(size_t) c.led] / 255.0f;
                    const bool red = c.name == "REC" || c.name == "RESAMPLE" || c.name == "DEL";
                    g.setColour ((red ? juce::Colour (0xffff2a3a) : accent()).withAlpha (0.12f + 0.78f * level));
                    g.fillRoundedRectangle (r, 3.0f);
                }
                g.setColour (ink());
                g.setFont (juce::FontOptions (juce::jmin (r.getHeight() * 0.36f, r.getWidth() * 0.19f)));
                g.drawFittedText (c.name.replace ("/", "/ ").replace ("+", "+ "), r.reduced (2.0f).toNearestInt(),
                                  juce::Justification::centred, 2, 0.8f);
                break;
            }
            case PanelControl::Type::pad:
            {
                g.setColour (c.pressed ? juce::Colour (0xff5b5e66) : juce::Colour (0xff3b3d44));
                g.fillRoundedRectangle (r, 5.0f);
                const int base = (c.name.getIntValue() - 1) * 3;
                if (base >= 0 && base + 2 < 0x30)
                {
                    const auto rgb = juce::Colour (leds[(size_t) base], leds[(size_t) base + 1], leds[(size_t) base + 2]);
                    if (rgb.getBrightness() > 0.0f)
                    {
                        // The pad's LEDs light it from inside.
                        g.setColour (rgb.withAlpha (0.9f));
                        g.fillRoundedRectangle (r.reduced (2.0f), 5.0f);
                    }
                }
                g.setColour (juce::Colour (0xff55585f));
                g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 1.5f);
                // The number top right and the DJ-mode legend in a box, as
                // printed on the unit's pads.
                g.setColour (juce::Colour (0xffffa53a));
                g.setFont (juce::FontOptions (r.getHeight() * 0.3f, juce::Font::bold));
                g.drawText (c.name, r.reduced (r.getWidth() * 0.1f, r.getHeight() * 0.06f), juce::Justification::topRight);
                if (c.legend.isNotEmpty())
                {
                    auto box = juce::Rectangle<float> (r.getX() + r.getWidth() * 0.1f, r.getY() + r.getHeight() * 0.58f,
                                                       r.getWidth() * 0.38f, r.getHeight() * 0.17f);
                    g.drawRoundedRectangle (box, 2.0f, 1.0f);
                    g.setFont (juce::FontOptions (box.getHeight() * 0.75f, juce::Font::bold));
                    g.drawText (c.legend, box, juce::Justification::centred);
                }
                break;
            }
        }
        if (c.sub.isNotEmpty())
        {
            // The SHIFT function, printed under the key (boxed under pads).
            auto s = r.withY (r.getBottom() + r.getHeight() * 0.06f).withHeight (juce::jmax (9.0f, getHeight() / panelH * 2.6f));
            g.setColour (ink().withAlpha (0.85f));
            g.setFont (juce::FontOptions (s.getHeight() * 0.8f));
            g.drawFittedText (c.sub, s.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            if (c.type == PanelControl::Type::pad)
                g.drawRect (s.reduced (2.0f, 0.0f), 1.0f);
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

void PanelComponent::press (PanelControl& c, bool down, float velocity)
{
    c.pressed = down;
    const auto& b = c.binding;
    if (b.kind == Binding::Kind::key)
        link.sendKey (b.row, b.col, down);
    else if (b.kind == Binding::Kind::bmc)
        link.sendBmc (down ? b.down : b.up);
    else if (b.kind == Binding::Kind::analog)
    {
        // Pads are pressure sensors read inverted: released reads full
        // scale, a hard hit near zero. The firmware gets the velocity from
        // the pressure at the first scan after the hit.
        const int pressure = down ? juce::jlimit (200, 3900, (int) (velocity * 3900.0f)) : 0;
        link.sendKnob (b.adc, b.channel, b.mux, 4095 - pressure);
    }
    repaint();
}

void PanelComponent::setKnob (PanelControl& c, float v)
{
    c.value = juce::jlimit (0.0f, 1.0f, v);
    if (c.name == "VOLUME")
        link.outputGain = c.value * c.value * 32.0f;    // up to +30 dB
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
    if (c->name == "VALUE")
    {
        // An endless encoder: dragging turns it, a click pushes it.
        encoderSent = 0;
        encoderMoved = false;
        return;
    }
    if (c->type == PanelControl::Type::knob)
        dragStartValue = c->value;
    else
    {
        // Pads: clicking nearer the top hits harder.
        auto r = toScreen (c->bounds);
        const float v = 1.0f - (e.position.y - r.getY()) / juce::jmax (1.0f, r.getHeight());
        press (*c, true, 0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, v));
    }
}

void PanelComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (active != nullptr && active->name == "VALUE")
    {
        const int detents = -e.getDistanceFromDragStartY() / 12;
        if (detents != encoderSent)
        {
            link.sendEncoder (detents - encoderSent);
            active->value = std::fmod (active->value + 0.04f * (detents - encoderSent) + 10.0f, 1.0f);
            encoderSent = detents;
            encoderMoved = true;
            repaint();
        }
        return;
    }
    if (active != nullptr && active->type == PanelControl::Type::knob)
        setKnob (*active, dragStartValue - e.getDistanceFromDragStartY() / 200.0f);
}

void PanelComponent::mouseUp (const juce::MouseEvent&)
{
    if (active != nullptr && active->name == "VALUE")
    {
        if (! encoderMoved && active->binding.kind == Binding::Kind::analog)
        {
            // The push switch is read like a pad: pressed, then released.
            const auto& b = active->binding;
            link.sendKnob (b.adc, b.channel, b.mux, 0);
            juce::Timer::callAfterDelay (120, [this, b] { link.sendKnob (b.adc, b.channel, b.mux, 4095); });
        }
        active = nullptr;
        return;
    }
    if (active != nullptr && active->type != PanelControl::Type::knob)
        press (*active, false);
    active = nullptr;
}
