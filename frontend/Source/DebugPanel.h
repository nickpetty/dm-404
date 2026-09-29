#pragma once

#include "PanelComponent.h"

// The emulated hardware's raw inputs, for finding out what each panel
// control is: the 8x7 key matrix, the 32 analog inputs (ADC1 channels 3-6
// through the 8-way mux), a BMC packet injector and a log of what the
// firmware sends the BMC. With a panel control in learn mode, using one of
// these binds it.
class DebugPanel : public juce::Component
{
public:
    DebugPanel (EmulatorLink& link, PanelComponent& panel);

    void resized() override;
    void paint (juce::Graphics&) override;
    void logBmc (const uint8_t* packet);

private:
    class MatrixKey : public juce::TextButton
    {
    public:
        MatrixKey (DebugPanel& o, int r, int c);
        void buttonStateChanged() override;
        DebugPanel& owner;
        int row, col;
        bool down = false;
    };

    EmulatorLink& link;
    PanelComponent& panel;
    juce::OwnedArray<MatrixKey> keys;
    juce::OwnedArray<juce::Slider> analog;
    juce::TextEditor bmcPacket;
    juce::TextButton bmcSend { "Send to firmware" };
    juce::TextEditor log;
    juce::StringArray pending;
    juce::CriticalSection pendingLock;
    std::set<juce::String> seenSystem;
};
