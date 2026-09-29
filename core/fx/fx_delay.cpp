// Delays and loopers: TimeCtrlDly, Tape Echo, Sync Delay, Ko-Da-Ma,
// SX Delay, Zan-Zou, Cloud Delay, DJFX Delay, DJFX Looper, Back Spin,
// Scatter.
#include "fx_all.h"

namespace sp404fx {

using namespace daisysp;

namespace {

// HF DAMP / HIGH CUT frequencies (Zan-Zou shows 200 Hz ... OFF), index 0-17.
float DampHz(int i)
{
    static const float f[18] = {200,  250,  315,  400,  500,  630,  800,  1000, 1250,
                                1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 22000};
    return f[i < 0 ? 0 : i > 17 ? 17 : i];
}

// Low damp / low cut: 0 = flat, then 20 Hz up.
float LowDampHz(int i)
{
    return i <= 0 ? 10.f : ExpMap(i / 17.f, 20.f, 800.f);
}

// TIME 0-255 in milliseconds: 10-800 ms on a square law (the firmware shows
// 10 / 211 / 800 at 0 / 132 / 255).
float TimeMs(float n)
{
    return 10.f + 790.f * n * n;
}

// A stereo feedback delay with damping in the loop.
class Echo
{
  public:
    Delay   d[2]{Delay(6.f), Delay(6.f)};
    Biquad2 hi, lo;
    Smooth  time;
    float   fb = 0.f;

    void Init(float sr) { time.Init(sr, 120.f); }
    void SetDamp(float sr, float hi_hz, float lo_hz)
    {
        hi.Set(Biquad::LowPass, sr, hi_hz, 0.7071f);
        lo.Set(Biquad::HighPass, sr, lo_hz, 0.7071f);
    }
    // One frame; returns the wet signal. spread offsets the right side.
    void Process(float inl, float inr, float &wl, float &wr, float spread = 1.f, bool ping = false)
    {
        const float t = time.Next();
        wl            = d[0].Read(t);
        wr            = d[1].Read(t * spread);
        float fl = wl * fb, fr = wr * fb;
        hi.Process(fl, fr);
        lo.Process(fl, fr);
        if(ping)
            std::swap(fl, fr);
        d[0].Write(SoftClip(inl + fl));
        d[1].Write(SoftClip(inr + fr));
    }
};

} // namespace

// 2 TimeCtrlDly: turning TIME glides the delay (and so bends its pitch).
class TimeCtrlDly : public Effect
{
    Echo e_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        e_.Init(c.sr);
        e_.time.Init(c.sr, 400.f);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx02;
        const float t = p_[SYNC] ? kSyncBeats[int(N(TIME) * 15.f + 0.5f)] * ctx_->Beat() * Sr()
                                 : TimeMs(N(TIME)) * 0.001f * Sr();
        e_.time.Set(t);
        if(i < 0)
            e_.time.Reset(t);
        e_.fb = N(FEEDBACK) * 0.99f;
        e_.SetDamp(Sr(), DampHz(p_[H_DAMP_F]), LowDampHz(p_[L_DAMP_F]));
    }
    void Process(float &l, float &r) override
    {
        float wl, wr;
        e_.Process(l, r, wl, wr);
        const float lv = Level(fxp::fx02::LEVEL);
        l += wl * lv, r += wr * lv;
    }
};

// 9 Tape Echo: three playback heads (short/mid/long at 1:2:3), tape
// saturation, wow and flutter, bass/treble in the loop.
class TapeEcho : public Effect
{
    Delay      d_[2]{Delay(4.f), Delay(4.f)};
    Lfo        wow_, flut_;
    Smooth     time_;
    Biquad2    bass_, treb_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        wow_.Init(c.sr), flut_.Init(c.sr);
        wow_.SetWaveform(Oscillator::WAVE_SIN), flut_.SetWaveform(Oscillator::WAVE_SIN);
        time_.Init(c.sr, 200.f);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx09;
        const float t = p_[SYNC] ? kSyncBeats[int(N(TIME) * 15.f + 0.5f)] * ctx_->Beat()
                                 : TimeMs(N(TIME)) * 0.001f;
        time_.Set(t * Sr());
        if(i < 0)
            time_.Reset(t * Sr());
        wow_.SetFreq(0.3f + N(W_F_RATE) * 2.f);
        flut_.SetFreq(6.f + N(W_F_RATE) * 10.f);
        bass_.Set(Biquad::LowShelf, Sr(), 300.f, 0.7f, EqDb(BASS));
        treb_.Set(Biquad::HighShelf, Sr(), 3000.f, 0.7f, EqDb(TREBLE) - 3.f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx09;
        const float wf = (wow_.Process() * 0.004f + flut_.Process() * 0.0008f) * (0.3f + N(W_F_DEPTH));
        const float t  = time_.Next() * (1.f + wf);
        // MODE 0-6: S, M, L, S+M, S+L, M+L, S+M+L (the RE-201's head selector).
        static const uint8_t kHeads[7] = {1, 2, 4, 3, 5, 6, 7};
        const uint8_t        heads     = kHeads[p_[MODE] % 7];
        const float          pan[3]    = {N(PAN_S), N(PAN_M), N(PAN_L)};
        float                wl = 0.f, wr = 0.f, fb = 0.f;
        const float          norm = 1.f / float((heads & 1) + ((heads >> 1) & 1) + ((heads >> 2) & 1));
        for(int h = 0; h < 3; h++)
            if(heads & (1 << h))
            {
                const float x  = (d_[0].Read(t * (h + 1) / 3.f) + d_[1].Read(t * (h + 1) / 3.f)) * 0.5f;
                const float pn = p_[PAN_S] || p_[PAN_M] || p_[PAN_L] ? pan[h] : 0.5f;
                wl += x * std::sqrt(1.f - pn) * 1.41f * norm;
                wr += x * std::sqrt(pn) * 1.41f * norm;
                fb += x * norm;
            }
        float fl = fb * N(FEEDBACK) * 0.95f, fr = fl;
        bass_.Process(fl, fr);
        treb_.Process(fl, fr);
        const float drive = 1.f + N(TAPE_DIST) * 4.f;
        d_[0].Write(SoftClip((l + fl) * drive) / drive);
        d_[1].Write(SoftClip((r + fr) * drive) / drive);
        const float lv = Level(LEVEL);
        l += wl * lv, r += wr * lv;
    }
};

// 18 Sync Delay: TIME 0-15 is a note length (1/32 ... 1/1).
class SyncDelay : public Effect
{
    Echo e_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        e_.Init(c.sr);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx18;
        const float t = kSyncBeats[p_[TIME] & 15] * ctx_->Beat() * Sr();
        e_.time.Set(t);
        if(i < 0)
            e_.time.Reset(t);
        e_.fb = N(FEEDBACK) * 0.99f;
        e_.SetDamp(Sr(), DampHz(p_[H_DAMP_F] ? p_[H_DAMP_F] : 17), LowDampHz(p_[L_DAMP_F]));
    }
    void Process(float &l, float &r) override
    {
        float wl, wr;
        e_.Process(l, r, wl, wr);
        const float lv = Level(fxp::fx18::LEVEL);
        l += wl * lv, r += wr * lv;
    }
};

// 36 Ko-Da-Ma: an echo fed by SEND, so it can be thrown in and left to
// ring; MODE 1 is ping-pong.
class KoDaMa : public Effect
{
    Echo e_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        e_.Init(c.sr);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx36;
        const float t = kSyncBeats[p_[TIME] & 15] * ctx_->Beat() * Sr();
        e_.time.Set(t);
        if(i < 0)
            e_.time.Reset(t);
        e_.fb = N(FEEDBACK) * 0.99f;
        e_.SetDamp(Sr(), DampHz(p_[H_DAMP_F] ? p_[H_DAMP_F] : 17), LowDampHz(p_[L_DAMP_F]));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx36;
        const float s = N(SEND);
        float       wl, wr;
        e_.Process(l * s, r * s, wl, wr, 1.f, p_[MODE] != 0);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l += wl * lv, r += wr * lv;
    }
};

// 41 SX Delay: stereo delay with PHASE offset between the sides, feedback
// that can cross (FBK ROUTE), damping and chorus-like modulation.
class SxDelay : public Effect
{
    Echo       e_;
    Lfo        mod_;
    Biquad2    lo_, hi_;
    float      base_ = 0.f;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        e_.Init(c.sr);
        mod_.Init(c.sr);
        mod_.SetWaveform(Oscillator::WAVE_SIN);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx41;
        base_ = kSyncBeats[int(N(TIME) * 15.f + 0.5f)] * ctx_->Beat() * Sr();
        e_.time.Set(base_);
        if(i < 0)
            e_.time.Reset(base_);
        e_.fb = N(FEEDBACK) * 0.99f;
        e_.SetDamp(Sr(), p_[HF_DAMP] ? DampHz(p_[HF_DAMP]) : 22000.f, 20.f);
        mod_.SetFreq(ExpMap(N(MOD_RATE), 0.1f, 5.f));
        lo_.Set(Biquad::LowShelf, Sr(), 250.f, 0.7f, EqDb(EQ_LOW));
        hi_.Set(Biquad::HighShelf, Sr(), 4000.f, 0.7f, EqDb(EQ_HIGH));
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx41;
        if(p_[MOD_SW])
            e_.time.Set(base_ * (1.f + mod_.Process() * N(MOD_DEPTH) * 0.002f));
        float wl, wr;
        e_.Process(l, r, wl, wr, 1.f + (N(PHASE) - 0.5f) * 0.5f, p_[FBK_ROUTE] != 0);
        lo_.Process(wl, wr);
        hi_.Process(wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = Mix(l, l + wl * lv, N(BALANCE)), r = Mix(r, r + wr * lv, N(BALANCE));
    }
};

// 33 Zan-Zou: two multi-tap delays (main and sub), each with four taps
// panned across the field: afterimages.
class ZanZou : public Effect
{
    Delay   d_{6.f};
    Biquad2 damp_;
    float   fbk_ = 0.f;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx33;
        damp_.Set(Biquad::LowPass, Sr(), DampHz(p_[HF_DAMP]), 0.7071f);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx33;
        const float beat = ctx_->Beat() * Sr();
        const float t    = p_[SYNC] ? kSyncBeats[int(N(TIME) * 15.f + 0.5f)] * beat
                                    : (20.f + N(TIME) * 480.f) * 0.001f * Sr();
        const float tm = p_[M_D_SYNC] ? kSyncBeats[p_[M_D_TIME] & 15] * beat : t * 1.5f;
        const float pans[8] = {N(S_D_PAN1), N(S_D_PAN2), N(S_D_PAN3), N(S_D_PAN4),
                               N(M_D_PAN1), N(M_D_PAN2), N(M_D_PAN3), N(M_D_PAN4)};
        float       wl = 0.f, wr = 0.f;
        for(int k = 0; k < 8; k++)
        {
            const float x   = d_.Read((k < 4 ? t : tm) * float((k & 3) + 1) * 0.25f + 1.f);
            const float g   = (k < 4 ? Level(LEVEL) : (p_[M_D_LEVEL] ? Level(M_D_LEVEL) * 0.6f : 0.f)) * 0.45f;
            const float pan = (pans[0] + pans[1] + pans[2] + pans[3] == 0.f) ? (k & 3) / 3.f : pans[k];
            wl += x * g * std::sqrt(1.f - pan);
            wr += x * g * std::sqrt(pan);
        }
        float fl = d_.Read(t) * N(FEEDBACK) * 0.95f, fr = fl;
        damp_.Process(fl, fr);
        d_.Write((l + r) * 0.5f + fl);
        l += wl, r += wr;
    }
};

// 47 Cloud Delay: a pitch-shifted, diffused, washed-out delay. WINDOW sets
// the delay, PITCH (-12..+12 in 0.2 steps) shifts every repeat.
class CloudDelay : public Effect
{
    Delay        d_[2]{Delay(3.f), Delay(3.f)};
    PitchShifter ps_[2];
    Biquad2      soft_;
    float        hold_[2] = {}, ph_ = 0.f;
    Smooth       time_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        ps_[0].Init(c.sr), ps_[1].Init(c.sr);
        time_.Init(c.sr, 200.f);
        soft_.Set(Biquad::LowPass, c.sr, 6000.f, 0.6f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx47;
        const float semi = (p_[PITCH] - 60) * 0.2f;
        ps_[0].SetTransposition(semi), ps_[1].SetTransposition(semi);
        time_.Set((60.f + N(WINDOW) * 1500.f) * 0.001f * Sr());
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx47;
        const float t  = time_.Next();
        float       wl = d_[0].Read(t), wr = d_[1].Read(t * 1.07f);
        // CLOUDY: smear the repeats across the two sides.
        const float c = N(CLOUDY) * 0.5f;
        const float xl = wl + (wr - wl) * c, xr = wr + (wl - wr) * c;
        wl = ps_[0].Process(xl), wr = ps_[1].Process(xr);
        soft_.Process(wl, wr);
        if(p_[LOFI])
        {
            ph_ += 1.f - N(LOFI) * 0.8f;
            if(ph_ >= 1.f)
                ph_ -= 1.f, hold_[0] = wl, hold_[1] = wr;
            wl = hold_[0], wr = hold_[1];
        }
        const float fb = p_[FEEDBACK] ? N(FEEDBACK) * 0.9f : 0.5f;
        d_[0].Write(l + wl * fb), d_[1].Write(r + wr * fb);
        l = Mix(l, wl * 1.8f, N(BALANCE)), r = Mix(r, wr * 1.8f, N(BALANCE));
    }
};

// A loop grabbed from the incoming audio, replayed at a speed (negative:
// backwards). The DJFX effects use it. Started with less than a loop's
// worth heard (just engaged), it records the next loop first.
class Grab
{
  public:
    Delay  d[2]{Delay(8.f), Delay(8.f)};
    double pos  = 0.0; // playback position through the loop, oldest first
    float  len  = 0.f; // loop length, samples
    bool   held = false;
    float  fill = 0.f; // samples still to record before looping
    float  seen = 0.f; // samples heard since Reset

    void Reset() { held = false, fill = 0.f, seen = 0.f; }
    void Write(float l, float r)
    {
        if(held && fill <= 0.f)
            return;
        d[0].Write(l), d[1].Write(r);
        seen += 1.f;
        if(held)
            fill -= 1.f;
    }
    void Start(float length)
    {
        held = true, len = length, pos = 0.0;
        fill = seen >= length ? 0.f : length;
    }
    // Still recording: the caller passes the audio through.
    bool Filling() const { return held && fill > 0.f; }
    // Read and advance by speed (1 = forward at pitch).
    void Read(float &l, float &r, float speed)
    {
        // pos runs 0..len through the loop, oldest first.
        const float back = len - float(pos) + 1.f;
        l                = d[0].Read(back);
        r                = d[1].Read(back);
        pos += speed;
        while(pos >= len)
            pos -= len;
        while(pos < 0.0)
            pos += len;
    }
};

// 13 DJFX Looper: with LOOP SW on, loop the last LENGTH (0.230-0.012 s)
// at SPEED (-100..+100: backwards to forwards).
class DjfxLooper : public Effect
{
    Grab   g_;
    Smooth speed_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        speed_.Init(c.sr, 50.f);
    }
    void Changed(int) override
    {
        using namespace fxp::fx13;
        speed_.Set((p_[SPEED] - 100) / 100.f);
        const float len = (0.230f - N(LENGTH) * 0.218f) * Sr();
        if(p_[LOOP_SW] && !g_.held)
            g_.Start(len);
        else if(!p_[LOOP_SW])
            g_.held = false;
        g_.len = len;
    }
    void Engage() override
    {
        g_.Reset();
        Changed(-1);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx13;
        g_.Write(l, r);
        if(!g_.held || g_.Filling())
            return;
        float wl, wr;
        g_.Read(wl, wr, speed_.Next());
        const float lv = p_[LEVEL] ? Level(LEVEL) : 1.f;
        l = wl * lv, r = wr * lv;
    }
};

// 48 DJFX Delay: a synced delay; LOOP SW repeats the last LENGTH instead.
class DjfxDelay : public Effect
{
    Echo e_;
    Grab g_;

  public:
    void Init(const Context &c) override
    {
        Effect::Init(c);
        e_.Init(c.sr);
    }
    void Changed(int i) override
    {
        using namespace fxp::fx48;
        const float t = kSyncBeats[int(N(TIME) * 15.f + 0.5f)] * ctx_->Beat() * Sr();
        e_.time.Set(t);
        if(i < 0)
            e_.time.Reset(t);
        e_.fb = p_[FEEDBACK] ? N(FEEDBACK) * 0.99f : 0.4f;
        e_.SetDamp(Sr(), DampHz(p_[H_DAMP_F] ? p_[H_DAMP_F] : 17), LowDampHz(p_[L_DAMP_F]));
        const float len = (0.230f - N(LENGTH) * 0.218f) * Sr();
        if(p_[LOOP_SW] && !g_.held)
            g_.Start(len);
        else if(!p_[LOOP_SW])
            g_.held = false;
        g_.len = len;
    }
    void Engage() override
    {
        g_.Reset();
        Changed(-1);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx48;
        g_.Write(l, r);
        if(g_.held && !g_.Filling())
            g_.Read(l, r, 1.f);
        float wl, wr;
        e_.Process(l, r, wl, wr);
        const float lv = p_[LEVEL] ? Level(LEVEL) : 0.7f;
        l += wl * lv, r += wr * lv;
    }
};

// 46 Back Spin: BACK SW spins the record back over LENGTH (1/1-1/16) at
// SPEED, then lets go.
class BackSpin : public Effect
{
    Grab   g_;
    double t_ = 0.0, dur_ = 0.0;
    bool   armed_ = false;

  public:
    void Changed(int) override
    {
        using namespace fxp::fx46;
        static const float kLen[5] = {4.f, 2.f, 1.f, 0.5f, 0.25f};
        dur_ = kLen[int(N(LENGTH) * 4.f + 0.5f)] * ctx_->Beat() * Sr();
        if(p_[BACK_SW] && !armed_)
        {
            armed_ = true;
            g_.Start(float(dur_) * 2.f);
            g_.fill = 0.f; // spins back whatever there is
            g_.pos  = dur_ * 2.0 - 1.0;
            t_      = 0.0;
        }
        if(!p_[BACK_SW])
            armed_ = false, g_.held = false;
    }
    void Engage() override
    {
        armed_ = false;
        g_.Reset();
        Changed(-1);
    }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx46;
        // The spin plays back what was heard before the switch; nothing is
        // recorded during it.
        if(!g_.held)
            g_.Write(l, r);
        if(!g_.held)
            return;
        // The spin: fast backwards, slowing towards the end.
        const float prog  = float(t_ / dur_);
        t_ += 1.0;
        if(prog >= 1.f)
        {
            l = r = 0.f;
            return;
        }
        const float speed = -(1.f + N(SPEED) * 3.f) * (1.f - prog);
        g_.Read(l, r, speed);
        const float g = 1.f - prog * prog;
        l *= g, r *= g;
    }
};

// 30 Scatter: cut the audio into 16ths and, on some steps, replay a recent
// one instead (1-4 steps back, sometimes reversed). TYPE (1-10) picks the
// pattern, DEPTH (1-10) how many steps it touches.
class Scatter : public Effect
{
    Delay  d_[2]{Delay(2.f), Delay(2.f)};
    double pos_ = 0.0;

  public:
    void Engage() override { pos_ = 0.0; }
    void Process(float &l, float &r) override
    {
        using namespace fxp::fx30;
        d_[0].Write(l), d_[1].Write(r);
        if(!p_[SCATTER])
            return;
        const double step = 0.25 * ctx_->Beat() * Sr(); // a 16th
        pos_ += 1.0;
        if(pos_ >= step * 16.0)
            pos_ -= step * 16.0;
        const int    s    = int(pos_ / step);
        const double in   = pos_ - s * step;
        const int    type = p_[TYPE] % 10, depth = p_[DEPTH] % 10;
        // A fixed pattern per type: which steps move, how far back, and
        // whether they play backwards.
        const bool moved = ((s * 7 + type * 3) % 10) < depth + 1 && s != 0;
        if(!moved)
            return;
        const int  back_steps = 1 + ((s * (type + 1) + type) & 3);
        const bool rev        = (type & 1) && ((s + type) & 1);
        // Slice (s - back_steps) at the matching offset: back_steps steps
        // plus the offset difference (reversed: read the slice end first).
        const double inpos = rev ? step - in : in;
        const double back  = back_steps * step + in - inpos;
        float        wl    = d_[0].Read(float(back) + 1.f);
        float        wr    = d_[1].Read(float(back) + 1.f);
        const float  edge  = float(std::fmin(in, step - in) / (0.003 * Sr()));
        const float  g     = Clamp(edge, 0.f, 1.f);
        const float  bal   = p_[BALANCE] ? N(BALANCE) : 1.f;
        l = Mix(l, wl * g + l * (1.f - g), bal), r = Mix(r, wr * g + r * (1.f - g), bal);
    }
};

std::unique_ptr<Effect> MakeDelay(int id)
{
    switch(id)
    {
        case 2: return std::make_unique<TimeCtrlDly>();
        case 9: return std::make_unique<TapeEcho>();
        case 13: return std::make_unique<DjfxLooper>();
        case 18: return std::make_unique<SyncDelay>();
        case 30: return std::make_unique<Scatter>();
        case 33: return std::make_unique<ZanZou>();
        case 36: return std::make_unique<KoDaMa>();
        case 41: return std::make_unique<SxDelay>();
        case 46: return std::make_unique<BackSpin>();
        case 47: return std::make_unique<CloudDelay>();
        case 48: return std::make_unique<DjfxDelay>();
        default: return nullptr;
    }
}

} // namespace sp404fx
