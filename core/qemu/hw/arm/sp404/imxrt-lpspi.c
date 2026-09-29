/*
 * i.MX RT LPSPI, master mode, driving a QEMU SSI bus.
 *
 * Words written to TDR go through a 16-entry TX FIFO and are shifted out
 * at once, byte by byte (MSB first unless LSBF), with TCR's byte swap
 * applied; what comes back goes to the 16-entry RX FIFO unless RXMSK. The
 * TX side stalls while the RX FIFO is full, as the hardware does unless
 * CFGR1.NOSTALL. Chip selects are GPIO outputs "cs" (active low), held
 * across frames while TCR.CONT is set.
 *
 * DMA requests are GPIO outputs "tx-dreq" and "rx-dreq", for the eDMA's
 * dreq inputs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/sp404/sp404.h"

#define R_VERID 0x00
#define R_PARAM 0x04
#define R_CR    0x10
#define   CR_MEN    (1u << 0)
#define   CR_RST    (1u << 1)
#define   CR_RTF    (1u << 8)
#define   CR_RRF    (1u << 9)
#define R_SR    0x14
#define   SR_TDF    (1u << 0)
#define   SR_RDF    (1u << 1)
#define   SR_WCF    (1u << 8)
#define   SR_FCF    (1u << 9)
#define   SR_TCF    (1u << 10)
#define   SR_TEF    (1u << 11)
#define   SR_REF    (1u << 12)
#define   SR_DMF    (1u << 13)
#define   SR_W1C    (SR_WCF | SR_FCF | SR_TCF | SR_TEF | SR_REF | SR_DMF)
#define R_IER   0x18
#define R_DER   0x1c
#define   DER_TDDE  (1u << 0)
#define   DER_RDDE  (1u << 1)
#define R_CFGR0 0x20
#define R_CFGR1 0x24
#define   CFGR1_NOSTALL (1u << 3)
#define R_DMR0  0x30
#define R_DMR1  0x34
#define R_CCR   0x40
#define R_FCR   0x58
#define R_FSR   0x5c
#define R_TCR   0x60
#define   TCR_TXMSK (1u << 18)
#define   TCR_RXMSK (1u << 19)
#define   TCR_CONT  (1u << 21)
#define   TCR_BYSW  (1u << 22)
#define   TCR_LSBF  (1u << 23)
#define R_TDR   0x64
#define R_RSR   0x70
#define   RSR_SOF     (1u << 0)
#define   RSR_RXEMPTY (1u << 1)
#define R_RDR   0x74

#define FIFO    IMXRT_LPSPI_FIFO

static void lpspi_update(IMXRTLPSPI *s)
{
    uint32_t sr = s->sr & ~(SR_TDF | SR_RDF);

    if (s->tx_count <= (s->fcr & 0xf)) {
        sr |= SR_TDF;
    }
    if (s->rx_count > ((s->fcr >> 16) & 0xf)) {
        sr |= SR_RDF;
    }
    s->sr = sr;
    qemu_set_irq(s->irq, !!(s->sr & s->ier));
    qemu_set_irq(s->tx_dreq, (s->cr & CR_MEN) && (s->der & DER_TDDE) &&
                 (sr & SR_TDF));
    qemu_set_irq(s->rx_dreq, (s->der & DER_RDDE) && (sr & SR_RDF));
}

static void lpspi_cs(IMXRTLPSPI *s, bool active)
{
    unsigned pcs = (s->tcr >> 24) & 3;

    if (active && s->cs_active != (int)pcs) {
        if (s->cs_active >= 0) {
            qemu_irq_raise(s->cs[s->cs_active]);
        }
        qemu_irq_lower(s->cs[pcs]);
        s->cs_active = pcs;
    } else if (!active && s->cs_active >= 0) {
        qemu_irq_raise(s->cs[s->cs_active]);
        s->cs_active = -1;
        s->sr |= SR_FCF;
    }
}

/* Shift one frame (up to 32 bits) out and in. */
static uint32_t lpspi_shift(IMXRTLPSPI *s, uint32_t word)
{
    unsigned bits = (s->tcr & 0xfff) + 1;
    unsigned nbytes = DIV_ROUND_UP(MIN(bits, 32), 8);
    uint32_t in = 0;
    unsigned i;

    if (s->tcr & TCR_BYSW) {
        word = bswap32(word);
    }
    lpspi_cs(s, true);
    for (i = 0; i < nbytes; i++) {
        unsigned shift = (s->tcr & TCR_LSBF) ? 8 * i : 8 * (nbytes - 1 - i);
        uint8_t out = word >> shift;

        if (s->tcr & TCR_LSBF) {
            out = revbit8(out);
        }
        uint8_t got = ssi_transfer(s->bus, out);

        SP404_TRACE("lpspi", "%p cs%u tx %02x rx %02x", s, s->cs_active,
                    out, got);
        if (s->tcr & TCR_LSBF) {
            got = revbit8(got);
        }
        in |= (uint32_t)got << shift;
    }
    if (!(s->tcr & TCR_CONT)) {
        lpspi_cs(s, false);
    }
    s->sr |= SR_WCF;
    if (s->tcr & TCR_BYSW) {
        in = bswap32(in);
    }
    return in;
}

static void lpspi_rx_push(IMXRTLPSPI *s, uint32_t word)
{
    if (s->tcr & TCR_RXMSK) {
        return;
    }
    if (s->rx_count == FIFO) {
        s->sr |= SR_REF;
        return;
    }
    s->rxf[(s->rx_head + s->rx_count++) % FIFO] = word;
}

static void lpspi_run(IMXRTLPSPI *s)
{
    if (!(s->cr & CR_MEN)) {
        return;
    }
    while (s->tx_count) {
        if (!(s->tcr & TCR_RXMSK) && s->rx_count == FIFO &&
            !(s->cfgr1 & CFGR1_NOSTALL)) {
            return;             /* stalled on a full RX FIFO */
        }
        uint32_t w = s->txf[s->tx_head];
        s->tx_head = (s->tx_head + 1) % FIFO;
        s->tx_count--;
        lpspi_rx_push(s, lpspi_shift(s, w));
    }
    s->sr |= SR_TCF;
}

static void lpspi_soft_reset(IMXRTLPSPI *s)
{
    s->sr = 0;
    s->ier = s->der = s->cfgr0 = s->cfgr1 = s->dmr0 = s->dmr1 = 0;
    s->ccr = s->fcr = 0;
    s->tcr = 0x1f;
    s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
    lpspi_cs(s, false);
    s->sr &= ~SR_FCF;
}

static uint64_t lpspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTLPSPI *s = opaque;
    uint32_t v = 0;
    hwaddr reg = offset & ~3;

    switch (reg) {
    case R_VERID: v = 0x01020004; break;
    case R_PARAM: v = 0x00000404; break;       /* 16-word FIFOs */
    case R_CR: v = s->cr; break;
    case R_SR: v = s->sr; break;
    case R_IER: v = s->ier; break;
    case R_DER: v = s->der; break;
    case R_CFGR0: v = s->cfgr0; break;
    case R_CFGR1: v = s->cfgr1; break;
    case R_DMR0: v = s->dmr0; break;
    case R_DMR1: v = s->dmr1; break;
    case R_CCR: v = s->ccr; break;
    case R_FCR: v = s->fcr; break;
    case R_FSR: v = s->tx_count | (s->rx_count << 16); break;
    case R_TCR: v = s->tcr; break;
    case R_RSR: v = s->rx_count ? 0 : RSR_RXEMPTY; break;
    case R_RDR:
        if (s->rx_count) {
            v = s->rxf[s->rx_head];
            s->rx_head = (s->rx_head + 1) % FIFO;
            s->rx_count--;
            lpspi_run(s);       /* a stalled TX side can go on */
            lpspi_update(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-lpspi: read 0x%" HWADDR_PRIx
                      "\n", offset);
    }
    return extract32(v, (offset & 3) * 8, size * 8);
}

static void lpspi_write(void *opaque, hwaddr offset, uint64_t val,
                        unsigned size)
{
    IMXRTLPSPI *s = opaque;
    hwaddr reg = offset & ~3;
    /* A narrow write lands in its byte lane; the rest reads as zero. */
    uint32_t v = (uint32_t)val << ((offset & 3) * 8);

    SP404_TRACE("lpspi", "%p write 0x%02x = 0x%08x (cr 0x%x tx %u rx %u)", s,
                (unsigned)offset, v, s->cr, s->tx_count, s->rx_count);
    switch (reg) {
    case R_CR:
        if (v & CR_RST) {
            lpspi_soft_reset(s);
        }
        if (v & CR_RTF) {
            s->tx_head = s->tx_count = 0;
        }
        if (v & CR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        s->cr = v & (CR_MEN | CR_RST | (1u << 2) | (1u << 3));
        lpspi_run(s);
        break;
    case R_SR: s->sr &= ~(v & SR_W1C); break;
    case R_IER: s->ier = v; break;
    case R_DER: s->der = v; break;
    case R_CFGR0: s->cfgr0 = v; break;
    case R_CFGR1: s->cfgr1 = v; break;
    case R_DMR0: s->dmr0 = v; break;
    case R_DMR1: s->dmr1 = v; break;
    case R_CCR: s->ccr = v; break;
    case R_FCR: s->fcr = v; break;
    case R_TCR:
        s->tcr = v;
        if (!(v & TCR_CONT)) {
            lpspi_cs(s, false);
        }
        if ((v & TCR_TXMSK) && (s->cr & CR_MEN)) {
            lpspi_rx_push(s, lpspi_shift(s, 0));  /* receive-only frame */
        }
        break;
    case R_TDR:
        if (s->tx_count == FIFO) {
            s->sr |= SR_TEF;
            break;
        }
        s->txf[(s->tx_head + s->tx_count++) % FIFO] = v;
        lpspi_run(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-lpspi: write 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    lpspi_update(s);
}

static const MemoryRegionOps lpspi_ops = {
    .read = lpspi_read,
    .write = lpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpspi_reset(DeviceState *dev)
{
    IMXRTLPSPI *s = IMXRT_LPSPI(dev);

    s->cr = 0;
    s->cs_active = -1;
    lpspi_soft_reset(s);
    lpspi_update(s);
}

static void lpspi_realize(DeviceState *dev, Error **errp)
{
    IMXRTLPSPI *s = IMXRT_LPSPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &lpspi_ops, s, "imxrt.lpspi",
                          0x4000);
    /*
     * The eDMA writes back into this device from inside its own register
     * handlers (a request raised by a register write is serviced at once).
     */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->bus = ssi_create_bus(dev, "ssi");
    qdev_init_gpio_out_named(dev, s->cs, "cs", 4);
    qdev_init_gpio_out_named(dev, &s->tx_dreq, "tx-dreq", 1);
    qdev_init_gpio_out_named(dev, &s->rx_dreq, "rx-dreq", 1);
}

static void lpspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpspi_realize;
    device_class_set_legacy_reset(dc, lpspi_reset);
}

static const TypeInfo lpspi_info = {
    .name = TYPE_IMXRT_LPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTLPSPI),
    .class_init = lpspi_class_init,
};

static void lpspi_register_types(void)
{
    type_register_static(&lpspi_info);
}

type_init(lpspi_register_types)
