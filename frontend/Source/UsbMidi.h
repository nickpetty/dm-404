#pragma once

// MIDI messages to and from USB-MIDI event packets (4 bytes: cable << 4 |
// code index, then up to 3 MIDI bytes), the form the unit's firmware
// exchanges MIDI in over its BMC link.
//
// Cables as the firmware uses them: it plays MIDI arriving on cable 8
// (USB) or 9 (taken as the MIDI IN jack), and sends its MIDI out on 9: bit 3
// USB, bit 0 the MIDI OUT jack.

#include <JuceHeader.h>
#include <array>

namespace usbmidi
{
    constexpr uint8_t usbCable = 8, dinCable = 9;
    constexpr uint8_t usbBit = 8, dinBit = 1;

    using Packet = std::array<uint8_t, 4>;

    // Appends the packets for one MIDI message.
    template <typename Fn>
    inline void encode (const juce::MidiMessage& m, uint8_t cable, Fn&& out)
    {
        const auto* d = m.getRawData();
        const int n = m.getRawDataSize();
        if (n <= 0)
            return;
        const uint8_t c = (uint8_t) (cable << 4);
        const uint8_t s = d[0];
        if (s == 0xf0)
        {
            // SysEx: three bytes a packet (code 4), the last one 5, 6 or 7.
            int i = 0;
            while (n - i > 3)
            {
                out (Packet { (uint8_t) (c | 4), d[i], d[i + 1], d[i + 2] });
                i += 3;
            }
            const int left = n - i;
            Packet p { (uint8_t) (c | (4 + left)), 0, 0, 0 };
            for (int k = 0; k < left; ++k)
                p[(size_t) k + 1] = d[i + k];
            out (p);
        }
        else if (s >= 0x80 && s < 0xf0)
            out (Packet { (uint8_t) (c | (s >> 4)), s, n > 1 ? d[1] : (uint8_t) 0, n > 2 ? d[2] : (uint8_t) 0 });
        else if (s >= 0xf8)
            out (Packet { (uint8_t) (c | 0xf), s, 0, 0 });
        else if (s == 0xf2)
            out (Packet { (uint8_t) (c | 3), s, n > 1 ? d[1] : (uint8_t) 0, n > 2 ? d[2] : (uint8_t) 0 });
        else if (s == 0xf1 || s == 0xf3)
            out (Packet { (uint8_t) (c | 2), s, n > 1 ? d[1] : (uint8_t) 0, 0 });
        else
            out (Packet { (uint8_t) (c | 5), s, 0, 0 });
    }

    // Packets back to messages; keeps a partial SysEx between packets.
    class Decoder
    {
    public:
        // Returns true and sets `out` when a whole message is complete.
        bool add (const uint8_t* p, juce::MidiMessage& out)
        {
            const int cin = p[0] & 0xf;
            int n = 0;
            switch (cin)
            {
                case 0x8: case 0x9: case 0xa: case 0xb: case 0xe: case 0x3: case 0x4: case 0x7: n = 3; break;
                case 0xc: case 0xd: case 0x2: case 0x6: n = 2; break;
                case 0x5: case 0xf: n = 1; break;
                default: return false;                  // 0, 1: reserved / the BMC's own
            }
            if (cin == 4 || ((cin == 5 || cin == 6 || cin == 7) && (! sysex.isEmpty() || p[1] == 0xf0)))
            {
                // SysEx start, continuation or end.
                for (int i = 0; i < n; ++i)
                    sysex.add (p[1 + i]);
                if (cin == 4 && sysex.size() < 65536)
                    return false;
                out = juce::MidiMessage (sysex.getRawDataPointer(), sysex.size());
                sysex.clearQuick();
                return true;
            }
            out = juce::MidiMessage (p + 1, n);
            return true;
        }

    private:
        juce::Array<uint8_t> sysex;
    };
}
