#include "PanelComponent.h"
#include "Storage.h"
#include "BinaryData.h"

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
    startTimer (33);        // LED animation: blinks and pulses, ~30 fps
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
    // Mirror images of the right-hand keys, within the same backdrops.
    at ("FILTER+DRIVE", T::button, 123, 207, 90, 40);
    at ("RESONATOR", T::button, 117, 260, 90, 40);
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

    // The 16 pads: 1-4 along the top, 13-16 along the bottom, and the
    // right-hand column beside them, row for row.
    const float padX[] = { 97, 191, 285, 378 }, padY[] = { 585, 665, 747, 827 };
    const char* column[] = { "BUS FX", "HOLD", "EXT SOURCE", "SUB PAD" };
    for (int r = 0; r < 4; ++r)
        at (column[r], T::button, 472, padY[r] - 4, 72, 62);
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
    // In a development checkout, the repo's copy (learn writes it); the
    // app carries the same file built in.
    auto root = Storage::devRoot();
    return root == juce::File() ? juce::File() : root.getChildFile ("frontend/panel.json");
}

void PanelComponent::loadBindings()
{
    auto file = bindingsFile();
    auto v = file.existsAsFile() ? juce::JSON::parse (file)
                                 : juce::JSON::parse (juce::String::fromUTF8 (BinaryData::panel_json, BinaryData::panel_jsonSize));
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
    if (bindingsFile() == juce::File())
        return;
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
    if (index < 0 || index >= (int) leds.size())
        return;
    const auto i = (size_t) index;
    if (page == 4 || page == 5)
        return;
    if (page == 6 || page == 7 || page == 9)
    {
        // Swings between this and the value it had.
        ledMode[i] = page == 6 ? LedMode::blink : page == 9 ? LedMode::pulseSlow : LedMode::pulseFast;
        animValue[i] = (uint8_t) value;
    }
    else
    {
        ledMode[i] = LedMode::steady;
        leds[i] = (uint8_t) value;
    }
    repaint();
}

uint8_t PanelComponent::shown (int idx) const
{
    const auto i = (size_t) idx;
    float from = leds[i];
    const float to = animValue[i];
    // A blink goes dark between flashes when its resting level is no dimmer
    // (a bank key already lit, blinking for its second bank).
    if (ledMode[i] == LedMode::blink && from >= to)
        from = 0.0f;
    float w = 0.0f;
    switch (ledMode[i])
    {
        case LedMode::steady: return leds[i];
        case LedMode::blink: w = std::fmod (animTime * 2.0, 1.0) < 0.5 ? 1.0f : 0.0f; break;            // 2 Hz
        case LedMode::pulseSlow: w = 0.5f - 0.5f * (float) std::cos (animTime * juce::MathConstants<double>::twoPi * 0.5); break;
        case LedMode::pulseFast: w = 0.5f - 0.5f * (float) std::cos (animTime * juce::MathConstants<double>::twoPi * 1.5); break;
    }
    return (uint8_t) juce::roundToInt (from + (to - from) * w);
}

void PanelComponent::timerCallback()
{
    animTime += getTimerInterval() / 1000.0;
    for (auto m : ledMode)
        if (m != LedMode::steady)
        {
            repaint();
            break;
        }
}

juce::Rectangle<float> PanelComponent::toScreen (juce::Rectangle<float> r) const
{
    const float sx = getWidth() / panelW, sy = getHeight() / panelH;
    return { r.getX() * sx, r.getY() * sy, r.getWidth() * sx, r.getHeight() * sy };
}

void PanelComponent::resized()
{
    // The OLED sits in the middle of its round window.
    oled.setBounds (toScreen ({ 0, 0, 31.6f, 18.8f }).withCentre (displayDisc().getCentre()).toNearestInt());
}

juce::Rectangle<float> PanelComponent::displayDisc() const
{
    auto disc = toScreen ({ 30.0f, 27.0f, 40.0f, 0 });
    return disc.withHeight (disc.getWidth());
}

void PanelComponent::paint (juce::Graphics& g)
{
    if (background.isValid())
    {
        // The user's picture, stretched over the whole panel, in place of
        // the body and the backdrops behind the knobs and effect keys.
        g.drawImage (background, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
        g.setColour (juce::Colours::black);
        g.fillEllipse (displayDisc());
    }
    else
    {
        g.fillAll (body());
        // The round display window and the dark band behind the knobs.
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (toScreen ({ 10, 11, 80, 12 }), 8.0f);
        g.fillEllipse (displayDisc());
        g.setColour (juce::Colour (0xff111214));
        g.fillRoundedRectangle (toScreen ({ 11, 30, 20, 30 }), 10.0f);
        g.fillRoundedRectangle (toScreen ({ 69, 30, 20, 30 }), 10.0f);
    }

    // Section titles printed on the panel (photo pixel coordinates).
    const float ux = getWidth() / 570.0f, uy = getHeight() / 915.0f;
    // The text is centred over x0-x1; the rule runs under it (or above,
    // lineAbove) from lineX0 to lineX1, by default the same span.
    auto title = [&] (const char* text, float x0, float x1, float y, float lineX0 = -1, float lineX1 = -1,
                      bool lineAbove = false)
    {
        auto t = juce::Rectangle<float> (x0 * ux, y * uy, (x1 - x0) * ux, 12.0f * uy);
        g.setColour (textColour.withAlpha (0.8f));
        g.setFont (juce::FontOptions (t.getHeight() * 0.85f));
        g.drawText (text, t, juce::Justification::centred);
        const float ly = lineAbove ? t.getY() : t.getBottom();
        g.drawLine (lineX0 < 0 ? t.getX() : lineX0 * ux, ly, lineX1 < 0 ? t.getRight() : lineX1 * ux, ly, 1.0f);
    };
    title ("PATTERN SEQUENCER", 60, 230, 368);
    title ("SAMPLE EDIT", 260, 430, 368);
    title ("PUSH ENTER", 450, 516, 356);
    title ("SAMPLING", 120, 230, 425);
    title ("SAMPLE MODE", 395, 512, 425);
    title ("BANK", 392, 470, 478, 246, 463);                // over all five bank keys
    title ("DJ MODE", 380, 470, 525, -1, -1, true);
    // Over pad columns 1-2 and 3-4 (pads 78 wide, centred at 97, 191, 285, 378).
    title ("CH1", 58, 230, 535);
    title ("CH2", 246, 417, 535);

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
                // Knob names are printed above the knob, as on the unit
                // (VALUE has none: PUSH ENTER is printed above it).
                if (c.name != "VALUE")
                {
                    g.setColour (textColour);
                    g.setFont (juce::FontOptions (r.getHeight() * 0.22f));
                    g.drawText (c.name, r.withY (r.getY() - r.getHeight() * 0.3f).withHeight (r.getHeight() * 0.3f),
                                juce::Justification::centred);
                }
                break;
            }
            case PanelControl::Type::button:
            {
                g.setColour (c.pressed ? juce::Colour (0xff5b5e66) : juce::Colour (0xff34363c));
                g.fillRoundedRectangle (r, 3.0f);
                if (c.led >= 0 && shown (c.led) != 0)
                {
                    // A lit button: its LED glows through the key, faintly
                    // at the backlight level, fully when active.
                    const float level = shown (c.led) / 255.0f;
                    const bool red = c.name == "REC" || c.name == "RESAMPLE" || c.name == "DEL";
                    g.setColour ((red ? juce::Colour (0xffff2a3a) : accent()).withAlpha (0.12f + 0.78f * level));
                    g.fillRoundedRectangle (r, 3.0f);
                }
                // Names as large as the key allows, on up to two lines.
                g.setColour (juce::Colours::white);
                const bool twoLines = c.name.length() > 4;
                auto text = twoLines ? c.name.replace ("/", "/ ").replace ("+", "+ ") : c.name;
                if (c.bounds.getHeight() < 6.0f)
                    text = text.replaceFirstOccurrenceOf (" ", "\n");   // small keys: always two lines
                // Shrunk only as far as the longest line needs (squeezed
                // up to 20% narrower first).
                if (! text.contains ("\n") && text.contains (" "))
                {
                    // Big keys: one line if it fits, else two.
                    juce::Font one (juce::FontOptions (r.getHeight() * 0.4f, juce::Font::bold));
                    if (juce::GlyphArrangement::getStringWidth (one, text) > (r.getWidth() - 3.0f) / 0.8f)
                        text = text.replaceFirstOccurrenceOf (" ", "\n");
                }
                const auto lines = juce::StringArray::fromLines (text);
                juce::Font font (juce::FontOptions (r.getHeight() * (lines.size() > 1 ? 0.4f : 0.48f), juce::Font::bold));
                float widest = 0.0f;
                for (auto& line : lines)
                    widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (font, line));
                const float room = (r.getWidth() - 3.0f) / 0.8f;
                if (widest > room)
                    font = font.withHeight (font.getHeight() * room / widest);
                g.setFont (font);
                const float lineH = font.getHeight() * 1.02f;
                auto block = r.withSizeKeepingCentre (r.getWidth() - 2.0f, lineH * (float) lines.size());
                for (auto& line : lines)
                    g.drawFittedText (line, block.removeFromTop (lineH).toNearestInt(), juce::Justification::centred, 1, 0.8f);
                break;
            }
            case PanelControl::Type::pad:
            {
                // A solid pad; its LED colours the inner border, the number
                // and the legend (dim grey while the LED is off).
                g.setColour (c.pressed ? juce::Colour (0xff4a4d55) : juce::Colour (0xff2c2e33));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (juce::Colour (0xff55585f));
                g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 1.5f);
                auto ledColour = juce::Colour (0xff6a6d75);
                const int base = (c.name.getIntValue() - 1) * 3;
                if (base >= 0 && base + 2 < 0x30)
                {
                    const auto rgb = juce::Colour (shown (base), shown (base + 1), shown (base + 2));
                    if (rgb.getBrightness() > 0.0f)
                    {
                        // Brightness maps onto 0.45..1 so the resting (dim)
                        // colour stays readable and a lit pad still stands out.
                        const float peak = juce::jmax (rgb.getFloatRed(), rgb.getFloatGreen(), rgb.getFloatBlue());
                        const float lift = (0.45f + 0.55f * peak) / juce::jmax (peak, 0.01f);
                        ledColour = juce::Colour::fromFloatRGBA (juce::jmin (1.0f, rgb.getFloatRed() * lift),
                                                                 juce::jmin (1.0f, rgb.getFloatGreen() * lift),
                                                                 juce::jmin (1.0f, rgb.getFloatBlue() * lift), 1.0f);
                    }
                }
                g.setColour (ledColour);
                g.drawRoundedRectangle (r.reduced (r.getWidth() * 0.07f), 4.0f, juce::jmax (1.5f, r.getWidth() * 0.035f));
                // The number top right and the DJ-mode legend in a box, as
                // printed on the unit's pads.
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
        if ((c.name == "SHIFT" && c.pressed) || c.latched)
        {
            // Held (latched or by the keyboard): a bright ring and a tag.
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (r.expanded (1.5f), 4.0f, 2.0f);
            auto tag = r.withY (r.getBottom() + 2.0f).withHeight (r.getHeight() * 0.5f);
            g.setColour (accent());
            g.setFont (juce::FontOptions (tag.getHeight() * 0.9f, juce::Font::bold));
            g.drawText (c.name != "SHIFT" || shiftLatched ? "HELD" : "SHIFT KEY", tag, juce::Justification::centred);
        }
        if (c.sub.isNotEmpty())
        {
            // The SHIFT function, printed under the key (boxed under pads).
            auto s = r.withY (r.getBottom() + r.getHeight() * 0.06f).withHeight (juce::jmax (10.0f, getHeight() / panelH * 3.0f));
            if (c.type == PanelControl::Type::button)
                s = s.expanded (r.getWidth() * 0.3f, 0.0f);     // may run wider than its key
            g.setColour (textColour);
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

juce::String PanelComponent::getTooltip()
{
    auto* c = hit (getMouseXYRelative().toFloat());
    if (c == nullptr)
        return {};
    if (c->name == "SHIFT")
        return "Click to hold, or hold the Shift key";
    if (c->name == "VALUE")
        return "Drag or scroll to turn, click to push";
    if (c->type == PanelControl::Type::knob)
        return "Drag or scroll to turn";
    if (c->type == PanelControl::Type::button)
        return c->latched ? "Held: click to release" : "Ctrl-click to hold";
    return {};
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

void PanelComponent::updateShift()
{
    for (auto& c : controls)
        if (c.name == "SHIFT")
        {
            const bool held = shiftLatched || shiftKeyboard;
            if (held != c.pressed)
                press (c, held);
        }
}

void PanelComponent::tap (const juce::String& name)
{
    for (auto& c : controls)
        if (c.name == name)
        {
            press (c, true);
            juce::Timer::callAfterDelay (150, [this, name]
            {
                for (auto& c2 : controls)
                    if (c2.name == name)
                        press (c2, false);
            });
        }
}

void PanelComponent::setShiftFromKeyboard (bool down)
{
    shiftKeyboard = down;
    updateShift();
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
    if (c->name == "SHIFT")
    {
        shiftLatched = ! shiftLatched;
        updateShift();
        return;
    }
    if (c->latched)
    {
        // Latched: this click lets it go.
        c->latched = false;
        press (*c, false);
        return;
    }
    if (e.mods.isCtrlDown() && c->type == PanelControl::Type::button)
    {
        // Ctrl-click: held until the next click, for the firmware's
        // hold-and-turn and hold-and-pad combinations (MFX + VALUE or a
        // pad picks an effect).
        c->latched = true;
        press (*c, true);
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

void PanelComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // The wheel turns the knob under the pointer, even while another
    // control is held down with the button (hold MFX, scroll over VALUE).
    auto* c = hit (e.position);
    if (c == nullptr || c->type != PanelControl::Type::knob)
        return;
    const float delta = w.deltaY * (w.isReversed ? -1.0f : 1.0f);
    if (c->name == "VALUE")
    {
        // One detent per wheel notch (JUCE reports ~0.1 per notch).
        wheelAccum += delta;
        const int detents = (int) (wheelAccum / 0.1f);
        if (detents != 0)
        {
            wheelAccum -= (float) detents * 0.1f;
            link.sendEncoder (detents);
            c->value = std::fmod (c->value + 0.04f * (float) detents + 10.0f, 1.0f);
            repaint();
        }
        return;
    }
    setKnob (*c, c->value + delta * 0.25f);
}

void PanelComponent::mouseUp (const juce::MouseEvent&)
{
    if (active != nullptr && active->name == "VALUE")
    {
        if (! encoderMoved && active->binding.kind == Binding::Kind::key)
        {
            // A click pushes it: the push switch is a key in the matrix.
            const auto b = active->binding;
            link.sendKey (b.row, b.col, true);
            juce::Timer::callAfterDelay (120, [this, b]
            {
                link.sendKey (b.row, b.col, false);
                releaseShiftLatch();
            });
        }
        active = nullptr;
        return;
    }
    if (active != nullptr && active->type != PanelControl::Type::knob)
    {
        press (*active, false);
        releaseShiftLatch();
    }
    active = nullptr;
}

void PanelComponent::releaseShiftLatch()
{
    // A clicked SHIFT lasts for one key or pad (let go after it is): it is
    // too easy to forget it is still held. Turning knobs (SHIFT: FINE)
    // keeps it.
    if (! shiftLatched)
        return;
    shiftLatched = false;
    updateShift();
    repaint();
}
