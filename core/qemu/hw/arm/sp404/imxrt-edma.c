/*
 * i.MX RT eDMA (32 channels) with its DMAMUX.
 *
 * MMIO region 0 is the eDMA (control registers and the TCDs at 0x1000),
 * region 1 the DMAMUX (CHCFG per channel). Peripherals raise DMA requests
 * on the GPIO inputs "dreq", numbered by DMAMUX request source; a channel
 * whose CHCFG routes an asserted source to it, or has A_ON, and whose ERQ is
 * set, runs minor loops until the request drops or ERQ is cleared.
 *
 * Transfers happen synchronously in the context that raised the request.
 * That suits peripherals that consume data instantly (SPI to a display);
 * paced peripherals (SAI audio) pace themselves by how they raise requests.
 * A request that stays up without end is serviced in slices from a bottom
 * half so the CPU still runs.
 *
 * sysbus IRQ n (0-15) is the shared line for channels n and n+16; IRQ 16 is
 * the error interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/arm/sp404/sp404.h"

#define R_CR    0x00
#define   CR_EMLM   (1u << 7)
#define   CR_HALT   (1u << 5)
#define R_ES    0x04
#define R_ERQ   0x0c
#define R_EEI   0x14
#define R_CEEI  0x18
#define R_SEEI  0x19
#define R_CERQ  0x1a
#define R_SERQ  0x1b
#define R_CDNE  0x1c
#define R_SSRT  0x1d
#define R_CERR  0x1e
#define R_CINT  0x1f
#define R_INT   0x24
#define R_ERR   0x2c
#define R_HRS   0x34
#define R_EARS  0x44
#define R_DCHPRI 0x100
#define R_TCD   0x1000

/* TCD word layout (byte offsets in the 32-byte TCD). */
#define T_SADDR     0x00
#define T_SOFF      0x04
#define T_ATTR      0x06
#define T_NBYTES    0x08
#define T_SLAST     0x0c
#define T_DADDR     0x10
#define T_DOFF      0x14
#define T_CITER     0x16
#define T_DLASTSGA  0x18
#define T_CSR       0x1c
#define T_BITER     0x1e

#define CSR_START       (1u << 0)
#define CSR_INTMAJOR    (1u << 1)
#define CSR_INTHALF     (1u << 2)
#define CSR_DREQ        (1u << 3)
#define CSR_ESG         (1u << 4)
#define CSR_MAJORELINK  (1u << 5)
#define CSR_ACTIVE      (1u << 6)
#define CSR_DONE        (1u << 7)

#define CHCFG_ENBL  (1u << 31)
#define CHCFG_A_ON  (1u << 29)

/* Minor loops serviced before yielding to the CPU. */
#define EDMA_SLICE  4096

static uint32_t tcd32(IMXRTEDMA *s, int ch, int off)
{
    return ldl_le_p(&s->tcd[ch][off]);
}

static uint16_t tcd16(IMXRTEDMA *s, int ch, int off)
{
    return lduw_le_p(&s->tcd[ch][off]);
}

static void set32(IMXRTEDMA *s, int ch, int off, uint32_t v)
{
    stl_le_p(&s->tcd[ch][off], v);
}

static void set16(IMXRTEDMA *s, int ch, int off, uint16_t v)
{
    stw_le_p(&s->tcd[ch][off], v);
}

static void edma_update_irq(IMXRTEDMA *s)
{
    int i;

    for (i = 0; i < 16; i++) {
        uint32_t mask = (1u << i) | (1u << (i + 16));
        qemu_set_irq(s->irq[i], !!(s->intr & mask));
    }
    qemu_set_irq(s->irq[16], !!(s->err & s->eei));
}

static bool edma_requesting(IMXRTEDMA *s, int ch)
{
    uint32_t cfg = s->chcfg[ch];

    if (!(cfg & CHCFG_ENBL) || !(s->erq & (1u << ch))) {
        return false;
    }
    if (cfg & CHCFG_A_ON) {
        return true;
    }
    return test_bit(cfg & 0x7f, s->dreq);
}

static void edma_error(IMXRTEDMA *s, int ch, uint32_t es)
{
    s->es = es | (ch << 8) | (1u << 31);
    s->err |= 1u << ch;
    s->erq &= ~(1u << ch);
    qemu_log_mask(LOG_GUEST_ERROR, "imxrt-edma: channel %d error 0x%x\n",
                  ch, es);
    edma_update_irq(s);
}

static void edma_start(IMXRTEDMA *s, int ch)
{
    SP404_TRACE("edma", "ch%d start: %08x -> %08x nbytes %u citer %u", ch,
                tcd32(s, ch, T_SADDR), tcd32(s, ch, T_DADDR),
                tcd32(s, ch, T_NBYTES), tcd16(s, ch, T_CITER) & 0x7fff);
    uint16_t csr = tcd16(s, ch, T_CSR);

    set16(s, ch, T_CSR, csr | CSR_START);
    s->pending_start |= 1u << ch;
}

/* One minor loop of channel ch. */
static void edma_minor(IMXRTEDMA *s, int ch)
{
    uint32_t saddr = tcd32(s, ch, T_SADDR);
    uint32_t daddr = tcd32(s, ch, T_DADDR);
    int16_t soff = tcd16(s, ch, T_SOFF);
    int16_t doff = tcd16(s, ch, T_DOFF);
    uint16_t attr = tcd16(s, ch, T_ATTR);
    uint32_t nb = tcd32(s, ch, T_NBYTES);
    uint16_t citer = tcd16(s, ch, T_CITER);
    uint16_t biter = tcd16(s, ch, T_BITER);
    uint16_t csr = tcd16(s, ch, T_CSR);
    unsigned ssize = 1u << (attr >> 8 & 7), dsize = 1u << (attr & 7);
    unsigned smod = attr >> 11 & 0x1f, dmod = attr >> 3 & 0x1f;
    bool elink = citer & 0x8000;
    unsigned count_mask = elink ? 0x1ff : 0x7fff;
    uint32_t nbytes, done, have;
    int32_t mloff = 0;
    bool smloe = false, dmloe = false;
    uint8_t buf[64];

    if (ssize > 32 || dsize > 32 || (attr >> 8 & 7) == 4 || (attr & 7) == 4) {
        edma_error(s, ch, 1u << 5);     /* SAE/DAE-ish: bad size config */
        return;
    }
    if (s->cr & CR_EMLM) {
        smloe = nb >> 31;
        dmloe = nb >> 30 & 1;
        if (smloe || dmloe) {
            mloff = sextract32(nb, 10, 20);
            nbytes = nb & 0x3ff;
        } else {
            nbytes = nb & 0x3fffffff;
        }
    } else {
        nbytes = nb;
    }
    if (nbytes == 0) {
        nbytes = 4 * GiB - 1;   /* 0 means 4 GB; nobody wants that */
    }

    csr = (csr & ~CSR_START) | CSR_ACTIVE;
    set16(s, ch, T_CSR, csr);

    /* Read in source-size units, write in destination-size units. */
    done = have = 0;
    while (done < nbytes) {
        while (have < dsize && done + have < nbytes) {
            if (dma_memory_read(&address_space_memory, saddr, buf + have,
                                ssize, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                edma_error(s, ch, 1u << 2);     /* SBE: source bus error */
                return;
            }
            have += ssize;
            if (smod) {
                uint32_t m = (1u << smod) - 1;
                saddr = (saddr & ~m) | ((saddr + soff) & m);
            } else {
                saddr += soff;
            }
        }
        while (have >= dsize) {
            if (dma_memory_write(&address_space_memory, daddr, buf, dsize,
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                edma_error(s, ch, 1u << 0);     /* DBE: destination bus error */
                return;
            }
            memmove(buf, buf + dsize, have - dsize);
            have -= dsize;
            done += dsize;
            if (dmod) {
                uint32_t m = (1u << dmod) - 1;
                daddr = (daddr & ~m) | ((daddr + doff) & m);
            } else {
                daddr += doff;
            }
        }
        if (have && done + have >= nbytes) {
            break;              /* sizes that do not divide nbytes */
        }
    }
    if (smloe) {
        saddr += mloff;
    }
    if (dmloe) {
        daddr += mloff;
    }

    citer = (citer & ~count_mask) | (((citer & count_mask) - 1) & count_mask);
    if ((citer & count_mask) == 0) {
        /* Major loop complete. */
        SP404_TRACE("edma", "ch%d major loop done (csr 0x%04x) src %08x "
                    "dst %08x nbytes %u biter %u", ch, csr,
                    tcd32(s, ch, T_SADDR), tcd32(s, ch, T_DADDR), nbytes,
                    biter & count_mask);
        saddr += tcd32(s, ch, T_SLAST);
        csr = (csr & ~CSR_ACTIVE) | CSR_DONE;
        if (csr & CSR_INTMAJOR) {
            s->intr |= 1u << ch;
        }
        if (csr & CSR_DREQ) {
            s->erq &= ~(1u << ch);
        }
        set32(s, ch, T_SADDR, saddr);
        if (csr & CSR_ESG) {
            uint32_t sga = tcd32(s, ch, T_DLASTSGA);

            dma_memory_read(&address_space_memory, sga, s->tcd[ch], 32,
                            MEMTXATTRS_UNSPECIFIED);
            /* The new TCD starts idle; DONE stays visible. */
            csr = tcd16(s, ch, T_CSR) | CSR_DONE;
            set16(s, ch, T_CSR, csr & ~CSR_ACTIVE);
        } else {
            set32(s, ch, T_DADDR, daddr + tcd32(s, ch, T_DLASTSGA));
            set16(s, ch, T_CITER, biter);
            set16(s, ch, T_CSR, csr);
        }
        if (csr & CSR_MAJORELINK) {
            edma_start(s, (csr >> 8) & 0x1f);
        }
    } else {
        csr &= ~CSR_ACTIVE;
        if ((csr & CSR_INTHALF) &&
            (citer & count_mask) == ((biter & count_mask) >> 1)) {
            s->intr |= 1u << ch;
        }
        set32(s, ch, T_SADDR, saddr);
        set32(s, ch, T_DADDR, daddr);
        set16(s, ch, T_CITER, citer);
        set16(s, ch, T_CSR, csr);
        if (elink) {
            edma_start(s, (citer >> 9) & 0x1f);
        }
    }
    s->hrs &= ~(1u << ch);
}

static void edma_service(IMXRTEDMA *s)
{
    int budget = EDMA_SLICE;
    bool again;
    int ch;

    if (s->busy) {
        s->again = true;
        return;
    }
    s->busy = true;
    do {
        s->again = false;
        again = false;
        if (s->cr & CR_HALT) {
            break;
        }
        for (ch = 0; ch < IMXRT_EDMA_CHANNELS; ch++) {
            if (s->pending_start & (1u << ch)) {
                s->pending_start &= ~(1u << ch);
                edma_minor(s, ch);
                again = true;
                budget--;
            }
            /* One minor loop per channel per pass, as arbitration would. */
            if (budget > 0 && edma_requesting(s, ch)) {
                s->hrs |= 1u << ch;
                edma_minor(s, ch);
                budget--;
                again = true;
            }
        }
    } while ((again || s->again) && budget > 0);
    s->busy = false;
    edma_update_irq(s);
    if (budget <= 0) {
        qemu_bh_schedule(s->bh);
    }
}

static void edma_bh(void *opaque)
{
    edma_service(opaque);
}

static void edma_dreq(void *opaque, int src, int level)
{
    IMXRTEDMA *s = opaque;

    if (level) {
        set_bit(src, s->dreq);
        edma_service(s);
    } else {
        clear_bit(src, s->dreq);
    }
}

static uint64_t edma_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTEDMA *s = opaque;
    uint64_t v = 0;

    if (offset >= R_TCD && offset < R_TCD + IMXRT_EDMA_CHANNELS * 32) {
        unsigned ch = (offset - R_TCD) / 32, off = (offset - R_TCD) % 32;

        memcpy(&v, &s->tcd[ch][off], size);
        return le64_to_cpu(v);
    }
    if (offset >= R_DCHPRI && offset < R_DCHPRI + IMXRT_EDMA_CHANNELS) {
        memcpy(&v, &s->dchpri[offset - R_DCHPRI], size);
        return v;
    }
    switch (offset) {
    case R_CR: return s->cr;
    case R_ES: return s->es;
    case R_ERQ: return s->erq;
    case R_EEI: return s->eei;
    case R_INT: return s->intr;
    case R_ERR: return s->err;
    case R_HRS: return s->hrs;
    case R_EARS: return s->ears;
    }
    if (offset >= R_CEEI && offset <= R_CINT) {
        return 0;               /* write-only byte registers */
    }
    qemu_log_mask(LOG_GUEST_ERROR, "imxrt-edma: read 0x%" HWADDR_PRIx "\n",
                  offset);
    return 0;
}

/* The byte registers at 0x18-0x1f: bit 6 means all channels. */
static void edma_byte_cmd(IMXRTEDMA *s, hwaddr reg, uint8_t v)
{
    uint32_t mask = (v & 0x40) ? 0xffffffff : 1u << (v & 0x1f);
    int ch;

    if (v & 0x80) {
        return;                 /* NOP bit */
    }
    switch (reg) {
    case R_CEEI: s->eei &= ~mask; break;
    case R_SEEI: s->eei |= mask; break;
    case R_CERQ: s->erq &= ~mask; break;
    case R_SERQ:
        SP404_TRACE("edma", "SERQ 0x%02x", v);
        s->erq |= mask;
        break;
    case R_CERR: s->err &= ~mask; break;
    case R_CINT:
        SP404_TRACE("edma", "CINT 0x%02x", v);
        s->intr &= ~mask;
        break;
    case R_CDNE:
        for (ch = 0; ch < IMXRT_EDMA_CHANNELS; ch++) {
            if (mask & (1u << ch)) {
                set16(s, ch, T_CSR, tcd16(s, ch, T_CSR) & ~CSR_DONE);
            }
        }
        break;
    case R_SSRT:
        for (ch = 0; ch < IMXRT_EDMA_CHANNELS; ch++) {
            if (mask & (1u << ch)) {
                edma_start(s, ch);
            }
        }
        break;
    }
}

static void edma_write(void *opaque, hwaddr offset, uint64_t val,
                       unsigned size)
{
    IMXRTEDMA *s = opaque;
    unsigned i;

    if (offset >= R_TCD && offset < R_TCD + IMXRT_EDMA_CHANNELS * 32) {
        unsigned ch = (offset - R_TCD) / 32, off = (offset - R_TCD) % 32;
        uint64_t le = cpu_to_le64(val);

        memcpy(&s->tcd[ch][off], &le, size);
        if (off <= T_CSR && off + size > T_CSR &&
            (tcd16(s, ch, T_CSR) & CSR_START)) {
            SP404_TRACE("edma", "ch%d START via TCD", ch);
            s->pending_start |= 1u << ch;
        }
    } else if (offset >= R_DCHPRI &&
               offset < R_DCHPRI + IMXRT_EDMA_CHANNELS) {
        memcpy(&s->dchpri[offset - R_DCHPRI], &val, size);
        return;
    } else if (offset >= R_CEEI && offset <= R_CINT) {
        for (i = 0; i < size; i++) {
            edma_byte_cmd(s, offset + i, val >> (8 * i));
        }
    } else {
        switch (offset) {
        case R_CR: s->cr = val & ~(1u << 17); break;    /* CX: no-op */
        case R_ERQ:
            SP404_TRACE("edma", "ERQ = 0x%08x", (uint32_t)val);
            s->erq = val;
            break;
        case R_EEI: s->eei = val; break;
        case R_INT:
            SP404_TRACE("edma", "INT w1c 0x%08x", (uint32_t)val);
            s->intr &= ~val;
            break;
        case R_ERR: s->err &= ~val; break;
        case R_EARS: s->ears = val; break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "imxrt-edma: write 0x%"
                          HWADDR_PRIx "\n", offset);
            return;
        }
    }
    edma_service(s);
}

static uint64_t dmamux_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTEDMA *s = opaque;

    return offset / 4 < IMXRT_EDMA_CHANNELS ? s->chcfg[offset / 4] : 0;
}

static void dmamux_write(void *opaque, hwaddr offset, uint64_t val,
                         unsigned size)
{
    IMXRTEDMA *s = opaque;

    if (offset / 4 < IMXRT_EDMA_CHANNELS) {
        s->chcfg[offset / 4] = val;
        edma_service(s);
    }
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps dmamux_ops = {
    .read = dmamux_read,
    .write = dmamux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void edma_reset(DeviceState *dev)
{
    IMXRTEDMA *s = IMXRT_EDMA(dev);
    int i;

    s->cr = s->es = s->erq = s->eei = s->intr = s->err = s->hrs = 0;
    s->ears = s->pending_start = 0;
    memset(s->tcd, 0, sizeof(s->tcd));
    memset(s->chcfg, 0, sizeof(s->chcfg));
    for (i = 0; i < IMXRT_EDMA_CHANNELS; i++) {
        s->dchpri[i] = i & 0xf;
    }
}

static void edma_realize(DeviceState *dev, Error **errp)
{
    IMXRTEDMA *s = IMXRT_EDMA(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(s), &edma_ops, s, "imxrt.edma",
                          0x2000);
    memory_region_init_io(&s->mux_iomem, OBJECT(s), &dmamux_ops, s,
                          "imxrt.dmamux", 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mux_iomem);
    for (i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    qdev_init_gpio_in_named(dev, edma_dreq, "dreq", IMXRT_DMAMUX_SOURCES);
    s->bh = qemu_bh_new(edma_bh, s);
}

static void edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = edma_realize;
    device_class_set_legacy_reset(dc, edma_reset);
}

static const TypeInfo edma_info = {
    .name = TYPE_IMXRT_EDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTEDMA),
    .class_init = edma_class_init,
};

static void edma_register_types(void)
{
    type_register_static(&edma_info);
}

type_init(edma_register_types)
