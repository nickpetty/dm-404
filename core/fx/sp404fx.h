/*
 * sp404fx: the SP-404MKII's effects and final mix, standing in for Roland's
 * BMC sound chip, whose own program is encrypted.
 *
 * The i.MX firmware sends the BMC dry buses on SAI1 TX line 3 and drives it
 * with DT1 parameter writes over the BMC UART. This library takes both and
 * produces what the BMC would: the processed stereo mix, which is also what
 * comes back on the resampling loopback. QEMU loads it at run time
 * (core/qemu/hw/arm/sp404/sp404-fx.c); without it the mix stays dry.
 *
 * Built by tools/build_fx.sh with DaisySP. See core/fx/README.md for the
 * parameter map and routing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP404FX_H
#define SP404FX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP404FX_VERSION 1

/* The number of TX line 3 words the engine takes per frame (the buses). */
#define SP404FX_STEMS 8

typedef struct SP404FX SP404FX;

#if defined(_WIN32) && defined(SP404FX_BUILD)
#define SP404FX_API __declspec(dllexport)
#else
#define SP404FX_API
#endif

SP404FX_API int sp404fx_version(void);
SP404FX_API SP404FX *sp404fx_new(int sample_rate);
SP404FX_API void sp404fx_free(SP404FX *fx);

/*
 * A DT1 write from the firmware: "F0 41 10 00 00 00 00 08 12 a a a a
 * d.. sum F7". addr is the four address bytes; data the data bytes, each a
 * nibble, most significant first.
 */
SP404FX_API void sp404fx_dt1(SP404FX *fx, const uint8_t *addr,
                             const uint8_t *data, int len);

/*
 * Process frames: stems holds SP404FX_STEMS samples per frame (TX line 3
 * words 0-7, as 16-bit full scale = 1.0), input two (the unit's inputs),
 * and out receives two (the mix).
 */
SP404FX_API void sp404fx_process(SP404FX *fx, const float *stems,
                                 const float *input, float *out, int frames);

/* What slot 0-4 is running, for traces: effect ID (0 = bypass) and on/off. */
SP404FX_API int sp404fx_slot(SP404FX *fx, int slot, int *on);

#ifdef __cplusplus
}
#endif

#endif
