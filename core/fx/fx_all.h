// The effect factories, one per group (fx_*.cpp).
#pragma once

#include "fx.h"

namespace sp404fx {

std::unique_ptr<Effect> MakeFilter(int id);
std::unique_ptr<Effect> MakeMod(int id);
std::unique_ptr<Effect> MakeDelay(int id);
std::unique_ptr<Effect> MakeReverb(int id);
std::unique_ptr<Effect> MakeDrive(int id);
std::unique_ptr<Effect> MakePitch(int id);

// Tempo-synced lengths the firmware lists (in beats), longest first.
// SPEED/RATE lists: 2/1 1/1D 2/1T 1/1 1/2D 1/1T 1/2 1/4D 1/2T 1/4 1/8D
// 1/4T 1/8 1/16D 1/8T 1/16 1/32D 1/16T 1/32 1/32T 1/64 1/64T.
constexpr float kNotes22[22] = {8.f,    6.f,    16.f / 3, 4.f,     3.f,    8.f / 3,
                                2.f,    1.5f,   4.f / 3,  1.f,     0.75f,  2.f / 3,
                                0.5f,   0.375f, 1.f / 3,  0.25f,   0.1875f, 1.f / 6,
                                0.125f, 1.f / 12, 0.0625f, 1.f / 24};

inline float Note22(float n)
{
    return kNotes22[int(Clamp(n, 0.f, 1.f) * 21.f + 0.5f)];
}

} // namespace sp404fx
