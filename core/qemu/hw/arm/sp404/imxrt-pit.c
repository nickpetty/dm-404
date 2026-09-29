/*
 * i.MX RT PIT: four 32-bit down-counters on PERCLK.
 *
 * Each channel reloads from LDVAL and on expiry sets TIF (interrupting if
 * TIE) and pulses its trigger output, GPIO output "trigger" n, which the
 * XBAR routes on (the SP-404 paces its ADC scan from channel 0). Chained
 * mode is not modelled. sysbus IRQ 0 is the shared PIT interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/sp404/sp404.h"

#define R_MCR       0x000
#define   MCR_MDIS  (1u << 1)
#define R_LTMR64H   0x0e0
#define R_LTMR64L   0x0e4
#define R_CH(n)     (0x100 + (n) * 0x10)
#define   CH_LDVAL  0x0
#define   CH_CVAL   0x4
#define   CH_TCTRL  0x8
#define     TCTRL_TEN (1u << 0)
#define     TCTRL_TIE (1u << 1)
#define   CH_TFLG   0xc

static int64_t pit_period_ns(IMXRTPIT *s, int n)
{
    return muldiv64((uint64_t)s->ch[n].ldval + 1, NANOSECONDS_PER_SECOND,
                    s->freq);
}

static void pit_update_irq(IMXRTPIT *s)
{
    bool irq = false;

    for (int n = 0; n < 4; n++) {
        irq |= (s->ch[n].tctrl & TCTRL_TIE) && s->ch[n].tif;
    }
    qemu_set_irq(s->irq, irq);
}

static bool pit_running(IMXRTPIT *s, int n)
{
    return !(s->mcr & MCR_MDIS) && (s->ch[n].tctrl & TCTRL_TEN);
}

static void pit_arm(IMXRTPIT *s, int n)
{
    if (pit_running(s, n)) {
        s->ch[n].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->ch[n].timer, s->ch[n].start + pit_period_ns(s, n));
    } else {
        timer_del(s->ch[n].timer);
    }
}

static void pit_expire(void *opaque)
{
    IMXRTPITChannel *c = opaque;
    IMXRTPIT *s = c->pit;
    int n = c - s->ch;

    c->tif = true;
    c->start += pit_period_ns(s, n);
    timer_mod(c->timer, c->start + pit_period_ns(s, n));
    pit_update_irq(s);
    qemu_irq_pulse(s->trigger[n]);
}

static uint64_t pit_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTPIT *s = opaque;

    if (offset == R_MCR) {
        return s->mcr;
    }
    if (offset >= R_CH(0) && offset < R_CH(4)) {
        int n = (offset - R_CH(0)) / 0x10;
        IMXRTPITChannel *c = &s->ch[n];

        switch (offset & 0xf) {
        case CH_LDVAL: return c->ldval;
        case CH_CVAL:
            if (pit_running(s, n)) {
                int64_t left = timer_expire_time_ns(c->timer) -
                               qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                return left <= 0 ? 0 :
                       muldiv64(left, s->freq, NANOSECONDS_PER_SECOND);
            }
            return c->ldval;
        case CH_TCTRL: return c->tctrl;
        case CH_TFLG: return c->tif;
        }
    }
    return 0;           /* LTMR64 and the rest: lifetime timer not modelled */
}

static void pit_write(void *opaque, hwaddr offset, uint64_t val,
                      unsigned size)
{
    IMXRTPIT *s = opaque;

    if (offset == R_MCR) {
        s->mcr = val;
        for (int n = 0; n < 4; n++) {
            pit_arm(s, n);
        }
        return;
    }
    if (offset >= R_CH(0) && offset < R_CH(4)) {
        int n = (offset - R_CH(0)) / 0x10;
        IMXRTPITChannel *c = &s->ch[n];

        switch (offset & 0xf) {
        case CH_LDVAL:
            c->ldval = val;     /* takes effect at the next reload */
            break;
        case CH_TCTRL: {
            bool was = pit_running(s, n);

            c->tctrl = val & 7;
            if (pit_running(s, n) != was) {
                pit_arm(s, n);
            }
            break;
        }
        case CH_TFLG:
            if (val & 1) {
                c->tif = false;
            }
            break;
        }
        SP404_TRACE("pit", "ch%d ldval %u tctrl 0x%x", n, c->ldval, c->tctrl);
        pit_update_irq(s);
    }
}

static const MemoryRegionOps pit_ops = {
    .read = pit_read,
    .write = pit_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pit_reset(DeviceState *dev)
{
    IMXRTPIT *s = IMXRT_PIT(dev);

    s->mcr = MCR_MDIS;
    for (int n = 0; n < 4; n++) {
        timer_del(s->ch[n].timer);
        s->ch[n].ldval = s->ch[n].tctrl = 0;
        s->ch[n].tif = false;
    }
    pit_update_irq(s);
}

static void pit_realize(DeviceState *dev, Error **errp)
{
    IMXRTPIT *s = IMXRT_PIT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &pit_ops, s, "imxrt.pit",
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_out_named(dev, s->trigger, "trigger", 4);
    for (int n = 0; n < 4; n++) {
        s->ch[n].pit = s;
        s->ch[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pit_expire,
                                      &s->ch[n]);
    }
}

static const Property pit_properties[] = {
    DEFINE_PROP_UINT32("freq", IMXRTPIT, freq, 24000000),
};

static void pit_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pit_realize;
    device_class_set_legacy_reset(dc, pit_reset);
    device_class_set_props(dc, pit_properties);
}

static const TypeInfo pit_info = {
    .name = TYPE_IMXRT_PIT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTPIT),
    .class_init = pit_class_init,
};

static void pit_register_types(void)
{
    type_register_static(&pit_info);
}

type_init(pit_register_types)
