/*
 * i.MX RT XBARA: a signal crossbar. Output n follows whichever input its
 * SEL field names (SELm holds outputs 2m in bits 6:0 and 2m+1 in bits
 * 14:8). Inputs and outputs are GPIO lines "in" and "out", numbered as in
 * the reference manual's XBARA1 input and output tables. The edge-detect
 * control registers are kept but not acted on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/arm/sp404/sp404.h"

static unsigned xbar_sel(IMXRTXBAR *s, unsigned out)
{
    return s->sel[out];
}

static void xbar_input(void *opaque, int n, int level)
{
    IMXRTXBAR *s = opaque;

    s->in_level[n] = level;
    for (unsigned out = 0; out < IMXRT_XBAR_OUTPUTS; out++) {
        if (xbar_sel(s, out) == n) {
            qemu_set_irq(s->out[out], level);
        }
    }
}

static uint64_t xbar_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTXBAR *s = opaque;
    unsigned m = offset / 2;

    if (m * 2 + 1 < IMXRT_XBAR_OUTPUTS) {
        return s->sel[m * 2] | (s->sel[m * 2 + 1] << 8);
    }
    return s->ctrl[(offset / 2) & 1];
}

static void xbar_write(void *opaque, hwaddr offset, uint64_t val,
                       unsigned size)
{
    IMXRTXBAR *s = opaque;
    unsigned m = offset / 2;

    if (m * 2 + 1 < IMXRT_XBAR_OUTPUTS) {
        s->sel[m * 2] = val & 0x7f;
        s->sel[m * 2 + 1] = (val >> 8) & 0x7f;
        for (int k = 0; k < 2; k++) {
            unsigned out = m * 2 + k;

            SP404_TRACE("xbar", "output %u <- input %u", out, s->sel[out]);
            qemu_set_irq(s->out[out], s->in_level[s->sel[out]]);
        }
        return;
    }
    s->ctrl[(offset / 2) & 1] = val;
}

static const MemoryRegionOps xbar_ops = {
    .read = xbar_read,
    .write = xbar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static void xbar_reset(DeviceState *dev)
{
    IMXRTXBAR *s = IMXRT_XBAR(dev);

    memset(s->sel, 0, sizeof(s->sel));
    memset(s->ctrl, 0, sizeof(s->ctrl));
}

static void xbar_realize(DeviceState *dev, Error **errp)
{
    IMXRTXBAR *s = IMXRT_XBAR(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &xbar_ops, s, "imxrt.xbar",
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    qdev_init_gpio_in_named(dev, xbar_input, "in", IMXRT_XBAR_INPUTS);
    qdev_init_gpio_out_named(dev, s->out, "out", IMXRT_XBAR_OUTPUTS);
}

static void xbar_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xbar_realize;
    device_class_set_legacy_reset(dc, xbar_reset);
}

static const TypeInfo xbar_info = {
    .name = TYPE_IMXRT_XBAR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTXBAR),
    .class_init = xbar_class_init,
};

static void xbar_register_types(void)
{
    type_register_static(&xbar_info);
}

type_init(xbar_register_types)
