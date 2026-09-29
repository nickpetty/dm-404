// Filters and EQs: Filter+Drive, Isolator, Super Filter, Equalizer, Wah,
// Crusher, Lo-fi, SBF.
//
// Value scales follow what the firmware displays for each knob (README.md).
#include "fx.h"
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

// 1 Filter+Drive: a resonant filter after a drive stage, with a low boost.
// CUTOFF 0-255 = 20 Hz-16 kHz (exponential), FLT TYPE 0 HPF / 1 LPF.
class FilterDrive : public Effect
{
    using P = Effect;
    Svf    f_[2];
    Biquad2 low_;
    Smooth cut_, res_, drive_;

  public:
    void Init(const Context &c) override
    {
        P::Init(c);
        for(auto &f : f_)
            f.Init(c.sr);
        cut_.Init(c.sr), res_.Init(c.sr), drive_.Init(c.sr);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx01;
        cut_.Set(ExpMap(N(CUTOFF), 20.f, 16000.f));
        res_.Set(N(RESONANCE) * 0.95f);
        drive_.Set(N(DRIVE));
        if(i < 0 || i == LOW_FREQ || i == LOW_GAIN)
            low_.Set(Biquad::LowShelf, Sr(), ExpMap(N(LOW_FREQ), 40.f, 400.f), 0.7f,
                     (N(LOW_GAIN) - 0.f) * 12.f);
        if(i < 0)
            cut_.Reset(ExpMap(N(CUTOFF), 20.f, 16000.f)), res_.Reset(N(RESONANCE) * 0.95f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx01;
        const float c = cut_.Next(), q = res_.Next(), d = drive_.Next();
        const float pre = 1.f + d * 24.f, post = 1.f / (1.f + d * 3.f);
        float       x[2] = {l, r};
        for(int k = 0; k < 2; k++)
        {
            f_[k].SetFreq(c);
            f_[k].SetRes(q);
            f_[k].Process(SoftClip(x[k] * pre) * post);
            x[k] = p_[FLT_TYPE] == 0 ? f_[k].High() : f_[k].Low();
        }
        low_.Process(x[0], x[1]);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = x[0] * lv, r = x[1] * lv;
    }
};

// 3 Isolator: three-band kill EQ. 0 = -INF, the middle 0 dB, 255 = +12 dB.
class Isolator : public Effect
{
    Biquad lo_[2][2], hi_[2][2]; // 4th-order Linkwitz-Riley splits
    Smooth g_[3];

    static float Gain(float n)
    {
        if(n <= 0.5f)
            return n < 0.004f ? 0.f : std::pow(n * 2.f, 2.5f);
        return DbToGain((n - 0.5f) * 24.f);
    }

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        for(int k = 0; k < 2; k++)
            for(int s = 0; s < 2; s++)
            {
                lo_[k][s].Set(Biquad::LowPass, c.sr, 250.f, 0.7071f);
                hi_[k][s].Set(Biquad::HighPass, c.sr, 2500.f, 0.7071f);
            }
        for(auto &g : g_)
            g.Init(c.sr, 15.f), g.Reset(1.f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx03;
        g_[0].Set(Gain(N(LOW))), g_[1].Set(Gain(N(MID))), g_[2].Set(Gain(N(HIGH)));
    }
    void Process(float &l, float &r) override
    {
        const float gl = g_[0].Next(), gm = g_[1].Next(), gh = g_[2].Next();
        float      *x[2] = {&l, &r};
        for(int k = 0; k < 2; k++)
        {
            const float in  = *x[k];
            const float low = lo_[k][1].Process(lo_[k][0].Process(in));
            const float hig = hi_[k][1].Process(hi_[k][0].Process(in));
            const float mid = in - low - hig;
            *x[k]           = low * gl + mid * gm + hig * gh;
        }
    }
};

// 34 Super Filter: LPF/BPF/HPF, 12 or 24 dB, with an LFO on the cutoff.
class SuperFilter : public Effect
{
    Svf        f_[2][2];
    Lfo        lfo_;
    Smooth     cut_, res_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        for(auto &a : f_)
            for(auto &f : a)
                f.Init(c.sr);
        lfo_.Init(c.sr);
        cut_.Init(c.sr), res_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx34;
        cut_.Set(N(CUTOFF)), res_.Set(N(RESONANCE));
        const int wave[] = {Oscillator::WAVE_TRI, Oscillator::WAVE_SQUARE, Oscillator::WAVE_SIN,
                            Oscillator::WAVE_SAW, Oscillator::WAVE_RAMP};
        lfo_.SetWaveform(uint8_t(wave[p_[MOD_WAVE] % 5]));
        const float bars = 4.f - N(RATE) * 3.99f;
        lfo_.SetFreq(p_[SYNC] ? 1.f / (bars * 4.f * ctx_->Beat()) : ExpMap(N(RATE), 0.05f, 20.f));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx34;
        const float m   = p_[MOD_SW] ? lfo_.Process() * N(DEPTH) * 0.5f : 0.f;
        const float c   = ExpMap(Clamp(cut_.Next() + m, 0.f, 1.f), 20.f, 18000.f);
        const float q   = res_.Next() * 0.95f;
        const int   pol = p_[FLT_SLOPE] ? 2 : 1;
        float       x[2] = {l, r};
        for(int k = 0; k < 2; k++)
            for(int s = 0; s < pol; s++)
            {
                f_[k][s].SetFreq(c), f_[k][s].SetRes(s ? q * 0.5f : q);
                f_[k][s].Process(x[k]);
                x[k] = p_[FLT_TYPE] == 0 ? f_[k][s].Low()
                       : p_[FLT_TYPE] == 1 ? f_[k][s].Band()
                                           : f_[k][s].High();
            }
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = x[0] * lv, r = x[1] * lv;
    }
};

// 19 Equalizer: low shelf, two peaks, high shelf. Gains 0-30 = -15..+15 dB.
class Equalizer : public Effect
{
    Biquad2 b_[4];

  public:
    void Changed(int) override
    {
        using namespace fxp::fx19;
        b_[0].Set(Biquad::LowShelf, Sr(), ExpMap(N(LOW_FREQ, 255), 20.f, 400.f), 0.7f, EqDb(LOW_GAIN));
        b_[1].Set(Biquad::Peak, Sr(), ExpMap(N(MID_FREQ), 200.f, 8000.f),
                  0.5f + N(MID_Q, 255) * 8.f, EqDb(MID_GAIN));
        b_[2].Set(Biquad::Peak, Sr(), ExpMap(N(MID2_FREQ), 200.f, 8000.f),
                  0.5f + N(MID2_Q, 255) * 8.f, EqDb(MID2_GAIN));
        b_[3].Set(Biquad::HighShelf, Sr(), ExpMap(N(HIGH_FREQ), 2000.f, 16000.f), 0.7f,
                  EqDb(HIGH_GAIN));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx19;
        const float dl = l, dr = r;
        for(auto &b : b_)
            b.Process(l, r);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        const float bl = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(dl, l * lv, bl), r = Mix(dr, r * lv, bl);
    }
};

// 17 Wah: a band-pass swept by an LFO (RATE in bars, 1 - 0.01) around MANUAL.
class Wah : public Effect
{
    Svf        f_[2];
    Lfo        lfo_;
    Smooth     man_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        f_[0].Init(c.sr), f_[1].Init(c.sr);
        lfo_.Init(c.sr);
        lfo_.SetWaveform(Oscillator::WAVE_TRI);
        man_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx17;
        const float bars = 1.f - N(RATE) * 0.99f;
        lfo_.SetFreq(1.f / (bars * 4.f * ctx_->Beat()));
        man_.Set(N(MANUAL));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx17;
        const float depth = p_[DEPTH] ? N(DEPTH) : 0.6f;
        const float pos   = Clamp(man_.Next() + lfo_.Process() * depth * 0.5f, 0.f, 1.f);
        const float fc    = ExpMap(pos, 250.f, 3500.f);
        float       x[2]  = {l, r};
        for(int k = 0; k < 2; k++)
        {
            const float res = 0.3f + N(PEAK) * 0.65f;
            f_[k].SetFreq(fc), f_[k].SetRes(res);
            f_[k].Process(x[k]);
            // The band output grows with the resonance: keep the level.
            x[k] = p_[FLT_TYPE] ? f_[k].Low() : f_[k].Band() * (1.2f - res);
        }
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        const float lv  = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, x[0] * lv, bal), r = Mix(r, x[1] * lv, bal);
    }
};

// 4 Crusher: sample-rate reduction (RATE) then a low-pass (FILTER, 331 Hz -
// 15.4 kHz on a square law).
class Crusher : public Effect
{
    float   hold_[2] = {}, phase_ = 0.f;
    Biquad2 lp_;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx04;
        const float n = N(FILTER);
        lp_.Set(Biquad::LowPass, Sr(), 331.f + 15061.f * n * n, 0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx04;
        const float step = 1.f - N(RATE) * 0.985f; // fraction of 48 kHz
        phase_ += step;
        if(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            // 12-bit steps as the rate drops, like an old sampler.
            const float q = 2048.f;
            hold_[0]      = std::round(l * q) / q;
            hold_[1]      = std::round(r * q) / q;
        }
        float wl = hold_[0], wr = hold_[1];
        lp_.Process(wl, wr);
        l = Mix(l, wl, N(BALANCE)), r = Mix(r, wr, N(BALANCE));
    }
};

// 20 Lo-fi: nine sampler characters (rate and bits), pre/post filters, TONE
// -100..+100 as a tilt.
class Lofi : public Effect
{
    float   hold_[2] = {}, phase_ = 0.f;
    Biquad2 pre_, post_, tilt_lo_, tilt_hi_;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx20;
        pre_.Set(Biquad::LowPass, Sr(), 18000.f / (1.f + p_[PRE_FILT] * 0.9f), 0.7071f);
        post_.Set(Biquad::LowPass, Sr(), 16000.f / (1.f + p_[POST_FILT] * 0.9f), 0.7071f);
        const float t = (N(TONE) - 0.5f) * 2.f;
        tilt_lo_.Set(Biquad::LowShelf, Sr(), 300.f, 0.7f, -t * 9.f);
        tilt_hi_.Set(Biquad::HighShelf, Sr(), 3000.f, 0.7f, t * 9.f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx20;
        // LOFI TYPE 0-8: from mild (32 kHz, 12 bit) to crushed (6 kHz, 6 bit).
        static const float rate[9] = {32000, 26000, 22050, 18000, 15000, 12000, 10000, 8000, 6000};
        static const float bits[9] = {12, 12, 10, 10, 9, 8, 8, 7, 6};
        const int          t       = p_[LOFI_TYPE] % 9;
        float              wl = l, wr = r;
        pre_.Process(wl, wr);
        phase_ += rate[t] / Sr();
        if(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            const float q = std::pow(2.f, bits[t] - 1.f);
            hold_[0] = std::round(wl * q) / q, hold_[1] = std::round(wr * q) / q;
        }
        wl = hold_[0], wr = hold_[1];
        post_.Process(wl, wr);
        tilt_lo_.Process(wl, wr);
        tilt_hi_.Process(wl, wr);
        const float lv  = p_[LEVEL] ? Level(LEVEL) : 1.f;
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, wl * lv, bal), r = Mix(r, wr * lv, bal);
    }
};

// 40 SBF: a comb of notches (INTERVAL spacing, WIDTH depth), the SP-404's
// "side band filter" sweep sound.
class Sbf : public Effect
{
    Delay  d_[2]{Delay(0.05f), Delay(0.05f)};
    Smooth iv_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        iv_.Init(c.sr, 30.f);
    }
    void Changed(int) override { iv_.Set(N(fxp::fx40::INTERVAL)); }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx40;
        const float spacing = ExpMap(iv_.Next(), 60.f, 2000.f); // Hz between notches
        const float dly     = Sr() / spacing;
        const float w       = N(WIDTH) * 0.95f;
        float      *x[2]    = {&l, &r};
        for(int k = 0; k < 2; k++)
        {
            const float in  = *x[k];
            const float out = in - w * d_[k].Read(dly * (k ? 1.01f : 1.f));
            d_[k].Write(in);
            *x[k] = Mix(in, out, N(BALANCE));
        }
    }
};

std::unique_ptr<Effect> MakeFilter(int id)
{
    switch(id)
    {
        case 1: return std::make_unique<FilterDrive>();
        case 3: return std::make_unique<Isolator>();
        case 4: return std::make_unique<Crusher>();
        case 17: return std::make_unique<Wah>();
        case 19: return std::make_unique<Equalizer>();
        case 20: return std::make_unique<Lofi>();
        case 34: return std::make_unique<SuperFilter>();
        case 40: return std::make_unique<Sbf>();
        default: return nullptr;
    }
}

} // namespace sp404fx
