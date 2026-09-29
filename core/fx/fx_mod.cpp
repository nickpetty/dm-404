// Modulation: Phaser, Flanger, Chorus, JUNO Chorus, Tremolo/Pan, Ring Mod,
// Slicer, To-Gu-Ro, Stopper, Downer.
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

namespace {

// RATE on the phaser/flanger: 4 bars down to 0.016 bars per cycle.
float BarsRate(float n, float beat)
{
    const float bars = 4.f - n * 3.984f;
    return 1.f / (bars * 4.f * beat);
}

} // namespace

// 5 Phaser (DaisySP Phaser). MANUAL is the centre frequency.
class Phaser : public Effect
{
    daisysp::Phaser ph_[2];

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ph_[0].Init(c.sr), ph_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx05;
        const int poles[] = {4, 8, 12, 16};
        for(int k = 0; k < 2; k++)
        {
            ph_[k].SetPoles(poles[p_[TYPE] & 3]);
            ph_[k].SetLfoDepth(N(DEPTH));
            ph_[k].SetLfoFreq(p_[SYNC] ? BarsRate(N(RATE), ctx_->Beat()) * (k ? 1.02f : 1.f)
                                       : ExpMap(N(RATE), 0.05f, 10.f));
            ph_[k].SetFreq(ExpMap(N(MANUAL), 100.f, 6000.f));
            ph_[k].SetFeedback(N(RESONANCE) * 0.9f);
        }
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx05;
        const float bal = p_[BALANCE] ? N(BALANCE) : 0.5f;
        l = Mix(l, ph_[0].Process(l), bal), r = Mix(r, ph_[1].Process(r), bal);
    }
};

// 6 Flanger (DaisySP Flanger).
class Flanger : public Effect
{
    daisysp::Flanger fl_[2];
    Biquad2          lowcut_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        fl_[0].Init(c.sr), fl_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx06;
        for(int k = 0; k < 2; k++)
        {
            fl_[k].SetLfoDepth(N(DEPTH));
            fl_[k].SetLfoFreq(p_[SYNC] ? BarsRate(N(RATE), ctx_->Beat()) * (k ? 1.03f : 1.f)
                                       : ExpMap(N(RATE), 0.05f, 10.f));
            fl_[k].SetDelay(0.05f + N(MANUAL) * 0.9f);
            fl_[k].SetFeedback(N(RESONANCE) * 0.9f);
        }
        lowcut_.Set(Biquad::HighPass, Sr(), ExpMap(N(LOW_CUT_F), 20.f, 800.f), 0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx06;
        float wl = l, wr = r;
        lowcut_.Process(wl, wr);
        wl = fl_[0].Process(wl), wr = fl_[1].Process(wr);
        const float bal = p_[BALANCE] ? N(BALANCE) : 0.5f;
        l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

// 14 Chorus (DaisySP Chorus, stereo). RATE shows as a period, 0.33-2.30 s.
class Chorus : public Effect
{
    daisysp::Chorus ch_;
    Biquad2         lo_, hi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ch_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx14;
        ch_.SetLfoFreq(1.f / (0.33f + N(RATE) * 1.97f));
        ch_.SetLfoDepth(0.1f + N(DEPTH) * 0.9f);
        ch_.SetDelayMs(8.f);
        lo_.Set(Biquad::LowShelf, Sr(), 250.f, 0.7f, EqDb(EQ_LOW));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, EqDb(EQ_HIGH));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx14;
        ch_.Process((l + r) * 0.5f);
        // DaisySP's chorus outputs the delayed voice at about -6 dB.
        float wl = ch_.GetLeft() * 2.f, wr = ch_.GetRight() * 2.f;
        lo_.Process(wl, wr);
        hi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, wl * lv, N(BALANCE)), r = Mix(r, wr * lv, N(BALANCE));
    }
};

// 15 JUNO Chorus: the Juno-60 modes I, II, I+II and the JX-1's two, with the
// BBD's hiss (NOISE).
class JunoChorus : public Effect
{
    Delay      d_[2]{Delay(0.05f), Delay(0.05f)};
    Lfo        lfo_;
    WhiteNoise noise_;
    Biquad2    bbd_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        lfo_.Init(c.sr);
        lfo_.SetWaveform(Oscillator::WAVE_TRI);
        noise_.Init();
        bbd_.Set(Biquad::LowPass, c.sr, 9000.f, 0.7071f);
    }
    void Changed(int) override
    {
        // Mode: rate (Hz) and depth (ms).
        static const float rate[5]  = {0.513f, 0.863f, 9.75f, 0.4f, 0.6f};
        static const float depth[5] = {1.85f, 1.85f, 0.3f, 2.5f, 3.2f};
        const int          m        = p_[fxp::fx15::MODE] % 5;
        lfo_.SetFreq(rate[m]);
        depth_ = depth[m];
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx15;
        const float m   = lfo_.Process();
        const float ms  = 0.001f * Sr();
        const float n   = noise_.Process() * N(NOISE) * 0.004f;
        const float in  = (l + r) * 0.5f;
        d_[0].Write(in + n), d_[1].Write(in + n);
        float wl = d_[0].Read((3.5f + depth_ * m) * ms);
        float wr = d_[1].Read((3.5f - depth_ * m) * ms);
        bbd_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, (l + wl) * 0.5f * lv * 1.4f, N(BALANCE));
        r = Mix(r, (r + wr) * 0.5f * lv * 1.4f, N(BALANCE));
    }

  private:
    float depth_ = 1.85f;
};

// 16 Tremolo/Pan: TYPE 0 tremolo, 1 auto-pan; RATE 1 - 0.01 bars.
class TremoloPan : public Effect
{
    Lfo        lfo_;
    Biquad2    lo_, hi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        lfo_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx16;
        const uint8_t w[] = {Oscillator::WAVE_TRI, Oscillator::WAVE_SQUARE, Oscillator::WAVE_SIN,
                             Oscillator::WAVE_SAW, Oscillator::WAVE_RAMP};
        lfo_.SetWaveform(w[p_[WAVE] % 5]);
        const float bars = 1.f - N(RATE) * 0.99f;
        lfo_.SetFreq(1.f / (bars * 4.f * ctx_->Beat())); // RATE shows in bars
        lo_.Set(Biquad::LowShelf, Sr(), 250.f, 0.7f, EqDb(EQ_LOW));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, EqDb(EQ_HIGH));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx16;
        const float m = lfo_.Process() * N(DEPTH); // -depth..depth
        if(p_[TYPE] == 0)
        {
            const float g = 1.f - (m * 0.5f + N(DEPTH) * 0.5f);
            l *= g, r *= g;
        }
        else
        {
            const float pan = 0.5f + m * 0.5f;
            l *= std::sqrt(1.f - pan) * 1.41f, r *= std::sqrt(pan) * 1.41f;
        }
        lo_.Process(l, r);
        hi_.Process(l, r);
        if(p_[LEVEL])
            l *= Level(LEVEL), r *= Level(LEVEL);
    }
};

// 21 Ring Mod: a sine carrier, FREQUENCY 0-100 (display) over 20 Hz-2 kHz,
// raised by the input's envelope (SENS).
class RingMod : public Effect
{
    Lfo        car_;
    float      env_ = 0.f;
    Biquad2    lo_, hi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        car_.Init(c.sr);
        car_.SetWaveform(Oscillator::WAVE_SIN);
    }
    void Changed(int) override
    {
        using namespace fxp::fx21;
        lo_.Set(Biquad::LowShelf, Sr(), 250.f, 0.7f, EqDb(EQ_LOW));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, EqDb(EQ_HIGH));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx21;
        const float a = std::fabs(l) + std::fabs(r);
        env_ += (a > env_ ? 0.01f : 0.0005f) * (a - env_);
        const float f = ExpMap(Clamp(N(FREQUENCY) + (p_[POLARITY] ? -1.f : 1.f) * env_ * N(SENS), 0.f, 1.f),
                               20.f, 2000.f);
        car_.SetFreq(f);
        const float c  = car_.Process();
        float       wl = l * c, wr = r * c;
        lo_.Process(wl, wr);
        hi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, wl * lv, N(BALANCE)), r = Mix(r, wr * lv, N(BALANCE));
    }
};

// 26 Slicer: a 16-step gate, one of 32 patterns (PATTERN), each step one
// SPEED note long (2/1 - 1/64T), with SHUFFLE and a soft ATTACK.
class Slicer : public Effect
{
    double pos_ = 0.0; // in steps
    Smooth g_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        g_.Init(c.sr, 1.f);
    }
    void Changed(int) override { g_.Init(Sr(), 0.3f + N(fxp::fx26::ATTACK) * 20.f); }
    void Engage() override { pos_ = 0.0; }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx26;
        const float step_s = Note22(N(SPEED)) * ctx_->Beat();
        pos_ += 1.0 / (step_s * Sr());
        if(pos_ >= 16.0)
            pos_ -= 16.0;
        const int   step = int(pos_);
        const float frac = float(pos_ - step);
        // Patterns: bit masks generated from the pattern number, always
        // opening on the first step.
        const uint32_t pat  = Pattern(p_[PATTERN] & 31);
        const bool     open = (pat >> step) & 1;
        const float    duty = 0.55f + N(SHUFFLE) * 0.3f * (step & 1);
        const float    tgt  = open && frac < duty ? 1.f : 1.f - N(DEPTH);
        g_.Set(tgt);
        const float g = g_.Next();
        l *= g, r *= g;
        if(p_[LEVEL])
            l *= Level(LEVEL), r *= Level(LEVEL);
    }

  private:
    static uint32_t Pattern(int n)
    {
        static const uint16_t kPat[32] = {
            0xffff, 0x5555, 0xaaab, 0x3333, 0x7777, 0xdddd, 0x6db6, 0x9249, 0x0f0f, 0xf0f1,
            0x5f5f, 0xa5a5, 0x6666, 0x3c3c, 0xcccd, 0x1111, 0x8889, 0x7d7d, 0xbebf, 0x5b5b,
            0x2d2d, 0xb6db, 0x7575, 0xe7e7, 0x3f3f, 0xfcfd, 0x0f3f, 0x55ff, 0xff55, 0x6f6f,
            0xf6f7, 0x9999};
        return kPat[n] | 1u;
    }
};

// 38 To-Gu-Ro: a swirl of filter and amplitude modulation (RATE, DEPTH,
// RESONANCE), in tempo when SYNC is on.
class ToGuRo : public Effect
{
    Svf        f_[2];
    Lfo        lfo_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        f_[0].Init(c.sr), f_[1].Init(c.sr);
        lfo_.Init(c.sr);
        lfo_.SetWaveform(Oscillator::WAVE_SIN);
    }
    void Changed(int) override
    {
        using namespace fxp::fx38;
        lfo_.SetFreq(p_[SYNC] ? 1.f / (Note22(N(RATE)) * ctx_->Beat()) : ExpMap(N(RATE), 0.1f, 12.f));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx38;
        const float m   = lfo_.Process();
        const float fm  = p_[FLT_MOD] ? N(FLT_MOD) : 0.7f;
        const float am  = p_[AMP_MOD] ? N(AMP_MOD) : 0.5f;
        const float fc  = ExpMap(Clamp(0.55f + m * 0.45f * N(DEPTH) * fm, 0.f, 1.f), 120.f, 12000.f);
        float       x[2] = {l, r};
        for(int k = 0; k < 2; k++)
        {
            f_[k].SetFreq(fc * (k ? 1.1f : 1.f)), f_[k].SetRes(N(RESONANCE) * 0.9f);
            f_[k].Process(x[k]);
            x[k] = f_[k].Low();
        }
        const float g = 1.f - am * N(DEPTH) * (0.5f - 0.5f * m);
        l = x[0] * g, r = x[1] * g;
    }
};

// 24 Stopper: the turntable-stop: once engaged the pitch and cutoff fall
// over RATE (4/1 - 1/64), then silence; DEPTH how far it falls.
class Stopper : public Effect
{
    Delay   d_[2]{Delay(4.f), Delay(4.f)};
    double  read_ = 0.0, speed_ = 1.0;
    size_t  t_    = 0;
    Svf     f_[2];

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        f_[0].Init(c.sr), f_[1].Init(c.sr);
    }
    void Engage() override { t_ = 0, read_ = 0.0, speed_ = 1.0; }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx24;
        static const float kRate[9] = {16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f, 0.0625f};
        const float        len      = kRate[p_[RATE] % 9] * ctx_->Beat() * Sr();
        const float        prog     = Clamp(float(t_++) / len, 0.f, 1.f);
        const float        target   = 1.f - prog * (0.2f + 0.8f * N(DEPTH));
        speed_ += (target - speed_) * 0.001;
        // Read slower than we write: the lag grows as the speed falls.
        read_ += 1.0 - speed_;
        if(read_ > d_[0].Size() - 16)
            read_ = d_[0].Size() - 16;
        d_[0].Write(l), d_[1].Write(r);
        float       x[2] = {d_[0].Read(float(read_) + 1.f), d_[1].Read(float(read_) + 1.f)};
        const float fc   = ExpMap(float(speed_), 200.f, 18000.f);
        for(int k = 0; k < 2; k++)
        {
            f_[k].SetFreq(fc), f_[k].SetRes(N(RESONANCE) * 0.8f);
            f_[k].Process(x[k]);
            x[k] = f_[k].Low() * float(speed_ > 0.02 ? 1.0 : speed_ * 50.0);
        }
        l = x[0], r = x[1];
    }
};

// 25 Downer: pitch and filter sweep down over RATE (2/1 - 1/32), repeating.
class Downer : public Effect
{
    PitchShifter ps_[2];
    Svf          f_[2];
    double       pos_ = 0.0;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ps_[0].Init(c.sr), ps_[1].Init(c.sr);
        f_[0].Init(c.sr), f_[1].Init(c.sr);
    }
    void Engage() override { pos_ = 0.0; }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx25;
        static const float kRate[7] = {8.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f};
        const float        len      = kRate[p_[RATE] % 7] * ctx_->Beat();
        pos_ += 1.0 / (len * Sr());
        if(pos_ >= 1.0)
            pos_ -= 1.0;
        const float fall = float(pos_) * N(DEPTH);
        const float semi = -fall * (p_[PITCH] ? 12.f + N(PITCH, 24.f) * 12.f : 24.f);
        float       x[2] = {l, r};
        for(int k = 0; k < 2; k++)
        {
            ps_[k].SetTransposition(semi);
            x[k] = ps_[k].Process(x[k]);
            f_[k].SetFreq(ExpMap(1.f - fall * N(FILTER), 150.f, 18000.f));
            f_[k].SetRes(N(RESONANCE) * 0.8f);
            f_[k].Process(x[k]);
            x[k] = f_[k].Low();
        }
        l = x[0], r = x[1];
    }
};

std::unique_ptr<Effect> MakeMod(int id)
{
    switch(id)
    {
        case 5: return std::make_unique<Phaser>();
        case 6: return std::make_unique<Flanger>();
        case 14: return std::make_unique<Chorus>();
        case 15: return std::make_unique<JunoChorus>();
        case 16: return std::make_unique<TremoloPan>();
        case 21: return std::make_unique<RingMod>();
        case 24: return std::make_unique<Stopper>();
        case 25: return std::make_unique<Downer>();
        case 26: return std::make_unique<Slicer>();
        case 38: return std::make_unique<ToGuRo>();
        default: return nullptr;
    }
}

} // namespace sp404fx
