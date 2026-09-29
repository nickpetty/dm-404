// Reverbs: Reverb, Ha-Dou, SX Reverb (DaisySP's ReverbSc).
#include "daisysp-lgpl.h"
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

namespace {

// A ReverbSc with pre-delay and input/output cut filters.
class Room
{
  public:
    ReverbSc rv;
    Delay    pre[2]{Delay(0.3f), Delay(0.3f)};
    Biquad2  lowcut, highcut;
    float    pre_samples = 1.f;

    void Init(float sr) { rv.Init(sr); }
    void Set(float sr, float feedback, float damp_hz, float pre_ms, float low_hz, float high_hz)
    {
        rv.SetFeedback(Clamp(feedback, 0.f, 0.985f));
        rv.SetLpFreq(damp_hz);
        pre_samples = std::fmax(1.f, pre_ms * 0.001f * sr);
        lowcut.Set(Biquad::HighPass, sr, low_hz, 0.7071f);
        highcut.Set(Biquad::LowPass, sr, high_hz, 0.7071f);
    }
    void Process(float l, float r, float &wl, float &wr)
    {
        pre[0].Write(l), pre[1].Write(r);
        float il = pre[0].Read(pre_samples), ir = pre[1].Read(pre_samples);
        lowcut.Process(il, ir);
        rv.Process(il, ir, &wl, &wr);
        highcut.Process(wl, wr);
    }
};

} // namespace

// 10 Reverb: TYPE 0-3 AMBI, ROOM, HALL1, HALL2.
class Reverb : public Effect
{
    Room rm_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        rm_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx10;
        static const float base[4] = {0.45f, 0.65f, 0.8f, 0.88f};
        static const float damp[4] = {9000.f, 7000.f, 6000.f, 5000.f};
        const int          t       = p_[TYPE] & 3;
        const float        fb      = base[t] + N(TIME) * (0.98f - base[t]);
        rm_.Set(Sr(), fb, damp[t] * (0.6f + N(DENSITY) * 0.6f), N(PRE_DELAY) * 200.f,
                p_[LOW_CUT] ? ExpMap(N(LOW_CUT), 20.f, 800.f) : 20.f,
                p_[HIGH_CUT] ? ExpMap(N(HIGH_CUT), 1000.f, 20000.f) : 16000.f);
    }
    void Process(float &l, float &r) override
    {
        float wl, wr;
        rm_.Process(l, r, wl, wr);
        const float lv = Level(fxp::fx10::LEVEL);
        l += wl * lv, r += wr * lv;
    }
};

// 37 Ha-Dou: a reverb whose tail swims (MOD DEPTH), "wave motion".
class HaDou : public Effect
{
    Room       rm_;
    Delay      d_[2]{Delay(0.05f), Delay(0.05f)};
    Lfo        lfo_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        rm_.Init(c.sr);
        lfo_.Init(c.sr);
        lfo_.SetWaveform(Oscillator::WAVE_SIN);
        lfo_.SetFreq(0.35f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx37;
        rm_.Set(Sr(), 0.7f + N(TIME) * 0.28f, 7000.f, N(PRE_DELAY) * 200.f,
                p_[LOW_CUT] ? ExpMap(N(LOW_CUT), 20.f, 800.f) : 20.f,
                p_[HIGH_CUT] ? ExpMap(N(HIGH_CUT), 1000.f, 20000.f) : 14000.f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx37;
        float wl, wr;
        rm_.Process(l, r, wl, wr);
        const float m  = lfo_.Process() * N(MOD_DEPTH);
        const float ms = 0.001f * Sr();
        d_[0].Write(wl), d_[1].Write(wr);
        wl = d_[0].Read((12.f + 10.f * m) * ms);
        wr = d_[1].Read((12.f - 10.f * m) * ms);
        const float lv = Level(LEVEL);
        l += wl * lv, r += wr * lv;
    }
};

// 44 SX Reverb: TIME 0-127, TONE -100..+100, TYPE picks the size.
class SxReverb : public Effect
{
    Room    rm_;
    Biquad2 tlo_, thi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        rm_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx44;
        const float size = 0.55f + 0.1f * (p_[TYPE] & 3);
        rm_.Set(Sr(), size + N(TIME, 127.f) * (0.985f - size), 8000.f, N(PRE_DELAY) * 150.f, 40.f, 18000.f);
        const float t = (N(TONE) - 0.5f) * 2.f;
        tlo_.Set(Biquad::LowShelf, Sr(), 300.f, 0.7f, -t * 8.f);
        thi_.Set(Biquad::HighShelf, Sr(), 3000.f, 0.7f, t * 8.f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx44;
        float wl, wr;
        rm_.Process(l, r, wl, wr);
        tlo_.Process(wl, wr);
        thi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, wl * lv * 1.5f, N(BALANCE)), r = Mix(r, wr * lv * 1.5f, N(BALANCE));
    }
};

std::unique_ptr<Effect> MakeReverb(int id)
{
    switch(id)
    {
        case 10: return std::make_unique<Reverb>();
        case 37: return std::make_unique<HaDou>();
        case 44: return std::make_unique<SxReverb>();
        default: return nullptr;
    }
}

} // namespace sp404fx
