// The BMC's mix: parameters from DT1 writes, five effect slots, routing.
//
// Addresses (firmware 5.52; README.md has the derivation):
//   02 00 00 aa   system parameters from ID 33 (Volume) on: aa = 2 * (ID - 33)
//   02 02 00 00   Tempo_Master, BPM x 10 (four nibbles)
//   02 02 00 08+2n  Effect_FxType<n>: the effect in slot n (0 = input FX,
//                 1-4 = BUS 1-4), by effect ID (fxparams.h)
//   03 hi lo aa   effect parameters: hi:lo = (effect - 1) * 5 + slot as two
//                 7-bit halves, aa = 2 * parameter index
#include <cstring>

#include "fx.h"
#include "sp404fx.h"

namespace sp404fx {

namespace {

// System parameter indices in 02 00 (address / 2).
enum
{
    kInputVolume  = 1,
    kInputFxOnOff = 9,
    kBusMute      = 10, // Bus1Mute; BusN at 10 + 5 (N - 1)
    kBusOnOff     = 11,
    kAudioMute    = 31,
};
// In 02 02 (address / 2): Effect_InputAssign, the bus the input joins
// (0 DRY, 1 BUS 1, 2 BUS 2, 3/4 BUS 3/4, i.e. the master effects; the
// firmware sets 1).
constexpr int kInputAssign = 3;

// What TX line 3 words 0-7 carry, as stereo pairs. Pads routed to BUS 1
// play on words 2/3 (seen); the others follow the same pattern.
enum Stem
{
    kDry,
    kBus1,
    kBus2,
};
constexpr Stem kStemRoute[4] = {kDry, kBus1, kBus2, kDry};

constexpr int kSlots = 5; // 0 input FX, 1-4 BUS 1-4

class Slot
{
  public:
    void Init(const Context *ctx)
    {
        ctx_ = ctx;
        gain_.Init(ctx->sr, 6.f);
    }

    // The type the firmware selected; the switch happens once faded out.
    void Select(int id, const uint8_t (*params)[5][32], int slot)
    {
        pending_  = id;
        params_   = params;
        slot_idx_ = slot;
    }
    void SetOn(bool on)
    {
        if(on && !on_ && fx_)
            fx_->Engage();
        on_ = on;
    }
    void Param(int id, int idx, int v)
    {
        if(fx_ && id == type_)
            fx_->Set(idx, v);
    }

    void Process(float &l, float &r)
    {
        if(pending_ != type_)
        {
            gain_.Set(0.f);
            if(gain_.Value() < 1e-4f || !fx_)
                Swap();
        }
        else
            gain_.Set(on_ && fx_ ? 1.f : 0.f);
        const float g = gain_.Next();
        if(g < 1e-5f || !fx_)
            return;
        float wl = l, wr = r;
        fx_->Process(wl, wr);
        l += (wl - l) * g;
        r += (wr - r) * g;
    }

    int  Type() const { return type_; }
    bool On() const { return on_; }

  private:
    void Swap()
    {
        type_ = pending_;
        fx_   = MakeEffect(type_);
        if(fx_)
        {
            fx_->Init(*ctx_);
            const int n = fxp::kParamCount[type_];
            for(int i = 0; i < n && i < Effect::kMaxParams; i++)
                fx_->Set(i, params_[type_][slot_idx_][i]);
            fx_->Changed(-1);
        }
        gain_.Reset(0.f);
    }

    const Context          *ctx_ = nullptr;
    std::unique_ptr<Effect> fx_;
    int                     type_ = 0, pending_ = 0, slot_idx_ = 0;
    bool                    on_   = false;
    const uint8_t (*params_)[5][32] = nullptr;
    Smooth                  gain_;
};

} // namespace

class Engine
{
  public:
    explicit Engine(int sr)
    {
        ctx_.sr = float(sr);
        for(int i = 0; i < kSlots; i++)
            slot_[i].Init(&ctx_);
        std::memset(sys_, 0, sizeof(sys_));
        for(int id = 0; id < 49; id++)
            for(int s = 0; s < 5; s++)
                std::memcpy(params_[id][s], fxp::kNeutral[id], 32);
        mute_.Init(ctx_.sr, 5.f);
        mute_.Reset(1.f);
        inputGain_.Init(ctx_.sr, 10.f);
        inputGain_.Reset(0.f);
    }

    void Dt1(const uint8_t *a, const uint8_t *d, int len)
    {
        int v = 0;
        for(int i = 0; i < len; i++)
            v = v * 16 + (d[i] & 0xf);
        if(a[0] == 2 && a[2] == 0)
        {
            if(a[1] == 2 && a[3] == 0) // Tempo_Master, x10
            {
                if(v >= 200 && v <= 3000)
                    ctx_.bpm = v / 10.f;
                return;
            }
            if(a[1] == 2 && a[3] >= 8 && a[3] <= 16 && !(a[3] & 1))
            {
                const int s = (a[3] - 8) / 2;
                slot_[s].Select(v >= 0 && v < 49 ? v : 0, params_, s);
                return;
            }
            if(a[1] < 3 && a[3] < 128)
                sys_[a[1]][a[3] / 2] = uint8_t(v);
            if(a[1] == 0)
                System();
            return;
        }
        if(a[0] == 3 && a[3] < 64)
        {
            const int block = a[1] * 128 + a[2];
            const int id = block / 5 + 1, s = block % 5, idx = a[3] / 2;
            if(id < 1 || id > 48)
                return;
            params_[id][s][idx] = uint8_t(v);
            slot_[s].Param(id, idx, v);
        }
    }

    void Process(const float *st, const float *in, float *out, float *buses, int frames)
    {
        for(int f = 0; f < frames; f++, st += SP404FX_STEMS, in += 2, out += 2)
        {
            float bl[3] = {}, br[3] = {};
            for(int k = 0; k < 4; k++)
            {
                bl[kStemRoute[k]] += st[2 * k];
                br[kStemRoute[k]] += st[2 * k + 1];
            }
            // The input: through the input FX, heard at InputVolume
            // (EXT SOURCE on: 255, off: 0), on the bus it is assigned to.
            float il = in[0], ir = in[1];
            slot_[0].Process(il, ir);
            const float ig = inputGain_.Next();
            il *= ig;
            ir *= ig;
            const int ib = sys_[2][kInputAssign] == 1 || sys_[2][kInputAssign] == 2 ? sys_[2][kInputAssign] : kDry;
            bl[ib] += il;
            br[ib] += ir;
            float l = bl[kDry], r = br[kDry];
            float bus[3][2] = {{l, r}};
            for(int b = 1; b <= 2; b++)
            {
                float xl = bl[b], xr = br[b];
                slot_[b].Process(xl, xr);
                const float m = muted_[b - 1] ? 0.f : 1.f;
                bus[b][0] = xl * m;
                bus[b][1] = xr * m;
                l += xl * m;
                r += xr * m;
            }
            // BUS 3 and BUS 4 in series on the whole mix.
            slot_[3].Process(l, r);
            slot_[4].Process(l, r);
            const float g = mute_.Next();
            out[0] = l * g;
            out[1] = r * g;
            if(buses)
            {
                // DRY, BUS 1 and BUS 2 as they enter the master (BUS 3/4)
                // effects: after their own effects and mutes.
                for(int b = 0; b < 3; b++)
                {
                    buses[2 * b]     = bus[b][0] * g;
                    buses[2 * b + 1] = bus[b][1] * g;
                }
                // The input as it joined its bus: the resampling loopback
                // takes it out, since the inputs reach the RX side as they
                // are (so what a bus effect made of it stays in).
                buses[6] = il * g;
                buses[7] = ir * g;
                buses += SP404FX_BUSES;
            }
        }
    }

    int SlotInfo(int s, int *on) const
    {
        if(s < 0 || s >= kSlots)
            return -1;
        if(on)
            *on = slot_[s].On();
        return slot_[s].Type();
    }

  private:
    void System()
    {
        slot_[0].SetOn(sys_[0][kInputFxOnOff]);
        for(int b = 1; b <= 4; b++)
        {
            slot_[b].SetOn(sys_[0][kBusOnOff + 5 * (b - 1)]);
            if(b <= 2)
                muted_[b - 1] = sys_[0][kBusMute + 5 * (b - 1)];
        }
        mute_.Set(sys_[0][kAudioMute] ? 0.f : 1.f);
        inputGain_.Set(sys_[0][kInputVolume] / 255.f);
    }

    Context ctx_;
    Slot    slot_[kSlots];
    uint8_t sys_[3][64];
    uint8_t params_[49][5][32];
    bool    muted_[2] = {};
    Smooth  mute_, inputGain_;
};

} // namespace sp404fx

struct SP404FX
{
    sp404fx::Engine engine;
    explicit SP404FX(int sr) : engine(sr) {}
};

extern "C" {

int sp404fx_version(void)
{
    return SP404FX_VERSION;
}

SP404FX *sp404fx_new(int sample_rate)
{
    return new SP404FX(sample_rate > 0 ? sample_rate : 48000);
}

void sp404fx_free(SP404FX *fx)
{
    delete fx;
}

void sp404fx_dt1(SP404FX *fx, const uint8_t *addr, const uint8_t *data, int len)
{
    fx->engine.Dt1(addr, data, len);
}

void sp404fx_process(SP404FX *fx, const float *stems, const float *input, float *out, int frames)
{
    fx->engine.Process(stems, input, out, nullptr, frames);
}

void sp404fx_process_buses(SP404FX *fx, const float *stems, const float *input, float *out, float *buses,
                           int frames)
{
    fx->engine.Process(stems, input, out, buses, frames);
}

int sp404fx_slot(SP404FX *fx, int slot, int *on)
{
    return fx->engine.SlotInfo(slot, on);
}

} // extern "C"
