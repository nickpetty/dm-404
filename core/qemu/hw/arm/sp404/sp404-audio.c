/*
 * SP-404MKII audio path behind SAI1, as tallfree measured it on hardware:
 *
 * - Each TX slot carries a 20-bit two's-complement sample in its low bits
 *   (the field wraps at 2^20).
 * - Only TX line 3 reaches the main outputs: left sums words 0, 2, 4, 6, 12
 *   and 14, right sums 1, 3, 5, 7, 12 and 15, each at unity. Word 13 is a
 *   bitmask of buses in use, not audio.
 * - The receiver gets back, on RX line 0 words 0 and 1, the sum of TX line
 *   3 words 0-7 (even to the left, odd to the right): the resampling path.
 *   Real inputs would add in there too; there are none yet. RX slots are
 *   read as 16-bit samples (RCR5 puts the first bit at bit 15, and the
 *   firmware's DMA reads RDR 16 bits at a time).
 *
 * The stereo result goes to the frontend (out callback) and, when
 * SP404_WAV names a file, to a 48 kHz 16-bit WAV.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
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
    for (int i = 0; i < 8; i += 2) {
        loop_l += sext20(w[i]);
        loop_r += sext20(w[i + 1]);
    }
    l = loop_l + sext20(w[12]) + sext20(w[14]);
    r = loop_r + sext20(w[12]) + sext20(w[15]);
    if (rx_words >= 2) {
        rx[0][0] = (uint16_t)clip16(loop_l);
        rx[0][1] = (uint16_t)clip16(loop_r);
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

void sp404_audio_init(SP404Audio *a, IMXRTSAI *sai)
{
    const char *wav = getenv("SP404_WAV");

    memset(a, 0, sizeof(*a));
    if (wav) {
        a->wav = fopen(wav, "wb");
        if (a->wav) {
            wav_header(a);
        }
    }
    sai->frame_hook = sp404_audio_frame;
    sai->frame_opaque = a;
}
