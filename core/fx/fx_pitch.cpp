// Pitch and resonance: Resonator, Hyper-Reso, Chromatic PS, Auto Pitch,
// Harmony, Vocoder, Sub Sonic.
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

namespace {

float MidiHz(float note)
{
    return 440.f * std::pow(2.f, (note - 69.f) / 12.f);
}

// Chord shapes (semitones) for the resonators' CHORD.
constexpr int kChords[8][4] = {{0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 10}, {0, 3, 7, 10},
                               {0, 4, 7, 11}, {0, 5, 7, 12}, {0, 7, 12, 19}, {0, 12, 24, 0}};

// A tuned feedback comb with a damping filter: one resonator voice.
class Comb
{
  public:
    Delay  d{0.06f};
    Biquad damp;
    float  period = 100.f, fb = 0.9f;

    void  Tune(float sr, float hz) { period = Clamp(sr / hz, 2.f, 2800.f); }
    float Process(float x)
    {
        const float y = d.Read(period);
        d.Write(x + damp.Process(y) * fb);
        return y;
    }
};

} // namespace

// 12 Resonator: four combs on a chord built on ROOT (a MIDI note),
// FEEDBACK 0-99 %, BRIGHT opens the damping; ENV MOD lets the input's level
// raise the feedback.
class Resonator : public Effect
{
    Comb    c_[2][4];
    Biquad2 lowcut_;
    float   env_ = 0.f;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx12;
        const int *ch = kChords[p_[CHORD] & 7];
        for(int k = 0; k < 2; k++)
            for(int v = 0; v < 4; v++)
            {
                c_[k][v].Tune(Sr(), MidiHz(float(p_[ROOT] + ch[v]) + (k ? 0.05f : 0.f)));
                c_[k][v].damp.Set(Biquad::LowPass, Sr(), ExpMap(N(BRIGHT), 800.f, 16000.f), 0.6f);
            }
        lowcut_.Set(Biquad::HighPass, Sr(), p_[LOW_CUT_F] ? ExpMap(N(LOW_CUT_F), 20.f, 800.f) : 20.f,
                    0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx12;
        const float a = std::fabs(l) + std::fabs(r);
        env_ += (a > env_ ? 0.01f : 0.0003f) * (a - env_);
        const float fb = Clamp(0.8f + N(FEEDBACK) * 0.195f + env_ * N(ENV_MOD) * 0.1f, 0.f, 0.998f);
        float       wl = 0.f, wr = 0.f;
        for(int v = 0; v < 4; v++)
        {
            c_[0][v].fb = c_[1][v].fb = fb;
            wl += c_[0][v].Process(l * 0.1f);
            wr += c_[1][v].Process(r * 0.1f);
        }
        lowcut_.Process(wl, wr);
        const float bal = p_[BALANCE] ? N(BALANCE) : 0.5f;
        l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

// 35 Hyper-Reso: resonators on the notes of a scale around NOTE (-17..+17
// semitones from C4), SPREAD (unison ... huge) fanning them out in stereo,
// CHARACTER their brightness.
class HyperReso : public Effect
{
    Comb    c_[2][4];
    Biquad2 lowcut_;
    float   env_ = 0.f;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx35;
        static const int kScale[2][4] = {{0, 4, 7, 11}, {0, 3, 7, 10}};
        const int        root = 60 + p_[NOTE] - 17;
        const float      sp   = float(p_[SPREAD] % 5) * 0.08f;
        for(int k = 0; k < 2; k++)
            for(int v = 0; v < 4; v++)
            {
                const float det = (k ? 1.f : -1.f) * sp * float(v + 1);
                c_[k][v].Tune(Sr(), MidiHz(float(root + kScale[p_[SCALE] & 1][v] + 12 * (v & 1)) + det));
                c_[k][v].damp.Set(Biquad::LowPass, Sr(), ExpMap(N(CHARACTER), 1500.f, 18000.f), 0.6f);
            }
        lowcut_.Set(Biquad::HighPass, Sr(), p_[LOW_CUT_F] ? ExpMap(N(LOW_CUT_F), 20.f, 800.f) : 60.f,
                    0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx35;
        const float a = std::fabs(l) + std::fabs(r);
        env_ += (a > env_ ? 0.01f : 0.0003f) * (a - env_);
        const float fb = Clamp(0.9f + (p_[FEEDBACK] ? N(FEEDBACK) : 0.5f) * 0.095f
                                   + env_ * N(ENV_MOD) * 0.05f,
                               0.f, 0.998f);
        float wl = 0.f, wr = 0.f;
        for(int v = 0; v < 4; v++)
        {
            c_[0][v].fb = c_[1][v].fb = fb;
            wl += c_[0][v].Process(l * 0.08f);
            wr += c_[1][v].Process(r * 0.08f);
        }
        lowcut_.Process(wl, wr);
        const float bal = p_[BALANCE] ? N(BALANCE) : 0.5f;
        l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

// 22 Chromatic PS: two pitch shifters, PITCH -24..+12 semitones (0-36),
// FINE, PAN and LEVEL each.
class ChromaticPs : public Effect
{
    PitchShifter ps_[2];
    Biquad2      lo_, hi_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ps_[0].Init(c.sr), ps_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx22;
        ps_[0].SetTransposition(float(p_[PITCH1] - 24) + (N(FINE1) - 0.5f));
        ps_[1].SetTransposition(float(p_[PITCH2] - 24) + (N(FINE2) - 0.5f));
        lo_.Set(Biquad::LowShelf, Sr(), 250.f, 0.7f, EqDb(EQ_LOW));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, EqDb(EQ_HIGH));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx22;
        const float m  = (l + r) * 0.5f;
        const float a  = ps_[0].Process(m), b = ps_[1].Process(m);
        const float la = p_[LEVEL1] ? Level(LEVEL1) : 1.f, lb = p_[LEVEL2] ? Level(LEVEL2) : 1.f;
        const float pa = p_[PAN1] ? N(PAN1) : 0.f, pb = p_[PAN2] ? N(PAN2) : 1.f;
        float       wl = a * la * std::sqrt(1.f - pa) + b * lb * std::sqrt(1.f - pb);
        float       wr = a * la * std::sqrt(pa) + b * lb * std::sqrt(pb);
        lo_.Process(wl, wr), hi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, wl * lv, N(BALANCE)), r = Mix(r, wr * lv, N(BALANCE));
    }
};

// 27 Auto Pitch and 43 Harmony (input FX): a shifted voice. Real pitch
// correction is beyond this stand-in; PITCH shifts, ROBOT flattens by
// ring-locking, HARMONY adds the interval on top of the dry voice.
class Voice : public Effect
{
    PitchShifter ps_[2];
    bool         harmony_;

  public:
    explicit Voice(bool harmony) : harmony_(harmony) {}
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ps_[0].Init(c.sr), ps_[1].Init(c.sr);
    }
    void Changed(int) override
    {
        using namespace fxp::fx43;
        static const float kIv[8] = {3, 4, 5, 7, 8, 9, 12, -12};
        const float        semi   = harmony_ ? kIv[p_[HARMONY] & 7] : (N(fxp::fx27::PITCH) - 0.5f) * 24.f;
        ps_[0].SetTransposition(semi), ps_[1].SetTransposition(semi);
    }
    void Process(float &l, float &r) override
    {
        const float wl = ps_[0].Process(l), wr = ps_[1].Process(r);
        const float bal = N(harmony_ ? int(fxp::fx43::BALANCE) : int(fxp::fx27::BALANCE));
        if(harmony_)
            l += wl * bal, r += wr * bal;
        else
            l = Mix(l, wl, bal), r = Mix(r, wr, bal);
    }
};

// 42 Vocoder (input FX): the input's spectrum shapes a chord of saws on
// NOTE; 12 bands.
class Vocoder : public Effect
{
    static constexpr int kBands = 12;
    Biquad               mod_[kBands], car_[kBands];
    float                env_[kBands] = {};
    Oscillator           saw_[3];

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        for(int b = 0; b < kBands; b++)
        {
            const float f = ExpMap(b / float(kBands - 1), 120.f, 7000.f);
            mod_[b].Set(Biquad::BandPass, c.sr, f, 5.f);
            car_[b].Set(Biquad::BandPass, c.sr, f, 5.f);
        }
        for(auto &s : saw_)
            s.Init(c.sr), s.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW), s.SetAmp(0.3f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx42;
        const int *ch = kChords[p_[CHORD] & 7];
        for(int v = 0; v < 3; v++)
            saw_[v].SetFreq(MidiHz(float(36 + p_[NOTE] % 48 + ch[v])));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx42;
        const float in  = (l + r) * 0.5f;
        const float car = saw_[0].Process() + saw_[1].Process() + saw_[2].Process();
        float       out = 0.f;
        for(int b = 0; b < kBands; b++)
        {
            const float m = std::fabs(mod_[b].Process(in));
            env_[b] += (m > env_[b] ? 0.02f : 0.002f) * (m - env_[b]);
            out += car_[b].Process(car) * env_[b] * 25.f;
        }
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, out * lv, p_[BALANCE] ? N(BALANCE) : 1.f);
        r = Mix(r, out * lv, p_[BALANCE] ? N(BALANCE) : 1.f);
    }
};

// 45 Sub Sonic: adds an octave below, filtered to the lows.
class SubSonic : public Effect
{
    PitchShifter ps_;
    Biquad       lp_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ps_.Init(c.sr);
        ps_.SetTransposition(-12.f);
        lp_.Set(Biquad::LowPass, c.sr, 150.f, 0.7071f);
    }
    void Process(float &l, float &r) override
    {
        const float s = lp_.Process(ps_.Process((l + r) * 0.5f)) * 2.f * N(fxp::fx45::BALANCE);
        l += s, r += s;
    }
};

std::unique_ptr<Effect> MakePitch(int id)
{
    switch(id)
    {
        case 12: return std::make_unique<Resonator>();
        case 22: return std::make_unique<ChromaticPs>();
        case 27: return std::make_unique<Voice>(false);
        case 35: return std::make_unique<HyperReso>();
        case 42: return std::make_unique<Vocoder>();
        case 43: return std::make_unique<Voice>(true);
        case 45: return std::make_unique<SubSonic>();
        default: return nullptr;
    }
}

std::unique_ptr<Effect> MakeEffect(int id)
{
    for(auto make : {MakeFilter, MakeMod, MakeDelay, MakeReverb, MakeDrive, MakePitch})
        if(auto fx = make(id))
            return fx;
    return nullptr;
}

} // namespace sp404fx
