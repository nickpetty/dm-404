/*
 * i.MX RT ADC1/ADC2 with the ADC_ETC trigger controller.
 *
 * MMIO region 0 is ADC1, 1 is ADC2, 2 is ADC_ETC. Conversions are instant
 * and take their value from the machine's sample callback (adc, channel).
 *
 * ADC: software-triggered conversions (a write to HCn while CFG.ADTRG is
 * clear) land in Rn and set COCOn; calibration completes at once.
 *
 * ADC_ETC: GPIO inputs "trig" 0-7 are the XBAR triggers; on a rising edge
 * of an enabled trigger, its chain runs (triggers 0-3 on ADC1, 4-7 on
 * ADC2): each segment converts channel CSEL into the RESULT register and
 * the HWTS-selected HC/R pair, and its IE bits pick which DONE interrupt
 * reports the end. SW_TRIG does the same from software.
 *
 * sysbus IRQs: 0 ADC1, 1 ADC2, 2-4 ADC_ETC DONE0-2, 5 ADC_ETC error.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/arm/sp404/sp404.h"

/* ADC */
#define A_HC(n)     ((n) * 4)
#define A_HS        0x20
#define A_R(n)      (0x24 + (n) * 4)
#define A_CFG       0x44
#define   CFG_ADTRG (1u << 13)
#define A_GC        0x48
#define   GC_CAL    (1u << 7)
#define A_GS        0x4c
#define A_CV        0x50
#define A_OFS       0x54
#define A_CAL       0x58
#define HC_AIEN     (1u << 7)
#define HC_ADCH     0x1f

/* ADC_ETC */
#define E_CTRL          0x00
#define   ECTRL_SOFTRST (1u << 31)
#define E_DONE0_1       0x04
#define E_DONE2_ERR     0x08
#define E_DMA_CTRL      0x0c
#define E_TRIG(n)       (0x10 + (n) * 0x28)
#define   T_CTRL        0x00
#define     TCTRL_SW_TRIG (1u << 0)
#define   T_COUNTER     0x04
#define   T_CHAIN       0x08    /* four words, two segments each */
#define   T_RESULT      0x18    /* four words, two results each */

static uint16_t adc_sample(IMXRTADC *s, int adc, int ch)
{
    uint16_t v = s->sample ? s->sample(s->sample_opaque, adc, ch) : 0;

    return v & 0xfff;
}

static void adc_update_irq(IMXRTADC *s)
{
    for (int a = 0; a < 2; a++) {
        bool irq = false;

        for (int n = 0; n < 8; n++) {
            irq |= (s->adc[a].hc[n] & HC_AIEN) && (s->adc[a].hs & (1u << n));
        }
        qemu_set_irq(s->irq[a], irq);
    }
    qemu_set_irq(s->irq[2], !!(s->done0_1 & 0x000000ff));
    qemu_set_irq(s->irq[3], !!(s->done0_1 & 0x00ff0000));
    qemu_set_irq(s->irq[4], !!(s->done2_err & 0x000000ff));
    qemu_set_irq(s->irq[5], !!(s->done2_err & 0x00ff0000));
}

static void adc_convert(IMXRTADC *s, int a, int n, int ch)
{
    s->adc[a].r[n] = adc_sample(s, a, ch);
    s->adc[a].hs |= 1u << n;
}

/* Run trigger k's chain. */
static void etc_run(IMXRTADC *s, int k)
{
    IMXRTADCTrig *t = &s->trig[k];
    int a = k < 4 ? 0 : 1;
    int len = ((t->ctrl >> 8) & 7) + 1;

    for (int seg = 0; seg < len; seg++) {
        uint16_t c = t->chain[seg / 2] >> ((seg & 1) * 16);
        int csel = c & 0xf, hwts = (c >> 4) & 0xff, ie = (c >> 13) & 3;
        uint16_t v = adc_sample(s, a, csel);

        t->result[seg / 2] = deposit32(t->result[seg / 2], (seg & 1) * 16,
                                       12, v);
        for (int n = 0; n < 8; n++) {
            if (hwts & (1u << n)) {
                s->adc[a].r[n] = v;
                s->adc[a].hs |= 1u << n;
            }
        }
        switch (ie) {
        case 1: s->done0_1 |= 1u << k; break;
        case 2: s->done0_1 |= 1u << (k + 16); break;
        case 3: s->done2_err |= 1u << k; break;
        }
    }
    SP404_TRACE("adc", "trig%d: %d conversions", k, len);
    adc_update_irq(s);
}

static void etc_trigger(void *opaque, int k, int level)
{
    IMXRTADC *s = opaque;
    bool rising = level && !(s->trig_level & (1u << k));

    s->trig_level = deposit32(s->trig_level, k, 1, !!level);
    if (rising && (s->etc_ctrl & (1u << k))) {
        etc_run(s, k);
    }
}

static uint64_t adc_read(IMXRTADC *s, int a, hwaddr offset)
{
    IMXRTADCUnit *u = &s->adc[a];

    switch (offset) {
    case A_HC(0) ... A_HC(7):
        return u->hc[offset / 4];
    case A_HS:
        return u->hs;
    case A_R(0) ... A_R(7): {
        int n = (offset - A_R(0)) / 4;

        u->hs &= ~(1u << n);
        adc_update_irq(s);
        return u->r[n];
    }
    case A_CFG: return u->cfg;
    case A_GC: return u->gc;
    case A_GS: return u->gs;
    case A_CV: return u->cv;
    case A_OFS: return u->ofs;
    case A_CAL: return u->cal;
    }
    return 0;
}

static void adc_write(IMXRTADC *s, int a, hwaddr offset, uint32_t v)
{
    IMXRTADCUnit *u = &s->adc[a];

    switch (offset) {
    case A_HC(0) ... A_HC(7): {
        int n = offset / 4;

        u->hc[n] = v;
        u->hs &= ~(1u << n);
        if (!(u->cfg & CFG_ADTRG) && (v & HC_ADCH) != HC_ADCH &&
            (v & HC_ADCH) != 0x10) {
            adc_convert(s, a, n, v & HC_ADCH);
        }
        break;
    }
    case A_CFG: u->cfg = v; break;
    case A_GC:
        u->gc = v & ~GC_CAL;            /* calibration finishes at once */
        if (v & GC_CAL) {
            u->hs |= 1;
        }
        break;
    case A_GS: u->gs &= ~(v & 6); break;
    case A_CV: u->cv = v; break;
    case A_OFS: u->ofs = v; break;
    case A_CAL: u->cal = v; break;
    }
    adc_update_irq(s);
}

static uint64_t etc_read(IMXRTADC *s, hwaddr offset)
{
    switch (offset) {
    case E_CTRL: return s->etc_ctrl;
    case E_DONE0_1: return s->done0_1;
    case E_DONE2_ERR: return s->done2_err;
    case E_DMA_CTRL: return s->dma_ctrl;
    }
    if (offset >= E_TRIG(0) && offset < E_TRIG(8)) {
        IMXRTADCTrig *t = &s->trig[(offset - E_TRIG(0)) / 0x28];
        hwaddr r = (offset - E_TRIG(0)) % 0x28;

        if (r == T_CTRL) {
            return t->ctrl;
        } else if (r == T_COUNTER) {
            return t->counter;
        } else if (r < T_RESULT) {
            return t->chain[(r - T_CHAIN) / 4];
        } else {
            return t->result[(r - T_RESULT) / 4];
        }
    }
    return 0;
}

static void etc_write(IMXRTADC *s, hwaddr offset, uint32_t v)
{
    switch (offset) {
    case E_CTRL:
        s->etc_ctrl = v & ~ECTRL_SOFTRST;
        if (v & ECTRL_SOFTRST) {
            s->done0_1 = s->done2_err = 0;
        }
        break;
    case E_DONE0_1: s->done0_1 &= ~v; break;
    case E_DONE2_ERR: s->done2_err &= ~v; break;
    case E_DMA_CTRL: s->dma_ctrl = v; break;
    default:
        if (offset >= E_TRIG(0) && offset < E_TRIG(8)) {
            int k = (offset - E_TRIG(0)) / 0x28;
            IMXRTADCTrig *t = &s->trig[k];
            hwaddr r = (offset - E_TRIG(0)) % 0x28;

            if (r == T_CTRL) {
                t->ctrl = v & ~TCTRL_SW_TRIG;
                if (v & TCTRL_SW_TRIG) {
                    etc_run(s, k);
                }
            } else if (r == T_COUNTER) {
                t->counter = v;
            } else if (r < T_RESULT) {
                t->chain[(r - T_CHAIN) / 4] = v;
            }
        }
    }
    adc_update_irq(s);
}

static uint64_t adc1_read(void *o, hwaddr off, unsigned sz)
{
    return adc_read(o, 0, off);
}

static void adc1_write(void *o, hwaddr off, uint64_t v, unsigned sz)
{
    adc_write(o, 0, off, v);
}

static uint64_t adc2_read(void *o, hwaddr off, unsigned sz)
{
    return adc_read(o, 1, off);
}

static void adc2_write(void *o, hwaddr off, uint64_t v, unsigned sz)
{
    adc_write(o, 1, off, v);
}

static uint64_t etc_mmio_read(void *o, hwaddr off, unsigned sz)
{
    return etc_read(o, off);
}

static void etc_mmio_write(void *o, hwaddr off, uint64_t v, unsigned sz)
{
    etc_write(o, off, v);
}

#define ADC_OPS(r, w) {                             \
    .read = r, .write = w,                          \
    .endianness = DEVICE_LITTLE_ENDIAN,             \
    .valid.min_access_size = 4,                     \
    .valid.max_access_size = 4,                     \
}

static const MemoryRegionOps adc1_ops = ADC_OPS(adc1_read, adc1_write);
static const MemoryRegionOps adc2_ops = ADC_OPS(adc2_read, adc2_write);
static const MemoryRegionOps etc_ops = ADC_OPS(etc_mmio_read, etc_mmio_write);

static void imxrt_adc_reset(DeviceState *dev)
{
    IMXRTADC *s = IMXRT_ADC(dev);

    memset(s->adc, 0, sizeof(s->adc));
    for (int a = 0; a < 2; a++) {
        for (int n = 0; n < 8; n++) {
            s->adc[a].hc[n] = HC_ADCH;
        }
        s->adc[a].cfg = 0x00000200;
    }
    memset(s->trig, 0, sizeof(s->trig));
    s->etc_ctrl = 0xc0000000;
    s->done0_1 = s->done2_err = s->dma_ctrl = 0;
    s->trig_level = 0;
    adc_update_irq(s);
}

static void imxrt_adc_realize(DeviceState *dev, Error **errp)
{
    IMXRTADC *s = IMXRT_ADC(dev);

    memory_region_init_io(&s->mmio[0], OBJECT(s), &adc1_ops, s, "imxrt.adc1",
                          0x4000);
    memory_region_init_io(&s->mmio[1], OBJECT(s), &adc2_ops, s, "imxrt.adc2",
                          0x4000);
    memory_region_init_io(&s->mmio[2], OBJECT(s), &etc_ops, s,
                          "imxrt.adc_etc", 0x4000);
    for (int i = 0; i < 3; i++) {
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio[i]);
    }
    for (int i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    qdev_init_gpio_in_named(dev, etc_trigger, "trig", 8);
}

static void imxrt_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt_adc_realize;
    device_class_set_legacy_reset(dc, imxrt_adc_reset);
}

static const TypeInfo imxrt_adc_info = {
    .name = TYPE_IMXRT_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTADC),
    .class_init = imxrt_adc_class_init,
};

static void imxrt_adc_register_types(void)
{
    type_register_static(&imxrt_adc_info);
}

type_init(imxrt_adc_register_types)
