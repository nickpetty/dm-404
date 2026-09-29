#include "DebugPanel.h"

namespace
{
    // The analog inputs the firmware scans: ADC1 channels 4, 5, 6 and 3.
    constexpr int scannedChannels[] = { 4, 5, 6, 3 };
}

DebugPanel::MatrixKey::MatrixKey (DebugPanel& o, int r, int c)
    : juce::TextButton (juce::String (r) + "," + juce::String (c)), owner (o), row (r), col (c)
{
}

void DebugPanel::MatrixKey::buttonStateChanged()
{
    const bool now = isDown();
    if (now == down)
        return;
    down = now;
    if (down && owner.panel.getLearning() != nullptr)
    {
        Binding b;
        b.kind = Binding::Kind::key;
        b.row = row;
        b.col = col;
        owner.panel.learn (b);
    }
    owner.link.sendKey (row, col, down);
}

DebugPanel::DebugPanel (EmulatorLink& l, PanelComponent& p) : link (l), panel (p)
{
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 7; ++c)
            addAndMakeVisible (keys.add (new MatrixKey (*this, r, c)));

    for (int ch : scannedChannels)
        for (int mux = 0; mux < 8; ++mux)
        {
            auto* s = analog.add (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox));
            s->setRange (0, 4095, 1);
            s->setValue (2048, juce::dontSendNotification);
            s->setTooltip ("ADC1 ch" + juce::String (ch) + " mux " + juce::String (mux));
            s->onDragStart = [this, ch, mux]
            {
                if (panel.getLearning() != nullptr)
                {
                    Binding b;
                    b.kind = Binding::Kind::analog;
                    b.adc = 0;
                    b.channel = ch;
                    b.mux = mux;
                    panel.learn (b);
                }
            };
            s->onValueChange = [this, s, ch, mux]
            {
                link.sendKnob (0, ch, mux, (int) s->getValue());
            };
            addAndMakeVisible (s);
        }

    bmcPacket.setText ("09904064");
    bmcPacket.setTooltip ("4 bytes, hex: cable/CIN, then 3 MIDI-style bytes");
    addAndMakeVisible (bmcPacket);
    bmcSend.onClick = [this]
    {
        juce::MemoryBlock mb;
        mb.loadFromHexString (bmcPacket.getText());
        if (mb.getSize() != 4)
            return;
        const auto* d = static_cast<const uint8_t*> (mb.getData());
        if (panel.getLearning() != nullptr)
        {
            Binding b;
            b.kind = Binding::Kind::bmc;
            std::memcpy (b.down, d, 4);
            std::memcpy (b.up, d, 4);
            if ((b.up[0] & 0x0f) == 0x09)       // note on: release is note off
            {
                b.up[0] = (uint8_t) ((b.up[0] & 0xf0) | 0x08);
                b.up[1] = (uint8_t) ((b.up[1] & 0x0f) | 0x80);
                b.up[3] = 0;
            }
            panel.learn (b);
        }
        link.sendBmc (d);
    };
    addAndMakeVisible (bmcSend);

    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    addAndMakeVisible (log);
}

void DebugPanel::logBmc (const uint8_t* p)
{
    // System messages (CIN 0/1) once each; MIDI is too chatty to log.
    const juce::String hex = juce::String::toHexString (p, 4, 1);
    if ((p[0] & 0x0f) > 1)
        return;
    const juce::ScopedLock sl (pendingLock);
    if (seenSystem.insert (hex).second)
    {
        pending.add (hex);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DebugPanel> (this)]
        {
            if (safe == nullptr)
                return;
            juce::StringArray lines;
            {
                const juce::ScopedLock sl2 (safe->pendingLock);
                lines.swapWith (safe->pending);
            }
            for (auto& line : lines)
                safe->log.insertTextAtCaret (line + "\n");
        });
    }
}

void DebugPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15161a));
    g.setColour (juce::Colours::grey);
    g.setFont (13.0f);
    g.drawText ("Key matrix (row, column)", 10, 4, 300, 18, juce::Justification::left);
    g.drawText ("Analog inputs: ADC1 ch 4, 5, 6, 3 x mux 0-7", 10, 238, 400, 18, juce::Justification::left);
    g.drawText ("BMC", 10, 420, 100, 18, juce::Justification::left);
}

void DebugPanel::resized()
{
    const int kw = (getWidth() - 20) / 7;
    for (auto* k : keys)
        k->setBounds (10 + k->col * kw, 24 + k->row * 26, kw - 4, 22);

    const int sw = (getWidth() - 20) / 32;
    for (int i = 0; i < analog.size(); ++i)
        analog[i]->setBounds (10 + i * sw, 258, sw - 1, 150);

    bmcPacket.setBounds (10, 440, 120, 24);
    bmcSend.setBounds (136, 440, 130, 24);
    log.setBounds (10, 470, getWidth() - 20, getHeight() - 480);
}
