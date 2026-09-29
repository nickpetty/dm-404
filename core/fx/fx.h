// The effect interface and the small DSP pieces the effects share.
//
// Parameters arrive as the raw values the firmware sends (usually 0-255,
// some enumerations: core/fx/fxmap.json). Each effect maps them to sound.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "daisysp.h"
#include "fxparams.h"

namespace sp404fx {

// What every effect can see: the rate, the tempo, and whether it is on.
struct Context
{
    float sr    = 48000.f;
    float bpm   = 120.f;
    // Seconds per beat (a quarter note).
    float Beat() const { return 60.f / bpm; }
};

// Note lengths of the tempo-synced times (the firmware's list at
// 0x80030cb0: 1/32, 1/16T, 1/32D, 1/16 ... 1/2D, 1/1), in beats.
constexpr float kSyncBeats[16] = {0.125f, 1.f / 6.f, 0.1875f, 0.25f,
                                  1.f / 3.f, 0.375f, 0.5f, 2.f / 3.f,
                                  0.75f, 1.f, 4.f / 3.f, 1.5f,
                                  2.f, 8.f / 3.f, 3.f, 4.f};

inline float Clamp(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

// 0..max -> lo..hi on a log scale (frequencies, times).
inline float ExpMap(float n, float lo, float hi)
{
    return lo * std::pow(hi / lo, Clamp(n, 0.f, 1.f));
}

inline float DbToGain(float db)
{
    return std::pow(10.f, db / 20.f);
}

// Soft clip with unity slope at zero.
inline float SoftClip(float x)
{
    return std::tanh(x);
}

// DaisySP's Oscillator at full swing (its default amplitude is 0.5): the
// modulation source of every LFO here.
class Lfo : public daisysp::Oscillator
{
  public:
    void Init(float sr)
    {
        Oscillator::Init(sr);
        SetAmp(1.f);
    }
};

// A one-pole smoother for parameters, so knob steps do not zipper.
class Smooth
{
  public:
    void  Init(float sr, float ms = 20.f) { coef_ = 1.f - std::exp(-1.f / (0.001f * ms * sr)); }
    void  Set(float target) { target_ = target; }
    void  Reset(float v) { target_ = v_ = v; }
    float Next() { return v_ += coef_ * (target_ - v_); }
    float Value() const { return v_; }

  private:
    float coef_ = 0.01f, target_ = 0.f, v_ = 0.f;
};

// RBJ cookbook biquad (transposed direct form II).
class Biquad
{
  public:
    enum Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };

    void Set(Type t, float sr, float f, float q, float gain_db = 0.f)
    {
        f              = Clamp(f, 10.f, sr * 0.45f);
        q              = std::fmax(q, 0.05f);
        const float A  = std::pow(10.f, gain_db / 40.f);
        const float w0 = 2.f * float(M_PI) * f / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float al = sw / (2.f * q);
        float b0, b1, b2, a0, a1, a2;
        switch(t)
        {
            case LowPass:
                b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2;
                a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
                break;
            case HighPass:
                b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
                a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
                break;
            case BandPass:
                b0 = al, b1 = 0, b2 = -al;
                a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
                break;
            case Notch:
                b0 = 1, b1 = -2 * cw, b2 = 1;
                a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
                break;
            case Peak:
                b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A;
                a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
                break;
            case LowShelf:
            {
                const float s = 2 * std::sqrt(A) * al;
                b0            = A * ((A + 1) - (A - 1) * cw + s);
                b1            = 2 * A * ((A - 1) - (A + 1) * cw);
                b2            = A * ((A + 1) - (A - 1) * cw - s);
                a0            = (A + 1) + (A - 1) * cw + s;
                a1            = -2 * ((A - 1) + (A + 1) * cw);
                a2            = (A + 1) + (A - 1) * cw - s;
                break;
            }
            default: // HighShelf
            {
                const float s = 2 * std::sqrt(A) * al;
                b0            = A * ((A + 1) + (A - 1) * cw + s);
                b1            = -2 * A * ((A - 1) + (A + 1) * cw);
                b2            = A * ((A + 1) + (A - 1) * cw - s);
                a0            = (A + 1) - (A - 1) * cw + s;
                a1            = 2 * ((A - 1) - (A + 1) * cw);
                a2            = (A + 1) - (A - 1) * cw - s;
                break;
            }
        }
        b0_ = b0 / a0, b1_ = b1 / a0, b2_ = b2 / a0, a1_ = a1 / a0, a2_ = a2 / a0;
    }
    float Process(float x)
    {
        const float y = b0_ * x + z1_;
        z1_           = b1_ * x - a1_ * y + z2_;
        z2_           = b2_ * x - a2_ * y;
        return y;
    }
    void Reset() { z1_ = z2_ = 0.f; }

  private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

// A stereo pair of biquads with shared settings.
struct Biquad2
{
    Biquad l, r;
    void   Set(Biquad::Type t, float sr, float f, float q, float g = 0.f)
    {
        l.Set(t, sr, f, q, g);
        r.Set(t, sr, f, q, g);
    }
    void Process(float &a, float &b)
    {
        a = l.Process(a);
        b = r.Process(b);
    }
};

// A heap-allocated fractional delay line (seconds of audio at 48 kHz).
class Delay
{
  public:
    explicit Delay(float max_seconds = 6.f, float sr = 48000.f)
    : buf_(size_t(max_seconds * sr) + 4, 0.f)
    {
    }
    void Write(float x)
    {
        buf_[w_] = x;
        if(++w_ == buf_.size())
            w_ = 0;
    }
    // Delay in samples (>= 1), linear interpolation.
    float Read(float d) const
    {
        d              = Clamp(d, 1.f, float(buf_.size() - 3));
        const size_t n = buf_.size();
        // w_ is where the next sample goes: the newest is at w_ - 1.
        const float  p = float(w_) - d + float(n);
        const size_t i = size_t(p);
        const float  f = p - float(i);
        const float  a = buf_[i % n], b = buf_[(i + 1) % n];
        return a + (b - a) * f;
    }
    size_t Size() const { return buf_.size(); }
    void   Clear() { std::fill(buf_.begin(), buf_.end(), 0.f); }

  private:
    std::vector<float> buf_;
    size_t             w_ = 0;
};

class Effect
{
  public:
    virtual ~Effect() = default;
    // Called once, before any parameter.
    virtual void Init(const Context &ctx) { ctx_ = &ctx; }
    // A parameter changed (i = -1: all of them, after creation).
    virtual void Changed(int i) { (void)i; }
    // The slot was switched on: one-shot effects (Stopper, Back Spin, the
    // loopers) start over.
    virtual void Engage() {}
    virtual void Process(float &l, float &r) = 0;

    void Set(int i, int v)
    {
        if(i >= 0 && i < kMaxParams)
        {
            p_[i] = v;
            Changed(i);
        }
    }
    static constexpr int kMaxParams = 32;

  protected:
    const Context *ctx_ = nullptr;
    int            p_[kMaxParams] = {};

    float Sr() const { return ctx_->sr; }
    // Parameter i as 0..1 of max.
    float N(int i, float max = 255.f) const { return Clamp(p_[i] / max, 0.f, 1.f); }
    // 0-255 levels: 255 is unity.
    float Level(int i) const { return N(i); }
    // Equalizer gains 0-30 (15 = 0 dB), as the firmware's EQ LOW/EQ HIGH.
    float EqDb(int i) const { return float(p_[i] - 15); }
    // Dry/wet mix, BALANCE 0-255 (fully wet at 255).
    static float Mix(float dry, float wet, float bal) { return dry + (wet - dry) * bal; }
};

std::unique_ptr<Effect> MakeEffect(int id);

} // namespace sp404fx
