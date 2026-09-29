// Drive, dynamics and media: Overdrive, Distortion, WrmSaturator, Gt Amp
// Sim, Compressor, COMPRESSOR 2, 404 VinylSim, 303 VinylSim, Cassette Sim.
#include "daisysp-lgpl.h"
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

namespace {

// TONE -100..+100 (0-255) as a tilt around 1 kHz, +-9 dB.
void SetTilt(Biquad2 &lo, Biquad2 &hi, float sr, float n)
{
    const float t = (n - 0.5f) * 2.f;
    lo.Set(Biquad::LowShelf, sr, 600.f, 0.7f, -t * 9.f);
    hi.Set(Biquad::HighShelf, sr, 1800.f, 0.7f, t * 9.f);
}

// Wow and flutter on a short delay: the pitch wobble of tape and vinyl.
class Wobble
{
  public:
    Delay      d[2]{Delay(0.05f), Delay(0.05f)};
    Lfo        wow, flut;

    void Init(float sr, float wow_hz, float flut_hz)
    {
        wow.Init(sr), flut.Init(sr);
        wow.SetWaveform(Oscillator::WAVE_SIN), flut.SetWaveform(Oscillator::WAVE_SIN);
        wow.SetFreq(wow_hz), flut.SetFreq(flut_hz);
    }
    void Process(float &l, float &r, float depth, float sr)
    {
        const float m = (wow.Process() * 1.2f + flut.Process() * 0.25f) * depth;
        const float t = (6.f + m * 4.f) * 0.001f * sr;
        d[0].Write(l), d[1].Write(r);
        l = d[0].Read(t), r = d[1].Read(t);
    }
};

// Crackle and hiss.
class Surface
{
  public:
    WhiteNoise n;
    Biquad2    hiss_f;
    uint32_t   seed = 22222;

    void  Init(float sr)
    {
        n.Init();
        hiss_f.Set(Biquad::BandPass, sr, 5000.f, 0.5f);
    }
    float Rand()
    {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / 16777216.f;
    }
    void Add(float &l, float &r, float crackle, float hiss)
    {
        // At full: hiss around -50 dBFS, crackles up to -20 dBFS.
        float hl = n.Process() * hiss * 0.004f, hr = n.Process() * hiss * 0.004f;
        hiss_f.Process(hl, hr);
        float c = 0.f;
        if(Rand() < crackle * 0.0006f)
            c = (Rand() - 0.5f) * 0.2f * crackle;
        l += hl + c, r += hr + c;
    }
};

} // namespace

// 7 Overdrive and 8 Distortion: a gain stage and clipper with TONE.
class Drive : public Effect
{
    Overdrive od_[2];
    Biquad2   lo_, hi_, pre_;
    bool      dist_;

  public:
    explicit Drive(bool dist) : dist_(dist) {}
    void Init(const Context &c) override
    {
        Effect::Init(c);
        od_[0].Init(), od_[1].Init();
        pre_.Set(Biquad::HighPass, c.sr, dist_ ? 120.f : 60.f, 0.7071f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx07; // same layout as fx08
        const float d = dist_ ? 0.45f + N(DRIVE) * 0.5f : 0.2f + N(DRIVE) * 0.55f;
        od_[0].SetDrive(d), od_[1].SetDrive(d);
        SetTilt(lo_, hi_, Sr(), N(TONE));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx07;
        float wl = l, wr = r;
        pre_.Process(wl, wr);
        wl = od_[0].Process(wl), wr = od_[1].Process(wr);
        if(dist_)
            wl = SoftClip(wl * 2.f) * 0.7f, wr = SoftClip(wr * 2.f) * 0.7f;
        // DaisySP's Overdrive makes up the gain it squashes; pads sit around
        // -17 dBFS, where that would be +25 dB. Keep it near the dry level.
        const float trim = 1.f / (1.f + (dist_ ? 10.f : 12.f) * N(DRIVE));
        wl *= trim, wr *= trim;
        lo_.Process(wl, wr);
        hi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 0.8f;
        l = Mix(l, wl * lv, N(BALANCE)), r = Mix(r, wr * lv, N(BALANCE));
    }
};

// 11 WrmSaturator: warm, asymmetric saturation. DRIVE 0-48 dB, EQ LOW and
// EQ HIGH 0-48 = -24..+24 dB after it.
class WrmSaturator : public Effect
{
    Biquad2 pre_, lo_, hi_;
    DcBlock dc_[2];

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        dc_[0].Init(c.sr), dc_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx11;
        pre_.Set(Biquad::HighPass, Sr(), 30.f, 0.7071f);
        lo_.Set(Biquad::LowShelf, Sr(), 200.f, 0.7f, float(p_[EQ_LOW] - 24));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, float(p_[EQ_HIGH] - 24));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx11;
        const float g    = DbToGain(float(p_[DRIVE]));
        const float comp = 1.f / std::sqrt(g);
        float       x[2] = {l, r};
        pre_.Process(x[0], x[1]);
        for(int k = 0; k < 2; k++)
        {
            const float y = x[k] * g;
            // Asymmetric: the positive side saturates a little harder.
            x[k] = dc_[k].Process(y > 0 ? SoftClip(y * 1.1f) / 1.1f : SoftClip(y * 0.9f) / 0.9f) * comp;
        }
        lo_.Process(x[0], x[1]);
        hi_.Process(x[0], x[1]);
        const float lv  = p_[LEVEL] ? Level(LEVEL) : 1.f;
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, x[0] * lv, bal), r = Mix(r, x[1] * lv, bal);
    }
};

// 23 Gt Amp Sim (input FX): preamp gain, a tone stack and a speaker's
// band limit.
class GtAmp : public Effect
{
    Biquad2 bass_, mid_, treb_, pres_, cab_lo_, cab_hi_;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx23;
        bass_.Set(Biquad::LowShelf, Sr(), 120.f, 0.7f, (N(BASS) - 0.5f) * 20.f);
        mid_.Set(Biquad::Peak, Sr(), 700.f, 0.8f, (N(MIDDLE) - 0.5f) * 20.f);
        treb_.Set(Biquad::HighShelf, Sr(), 2500.f, 0.7f, (N(TREBLE) - 0.5f) * 20.f);
        pres_.Set(Biquad::Peak, Sr(), 4500.f, 1.f, N(PRESENCE) * 8.f);
        cab_lo_.Set(Biquad::HighPass, Sr(), 90.f, 0.7071f);
        cab_hi_.Set(Biquad::LowPass, Sr(), 4800.f, 0.9f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx23;
        const float g = 1.f + N(DRIVE) * 40.f;
        float       wl = SoftClip(l * g), wr = SoftClip(r * g);
        bass_.Process(wl, wr), mid_.Process(wl, wr), treb_.Process(wl, wr), pres_.Process(wl, wr);
        cab_lo_.Process(wl, wr), cab_hi_.Process(wl, wr); // the speaker
        const float lv = (p_[LEVEL] ? Level(LEVEL) : 0.8f) * (p_[MASTER] ? 0.3f + N(MASTER) : 1.f);
        // The clipped preamp sits near full scale: bring it back to the
        // level it came in at.
        l = wl * lv * 0.15f, r = wr * lv * 0.15f;
    }
};

// 28 Compressor and 29 COMPRESSOR 2 (DaisySP's Compressor). SUSTAIN lowers
// the threshold, ATTACK 0.1-80 ms, RATIO 1:1 - 20:1.
class Comp : public Effect
{
    Compressor c_[2];
    Biquad2    lo_, hi_;
    bool       two_;

  public:
    explicit Comp(bool two) : two_(two) {}
    void Init(const Context &c) override
    {
        Effect::Init(c);
        c_[0].Init(c.sr), c_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx28; // SUSTAIN, ATTACK, LEVEL, RATIO as fx29
        for(auto &c : c_)
        {
            c.SetThreshold(-N(SUSTAIN) * 40.f);
            c.SetRatio(1.f + N(RATIO) * 19.f);
            c.SetAttack(0.0001f + N(ATTACK) * 0.08f);
            c.SetRelease(0.15f);
            c.AutoMakeup(false);
            // Half the reduction at threshold, at most 6 dB.
            const float thr = -N(SUSTAIN) * 40.f, ratio = 1.f + N(RATIO) * 19.f;
            c.SetMakeup(std::fmin(std::fabs(thr - thr / ratio) * 0.5f, 6.f));
        }
        if(two_)
            SetTilt(lo_, hi_, Sr(), N(fxp::fx29::TONE));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx28;
        // Linked: both sides follow the louder one.
        const float key = std::fabs(l) > std::fabs(r) ? l : r;
        c_[0].Process(key);
        l = c_[0].Apply(l), r = c_[0].Apply(r);
        if(two_)
            lo_.Process(l, r), hi_.Process(l, r);
        if(p_[LEVEL])
            l *= Level(LEVEL) * 1.3f, r *= Level(LEVEL) * 1.3f;
    }
};

// 31 404 VinylSim: the SP-404's record: band limit (FREQUENCY), surface
// noise, wow and flutter, distortion.
class Vinyl404 : public Effect
{
    Wobble  wob_;
    Surface surf_;
    Biquad2 bw_lo_, bw_hi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        wob_.Init(c.sr, 0.55f, 7.f);
        surf_.Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx31;
        bw_lo_.Set(Biquad::HighPass, Sr(), 20.f + (1.f - N(FREQUENCY)) * 180.f, 0.7071f);
        bw_hi_.Set(Biquad::LowPass, Sr(), ExpMap(N(FREQUENCY), 2500.f, 18000.f), 0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx31;
        float wl = l, wr = r;
        wob_.Process(wl, wr, N(WOW_FLUT), Sr());
        const float d = 1.f + N(DIST) * 6.f;
        wl = SoftClip(wl * d) / d * 1.2f, wr = SoftClip(wr * d) / d * 1.2f;
        bw_lo_.Process(wl, wr);
        bw_hi_.Process(wl, wr);
        surf_.Add(wl, wr, N(NOISE), N(NOISE) * 0.5f);
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

// 32 303 VinylSim: the SP-303's vinyl sim: squashed (COMP), noisy, wobbly.
class Vinyl303 : public Effect
{
    Wobble     wob_;
    Surface    surf_;
    Compressor c_;
    Biquad2    bw_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        wob_.Init(c.sr, 0.5f, 6.5f);
        surf_.Init(c.sr);
        c_.Init(c.sr);
        bw_.Set(Biquad::LowPass, c.sr, 9000.f, 0.8f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx32;
        c_.SetThreshold(-N(COMP) * 30.f);
        c_.SetRatio(1.f + N(COMP) * 7.f);
        c_.SetAttack(0.005f), c_.SetRelease(0.1f);
        c_.AutoMakeup(false);
        c_.SetMakeup(N(COMP) * 6.f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx32;
        float wl = l, wr = r;
        c_.Process((wl + wr) * 0.5f);
        wl = c_.Apply(wl), wr = c_.Apply(wr);
        wob_.Process(wl, wr, N(WOW_FLUT), Sr());
        bw_.Process(wl, wr);
        surf_.Add(wl, wr, N(NOISE), N(NOISE) * 0.3f);
        const float lv  = p_[LEVEL] ? Level(LEVEL) : 1.f;
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, wl * lv, bal), r = Mix(r, wr * lv, bal);
    }
};

// 39 Cassette Sim: tape hiss (HISS), age 0-60 years (loses highs, more
// wobble), TONE, saturation, CATCH (occasional drop-outs).
class Cassette : public Effect
{
    Wobble   wob_;
    Surface  surf_;
    Biquad2  age_, lo_, hi_;
    Smooth   drop_;
    uint32_t seed_ = 1;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        wob_.Init(c.sr, 0.8f, 9.f);
        surf_.Init(c.sr);
        drop_.Init(c.sr, 30.f);
        drop_.Reset(1.f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx39;
        age_.Set(Biquad::LowPass, Sr(), 16000.f - N(AGE, 60.f) * 12000.f, 0.7071f);
        SetTilt(lo_, hi_, Sr(), N(TONE));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx39;
        float wl = l, wr = r;
        wob_.Process(wl, wr, N(WOW_FLUT) + N(AGE, 60.f) * 0.3f, Sr());
        const float d = 1.f + N(DRIVE) * 5.f;
        wl = SoftClip(wl * d) / d * 1.1f, wr = SoftClip(wr * d) / d * 1.1f;
        age_.Process(wl, wr);
        lo_.Process(wl, wr), hi_.Process(wl, wr);
        seed_ = seed_ * 1664525u + 1013904223u;
        if((seed_ >> 12) % 48000 < unsigned(N(CATCH) * 3.f))
            drop_.Reset(0.3f);
        drop_.Set(1.f);
        const float g = drop_.Next();
        wl *= g, wr *= g;
        surf_.Add(wl, wr, 0.f, N(HISS) * 2.f);
        const float bal = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

std::unique_ptr<Effect> MakeDrive(int id)
{
    switch(id)
    {
        case 7: return std::make_unique<Drive>(false);
        case 8: return std::make_unique<Drive>(true);
        case 11: return std::make_unique<WrmSaturator>();
        case 23: return std::make_unique<GtAmp>();
        case 28: return std::make_unique<Comp>(false);
        case 29: return std::make_unique<Comp>(true);
        case 31: return std::make_unique<Vinyl404>();
        case 32: return std::make_unique<Vinyl303>();
        case 39: return std::make_unique<Cassette>();
        default: return nullptr;
    }
}

} // namespace sp404fx
