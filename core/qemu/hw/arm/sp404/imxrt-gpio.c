/*
 * i.MX RT GPIO bank: DR, GDIR, PSR, ICR1/2, IMR, ISR, EDGE_SEL and the RT's
 * DR_SET/DR_CLEAR/DR_TOGGLE aliases.
 *
 * Inputs are qdev GPIO inputs (the level on each pad); outputs are named
 * GPIO outputs "out" that follow DR where GDIR makes the pin an output.
 * The interrupt outputs are sysbus IRQs 0 (pins 0-15), 1 (pins 16-31) and
 * 2-9 (the per-pin lines for pins 0-7 that GPIO1 alone has wired).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "hw/arm/sp404/sp404.h"

enum {
    R_DR = 0x00 / 4, R_GDIR = 0x04 / 4, R_PSR = 0x08 / 4,
    R_ICR1 = 0x0c / 4, R_ICR2 = 0x10 / 4, R_IMR = 0x14 / 4,
    R_ISR = 0x18 / 4, R_EDGE_SEL = 0x1c / 4,
    R_DR_SET = 0x84 / 4, R_DR_CLEAR = 0x88 / 4, R_DR_TOGGLE = 0x8c / 4,
};

static uint32_t gpio_pad_levels(IMXRTGPIO *s)
{
    if (s->in_hook) {
        s->in = s->in_hook(s->in_hook_opaque, s->in);
    }
    /* Output pins read back what is driven; inputs what the pad sees. */
    return (s->dr & s->gdir) | (s->in & ~s->gdir);
}

static void gpio_update(IMXRTGPIO *s)
{
    uint32_t pending = s->isr & s->imr;
    uint32_t outs = s->dr & s->gdir;
    uint32_t changed = outs ^ s->last_out;
    int i;

    qemu_set_irq(s->irq[0], !!(pending & 0x0000ffff));
    qemu_set_irq(s->irq[1], !!(pending & 0xffff0000));
    for (i = 0; i < 8; i++) {
        qemu_set_irq(s->irq[2 + i], !!(pending & (1u << i)));
    }
    for (i = 0; changed; i++, changed >>= 1) {
        if (changed & 1) {
            SP404_TRACE("gpio", "%s pin %d -> %d", s->name ? s->name : "?", i,
                        (outs >> i) & 1);
            qemu_set_irq(s->out[i], (outs >> i) & 1);
        }
    }
    s->last_out = outs;
}

/* Interrupt configuration for pin n: 0 low, 1 high, 2 rising, 3 falling. */
static unsigned gpio_icr(IMXRTGPIO *s, int n)
{
    uint32_t icr = n < 16 ? s->icr1 : s->icr2;

    return (icr >> ((n & 15) * 2)) & 3;
}

static void gpio_eval_pin(IMXRTGPIO *s, int n, bool old, bool new)
{
    uint32_t bit = 1u << n;

    if (s->edge_sel & bit) {
        if (old != new) {
            s->isr |= bit;
        }
        return;
    }
    switch (gpio_icr(s, n)) {
    case 0: if (!new) s->isr |= bit; break;
    case 1: if (new) s->isr |= bit; break;
    case 2: if (!old && new) s->isr |= bit; break;
    case 3: if (old && !new) s->isr |= bit; break;
    }
}

static void gpio_set_input(void *opaque, int n, int level)
{
    IMXRTGPIO *s = opaque;
    bool old = (s->in >> n) & 1;

    s->in = deposit32(s->in, n, 1, !!level);
    if (!((s->gdir >> n) & 1)) {
        gpio_eval_pin(s, n, old, !!level);
    }
    gpio_update(s);
}

/* Level-triggered interrupts re-assert while the level persists. */
static void gpio_relevel(IMXRTGPIO *s)
{
    uint32_t pads = gpio_pad_levels(s);
    int n;

    for (n = 0; n < 32; n++) {
        if (!(s->edge_sel & (1u << n)) && gpio_icr(s, n) < 2) {
            bool lvl = (pads >> n) & 1;
            gpio_eval_pin(s, n, lvl, lvl);
        }
    }
}

static uint64_t gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTGPIO *s = opaque;

    switch (offset / 4) {
    case R_DR:
        SP404_TRACE("gpio-rd", "%s DR = 0x%08x (in 0x%08x gdir 0x%08x) pc 0x%08x",
                    s->name ? s->name : "?", gpio_pad_levels(s), s->in,
                    s->gdir, current_cpu ?
                    ARM_CPU(current_cpu)->env.regs[15] : 0);
        return gpio_pad_levels(s);
    case R_GDIR:
        return s->gdir;
    case R_PSR:
        return gpio_pad_levels(s);
    case R_ICR1:
        return s->icr1;
    case R_ICR2:
        return s->icr2;
    case R_IMR:
        return s->imr;
    case R_ISR:
        return s->isr;
    case R_EDGE_SEL:
        return s->edge_sel;
    case R_DR_SET: case R_DR_CLEAR: case R_DR_TOGGLE:
        return 0;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "imxrt-gpio: read of 0x%" HWADDR_PRIx "\n",
                  offset);
    return 0;
}

static void gpio_write(void *opaque, hwaddr offset, uint64_t val,
                       unsigned size)
{
    IMXRTGPIO *s = opaque;

    switch (offset / 4) {
    case R_DR:
        s->dr = val;
        break;
    case R_GDIR:
        s->gdir = val;
        break;
    case R_ICR1:
        s->icr1 = val;
        break;
    case R_ICR2:
        s->icr2 = val;
        break;
    case R_IMR:
        s->imr = val;
        break;
    case R_ISR:
        s->isr &= ~val;
        gpio_relevel(s);
        break;
    case R_EDGE_SEL:
        s->edge_sel = val;
        break;
    case R_DR_SET:
        s->dr |= val;
        break;
    case R_DR_CLEAR:
        s->dr &= ~val;
        break;
    case R_DR_TOGGLE:
        s->dr ^= val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-gpio: write of 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    gpio_update(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gpio_reset(DeviceState *dev)
{
    IMXRTGPIO *s = IMXRT_GPIO(dev);

    s->dr = s->gdir = s->icr1 = s->icr2 = s->imr = s->isr = 0;
    s->edge_sel = s->last_out = 0;
    s->in = s->reset_in;
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    IMXRTGPIO *s = IMXRT_GPIO(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(s), &gpio_ops, s, "imxrt.gpio",
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    for (i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    qdev_init_gpio_in(dev, gpio_set_input, 32);
    qdev_init_gpio_out_named(dev, s->out, "out", 32);
}

static const Property gpio_properties[] = {
    /* Pad levels at power-on: pull-ups and signals already present. */
    DEFINE_PROP_UINT32("reset-in", IMXRTGPIO, reset_in, 0),
    DEFINE_PROP_STRING("name", IMXRTGPIO, name),
};

static void gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gpio_realize;
    device_class_set_legacy_reset(dc, gpio_reset);
    device_class_set_props(dc, gpio_properties);
}

static const TypeInfo gpio_info = {
    .name = TYPE_IMXRT_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTGPIO),
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)
