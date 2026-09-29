/*
 * SP-404MKII emulator: register stub for unmodelled peripheral space.
 *
 * Behaves like a bank of plain registers: a read returns the last value
 * written, adjusted by a table of forced bits (status flags the firmware
 * polls, such as PLL lock). Ranges listed as SCT give each group of four
 * words i.MX's register/SET/CLR/TOG behaviour. Every access is logged under
 * -d unimp, and the first access to each register is logged even without
 * it, so a boot log shows which peripherals the firmware touches and from
 * where. The PC logged is exact with -accel tcg,one-insn-per-tb=on and
 * otherwise the start of the translation block.
 *
 * Real device models are mapped over this with a higher priority as they
 * are written.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "hw/arm/sp404/sp404.h"

bool sp404_trace_enabled(const char *dev)
{
    const char *e = getenv("SP404_TRACE");
    g_auto(GStrv) names = NULL;

    if (!e) {
        return false;
    }
    names = g_strsplit(e, ",", -1);
    for (int i = 0; names[i]; i++) {
        if (!strcmp(names[i], dev) || !strcmp(names[i], "all")) {
            return true;
        }
    }
    return false;
}

static uint32_t stub_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static const SP404StubBits *stub_find(SP404RegStub *s, hwaddr abs)
{
    const SP404StubBits *b;

    for (b = s->bits; b && b->addr; b++) {
        if (b->addr == abs) {
            return b;
        }
    }
    return NULL;
}

static bool stub_is_sct(SP404RegStub *s, hwaddr abs)
{
    const SP404StubRange *r;

    for (r = s->sct; r && r->end; r++) {
        if (abs >= r->start && abs < r->end) {
            return true;
        }
    }
    return false;
}

static bool stub_first_touch(SP404RegStub *s, hwaddr offset)
{
    uint32_t idx = offset >> 2;
    uint8_t bit = 1u << (idx & 7);

    if (s->seen[idx >> 3] & bit) {
        return false;
    }
    s->seen[idx >> 3] |= bit;
    return true;
}

static uint64_t stub_read(void *opaque, hwaddr offset, unsigned size)
{
    SP404RegStub *s = opaque;
    hwaddr abs = s->base + offset;
    hwaddr reg = offset & ~3ULL;
    const SP404StubBits *b;
    uint32_t word;
    uint64_t val;

    if (stub_is_sct(s, abs)) {
        reg &= ~0xfULL;         /* SET/CLR/TOG read back the register */
    }
    word = s->regs[reg >> 2];
    b = stub_find(s, s->base + reg);
    if (b) {
        word = (word | b->set) & ~b->clear;
    }
    val = extract32(word, (offset & 3) * 8, size * 8);
    if (stub_first_touch(s, offset & ~3ULL)) {
        qemu_log("sp404-stub: first read  0x%08" HWADDR_PRIx " = 0x%08" PRIx64
                 "  pc 0x%08x\n", abs, val, stub_pc());
    }
    qemu_log_mask(LOG_UNIMP, "sp404-stub: read  0x%08" HWADDR_PRIx
                  " (%u) = 0x%" PRIx64 "  pc 0x%08x\n", abs, size, val,
                  stub_pc());
    return val;
}

static void stub_write(void *opaque, hwaddr offset, uint64_t val,
                       unsigned size)
{
    SP404RegStub *s = opaque;
    hwaddr abs = s->base + offset;
    uint32_t *word;
    uint32_t v;

    if (stub_is_sct(s, abs) && (offset & 0xc)) {
        word = &s->regs[(offset & ~0xfULL) >> 2];
        v = deposit32(0, (offset & 3) * 8, size * 8, val);
        switch (offset & 0xc) {
        case 4: *word |= v; break;
        case 8: *word &= ~v; break;
        default: *word ^= v; break;
        }
    } else {
        word = &s->regs[offset >> 2];
        *word = deposit32(*word, (offset & 3) * 8, size * 8, val);
    }
    if (stub_first_touch(s, offset & ~3ULL)) {
        qemu_log("sp404-stub: first write 0x%08" HWADDR_PRIx " = 0x%08" PRIx64
                 "  pc 0x%08x\n", abs, val, stub_pc());
    }
    qemu_log_mask(LOG_UNIMP, "sp404-stub: write 0x%08" HWADDR_PRIx
                  " (%u) = 0x%" PRIx64 "  pc 0x%08x\n", abs, size, val,
                  stub_pc());
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void stub_realize(DeviceState *dev, Error **errp)
{
    SP404RegStub *s = SP404_REGSTUB(dev);

    if (!s->size || (s->size & 3)) {
        error_setg(errp, "sp404-regstub: size must be a nonzero multiple of 4");
        return;
    }
    s->regs = g_new0(uint32_t, s->size / 4);
    s->seen = g_new0(uint8_t, s->size / 32 + 1);
    memory_region_init_io(&s->iomem, OBJECT(s), &stub_ops, s,
                          s->name ? s->name : "sp404-regstub", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property stub_properties[] = {
    DEFINE_PROP_STRING("name", SP404RegStub, name),
    DEFINE_PROP_UINT64("base", SP404RegStub, base, 0),
    DEFINE_PROP_UINT64("size", SP404RegStub, size, 0),
};

static void stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = stub_realize;
    device_class_set_props(dc, stub_properties);
}

static const TypeInfo stub_info = {
    .name = TYPE_SP404_REGSTUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SP404RegStub),
    .class_init = stub_class_init,
};

static void stub_register_types(void)
{
    type_register_static(&stub_info);
}

type_init(stub_register_types)
