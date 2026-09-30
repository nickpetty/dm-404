/*
 * SP-404MKII audio path behind SAI1, as tallfree measured it on hardware:
 *
 * - Each TX slot carries a 20-bit two's-complement sample in its low bits
 *   (the field wraps at 2^20).
 * - Only TX line 3 reaches the main outputs: left sums words 0, 2, 4, 6, 12
 *   and 14, right sums 1, 3, 5, 7, 12 and 15, each at unity. Word 13 is a
 *   bitmask of buses in use, not audio.
 * - The receiver gets back, on RX line 0 words 0 and 1, the sum of TX line
 *   3 words 0-7 (even to the left, odd to the right), at unity: the
 *   resampling path, which skip back sampling records too.
 * - Words 0-7 are the buses (BUS 1 on 2/3): the BMC mixes them through its
 *   effects. With the effects engine loaded (sp404-fx.c) both the output
 *   and the loopback carry its mix; the metronome words stay outside it.
 *   The unit's inputs (from the frontend) are mixed in there. RX slots are
 *   read as 16-bit samples (RCR5 puts the first bit at bit 15, and the
 *   firmware's DMA reads RDR 16 bits at a time).
 *
 * The stereo result goes to the frontend (out callback) and, when
 * SP404_WAV names a file, to a 48 kHz 16-bit WAV.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "hw/arm/sp404/sp404.h"

#define MAIN_LINE 3

static int32_t sext20(uint32_t w)
{
    return sextract32(w, 0, 20);
}

static void wav_header(SP404Audio *a)
{
    uint32_t data = a->wav_frames * 4;
    uint8_t h[44];

    memcpy(h, "RIFF", 4);
    stl_le_p(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    stl_le_p(h + 16, 16);
    stw_le_p(h + 20, 1);                /* PCM */
    stw_le_p(h + 22, 2);
    stl_le_p(h + 24, 48000);
    stl_le_p(h + 28, 48000 * 4);
    stw_le_p(h + 32, 4);
    stw_le_p(h + 34, 16);
    memcpy(h + 36, "data", 4);
    stl_le_p(h + 40, data);
    fseek(a->wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), a->wav);
    fseek(a->wav, 0, SEEK_END);
}

static int16_t clip16(int32_t v20)
{
    int32_t v = v20 >> 4;               /* 20-bit full scale to 16-bit */

    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
}

static int16_t sat16(int32_t v)
{
    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
}

static void sp404_audio_frame(void *opaque,
                              uint32_t tx[4][IMXRT_SAI_MAX_WORDS],
                              unsigned tx_words,
                              uint32_t rx[4][IMXRT_SAI_MAX_WORDS],
                              unsigned rx_words)
{
    SP404Audio *a = opaque;
    uint32_t *w = tx[MAIN_LINE];
    int32_t l, r, loop_l = 0, loop_r = 0;
    int16_t lr[2];

    if (tx_words < 16) {
        return;
    }
    /* SP404_TRACE=audio: once a second, the peak of every slot of every line. */
    for (int ln = 0; ln < 4; ln++) {
        for (int i = 0; i < 16; i++) {
            int32_t v = abs(sext20(tx[ln][i]));
            if (v > a->slot_peak[ln][i]) {
                a->slot_peak[ln][i] = v;
            }
        }
    }
    if (++a->peak_frames == 48000) {
        for (int ln = 0; ln < 4; ln++) {
            char line[256];
            int n = 0;
            bool any = false;

            for (int i = 0; i < 16; i++) {
                n += snprintf(line + n, sizeof(line) - n, " %6d",
                              a->slot_peak[ln][i]);
                any |= a->slot_peak[ln][i] != 0;
            }
            if (any) {
                SP404_TRACE("audio", "line %d peaks:%s", ln, line);
            }
        }
        SP404_TRACE("audio", "rx words 0/1 peaks: %d %d (%u words)",
                    a->rx_peak[0], a->rx_peak[1], rx_words);
        a->rx_peak[0] = a->rx_peak[1] = 0;
        memset(a->slot_peak, 0, sizeof(a->slot_peak));
        a->peak_frames = 0;
    }
    if (a->fx && sp404_fx_active(a->fx)) {
        /* The BMC mixes the buses through its effects. */
        float stems[8], in[2] = { 0, 0 }, out[2];

        for (int i = 0; i < 8; i++) {
            stems[i] = sext20(w[i]) / 32768.0f;
        }
        if (a->in_count) {
            in[0] = a->in_ring[a->in_head * 2] / 32768.0f;
            in[1] = a->in_ring[a->in_head * 2 + 1] / 32768.0f;
        }
        if (a->usb_count) {
            in[0] += a->usb_ring[a->usb_head * 2] / 32768.0f;
            in[1] += a->usb_ring[a->usb_head * 2 + 1] / 32768.0f;
        }
        sp404_fx_process(a->fx, stems, in, out);
        loop_l = lrintf(fmaxf(fminf(out[0], 15.f), -15.f) * 32768.0f);
        loop_r = lrintf(fmaxf(fminf(out[1], 15.f), -15.f) * 32768.0f);
    } else {
        for (int i = 0; i < 8; i += 2) {
            loop_l += sext20(w[i]);
            loop_r += sext20(w[i + 1]);
        }
    }
    l = loop_l + sext20(w[12]) + sext20(w[14]);
    r = loop_r + sext20(w[12]) + sext20(w[15]);
    if (rx_words >= 2) {
        /*
         * At unity: the firmware writes 16-bit samples into the 20-bit
         * slots, and what comes back must match them, or resampling
         * records 24 dB down and skip back never sees its trigger level
         * (0x40c at 0x80bcf238, which any pad reaches on the unit).
         */
        rx[0][0] = (uint16_t)sat16(loop_l);
        rx[0][1] = (uint16_t)sat16(loop_r);
    }
    if (a->usb_count && a->in_slot + 1 < (int)rx_words) {
        /* USB audio (the DAW plugin) joins the inputs. */
        int32_t sl = (int16_t)rx[0][a->in_slot] + a->usb_ring[a->usb_head * 2];
        int32_t sr = (int16_t)rx[0][a->in_slot + 1] + a->usb_ring[a->usb_head * 2 + 1];

        rx[0][a->in_slot] = (uint16_t)sat16(sl);
        rx[0][a->in_slot + 1] = (uint16_t)sat16(sr);
        a->usb_head = (a->usb_head + 1) % SP404_USB_RING;
        a->usb_count--;
    }
    if (a->in_count && a->in_slot + 1 < (int)rx_words) {
        int16_t il = a->in_ring[a->in_head * 2], ir = a->in_ring[a->in_head * 2 + 1];
        int32_t sl = (int16_t)rx[0][a->in_slot] + il;
        int32_t sr = (int16_t)rx[0][a->in_slot + 1] + ir;

        rx[0][a->in_slot] = (uint16_t)(sl > INT16_MAX ? INT16_MAX : sl < INT16_MIN ? INT16_MIN : sl);
        rx[0][a->in_slot + 1] = (uint16_t)(sr > INT16_MAX ? INT16_MAX : sr < INT16_MIN ? INT16_MIN : sr);
        a->in_head = (a->in_head + 1) % 16384;
        a->in_count--;
    }
    for (int i = 0; i < 2 && i < (int)rx_words; i++) {
        int v = abs((int16_t)rx[0][i]);
        if (v > a->rx_peak[i]) {
            a->rx_peak[i] = v;
        }
    }
    lr[0] = clip16(l);
    lr[1] = clip16(r);
    if (a->out) {
        a->out(a->out_opaque, lr, 1);
    }
    if (a->wav) {
        int16_t le[2] = { cpu_to_le16(lr[0]), cpu_to_le16(lr[1]) };

        fwrite(le, sizeof(le), 1, a->wav);
        if (++a->wav_frames % 48000 == 0) {
            wav_header(a);              /* keep the file playable */
        }
    }
}

void sp404_audio_input(SP404Audio *a, const int16_t *lr, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (a->in_count == 16384) {
            /* Overrun: the frontend is ahead; drop the oldest. */
            a->in_head = (a->in_head + 1) % 16384;
            a->in_count--;
        }
        unsigned at = (a->in_head + a->in_count) % 16384;
        a->in_ring[at * 2] = lr[i * 2];
        a->in_ring[at * 2 + 1] = lr[i * 2 + 1];
        a->in_count++;
    }
}

void sp404_audio_usb_input(SP404Audio *a, const int16_t *lr, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (a->usb_count == SP404_USB_RING) {
            a->usb_head = (a->usb_head + 1) % SP404_USB_RING;
            a->usb_count--;
        }
        unsigned at = (a->usb_head + a->usb_count) % SP404_USB_RING;
        a->usb_ring[at * 2] = lr[i * 2];
        a->usb_ring[at * 2 + 1] = lr[i * 2 + 1];
        a->usb_count++;
    }
    /*
     * The sender's clock is the DAW's, not ours: should it run ahead, the
     * backlog (latency) would grow without end. Past 60 ms, skip back to
     * 20 ms (the plugin steers its rate, so this is rare).
     */
    if (a->usb_count > 48 * 60) {
        unsigned drop = a->usb_count - 48 * 20;

        a->usb_head = (a->usb_head + drop) % SP404_USB_RING;
        a->usb_count -= drop;
    }
}

void sp404_audio_init(SP404Audio *a, IMXRTSAI *sai)
{
    const char *wav = getenv("SP404_WAV");
    const char *slot = getenv("SP404_IN_SLOT");

    memset(a, 0, sizeof(*a));
    /*
     * The inputs arrive on RX line 0 words 0 and 1, where sampling and the
     * REC level meter read them (with the resampling loopback mixed in).
     */
    a->in_slot = slot ? atoi(slot) : 0;
    if (wav) {
        a->wav = fopen(wav, "wb");
        if (a->wav) {
            wav_header(a);
        }
    }
    sai->frame_hook = sp404_audio_frame;
    sai->frame_opaque = a;
}
