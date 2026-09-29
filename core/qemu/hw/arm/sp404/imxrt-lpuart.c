/*
 * i.MX RT LPUART.
 *
 * Transmission is instant: DATA writes go straight out, so TDRE and TC
 * stay set. Received bytes queue in a FIFO deeper than the hardware's
 * (the far end cannot be flow-controlled); RDRF follows the RX watermark
 * when the FIFO is enabled, else any data. IDLE is raised when a burst of
 * input has been taken, which is what the SDK's idle-line receive expects.
 *
 * The far end is a chardev ("chardev" property) or, for a peer modelled
 * inside the machine (the SP-404's BMC), the tx callback plus
 * imxrt_lpuart_receive().
 *
 * DMA requests are GPIO outputs "tx-dreq" and "rx-dreq".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "hw/arm/sp404/sp404.h"

#define R_VERID  0x00
#define R_PARAM  0x04
#define R_GLOBAL 0x08
#define   GLOBAL_RST (1u << 1)
#define R_PINCFG 0x0c
#define R_BAUD   0x10
#define   BAUD_RDMAE (1u << 21)
#define   BAUD_TDMAE (1u << 23)
#define R_STAT   0x14
#define   STAT_MA2F  (1u << 14)
#define   STAT_MA1F  (1u << 15)
#define   STAT_PF    (1u << 16)
#define   STAT_FE    (1u << 17)
#define   STAT_NF    (1u << 18)
#define   STAT_OR    (1u << 19)
#define   STAT_IDLE  (1u << 20)
#define   STAT_RDRF  (1u << 21)
#define   STAT_TC    (1u << 22)
#define   STAT_TDRE  (1u << 23)
#define   STAT_RXEDGIF (1u << 30)
#define   STAT_LBKDIF (1u << 31)
#define   STAT_W1C   (STAT_MA2F | STAT_MA1F | STAT_PF | STAT_FE | STAT_NF | \
                      STAT_OR | STAT_IDLE | STAT_RXEDGIF | STAT_LBKDIF)
#define R_CTRL   0x18
#define   CTRL_RE    (1u << 18)
#define   CTRL_TE    (1u << 19)
#define   CTRL_ILIE  (1u << 20)
#define   CTRL_RIE   (1u << 21)
#define   CTRL_TCIE  (1u << 22)
#define   CTRL_TIE   (1u << 23)
#define   CTRL_ORIE  (1u << 27)
#define R_DATA   0x1c
#define R_MATCH  0x20
#define R_MODIR  0x24
#define R_FIFO   0x28
#define   FIFO_RXFE     (1u << 3)
#define   FIFO_TXFE     (1u << 7)
#define   FIFO_RXFLUSH  (1u << 14)
#define   FIFO_TXFLUSH  (1u << 15)
#define   FIFO_RXEMPT   (1u << 22)
#define   FIFO_TXEMPT   (1u << 23)
#define   FIFO_SIZES    0x11            /* 4-entry TX and RX FIFOs */
#define R_WATER  0x2c

static unsigned lpuart_rx_ready_level(IMXRTLPUART *s)
{
    if (s->fifo & FIFO_RXFE) {
        return ((s->water >> 16) & 3) + 1;
    }
    return 1;
}

static void lpuart_update(IMXRTLPUART *s)
{
    uint32_t stat = s->stat | STAT_TDRE | STAT_TC;
    bool irq;

    stat &= ~STAT_RDRF;
    if (s->rx_count >= lpuart_rx_ready_level(s)) {
        stat |= STAT_RDRF;
    }
    s->stat = stat;
    irq = ((s->ctrl & CTRL_TIE) && (stat & STAT_TDRE)) ||
          ((s->ctrl & CTRL_TCIE) && (stat & STAT_TC)) ||
          ((s->ctrl & CTRL_RIE) && (stat & STAT_RDRF)) ||
          ((s->ctrl & CTRL_ILIE) && (stat & STAT_IDLE)) ||
          ((s->ctrl & CTRL_ORIE) && (stat & STAT_OR));
    qemu_set_irq(s->irq, irq);
    qemu_set_irq(s->tx_dreq, (s->baud & BAUD_TDMAE) && (s->ctrl & CTRL_TE));
    qemu_set_irq(s->rx_dreq, (s->baud & BAUD_RDMAE) && (stat & STAT_RDRF));
}

void imxrt_lpuart_receive(IMXRTLPUART *s, const uint8_t *buf, int len)
{
    int i;

    if (!(s->ctrl & CTRL_RE)) {
        return;
    }
    for (i = 0; i < len; i++) {
        if (s->rx_count == IMXRT_LPUART_RXQ) {
            s->stat |= STAT_OR;
            break;
        }
        s->rxq[(s->rx_head + s->rx_count++) % IMXRT_LPUART_RXQ] = buf[i];
    }
    SP404_TRACE("lpuart", "%s rx %d bytes, %u queued", s->name ? s->name : "?",
                len, s->rx_count);
    lpuart_update(s);
}

static int lpuart_can_receive(void *opaque)
{
    IMXRTLPUART *s = opaque;

    return (s->ctrl & CTRL_RE) ? IMXRT_LPUART_RXQ - s->rx_count : 0;
}

static void lpuart_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    imxrt_lpuart_receive(opaque, buf, size);
}

static void lpuart_soft_reset(IMXRTLPUART *s)
{
    s->baud = 0x0f000004;
    s->stat = 0x00c00000;
    s->ctrl = s->match = s->modir = s->pincfg = 0;
    s->fifo = FIFO_SIZES;
    s->water = 0;
    s->rx_head = s->rx_count = 0;
}

static uint64_t lpuart_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTLPUART *s = opaque;
    uint32_t v = 0;

    switch (offset & ~3) {
    case R_VERID: v = 0x04010003; break;
    case R_PARAM: v = 0x00000202; break;
    case R_GLOBAL: v = s->global; break;
    case R_PINCFG: v = s->pincfg; break;
    case R_BAUD: v = s->baud; break;
    case R_STAT: v = s->stat; break;
    case R_CTRL: v = s->ctrl; break;
    case R_DATA:
        if (s->rx_count) {
            v = s->rxq[s->rx_head];
            s->rx_head = (s->rx_head + 1) % IMXRT_LPUART_RXQ;
            s->rx_count--;
            if (!s->rx_count) {
                s->stat |= STAT_IDLE;   /* the burst is over */
                qemu_chr_fe_accept_input(&s->chr);
            }
        } else {
            v = 1u << 12;               /* RXEMPT */
        }
        lpuart_update(s);
        break;
    case R_MATCH: v = s->match; break;
    case R_MODIR: v = s->modir; break;
    case R_FIFO:
        v = s->fifo | FIFO_TXEMPT | (s->rx_count ? 0 : FIFO_RXEMPT);
        break;
    case R_WATER:
        v = (s->water & 0x00030003) | (MIN(s->rx_count, 4) << 24);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-lpuart: read 0x%" HWADDR_PRIx
                      "\n", offset);
    }
    return extract32(v, (offset & 3) * 8, size * 8);
}

static void lpuart_write(void *opaque, hwaddr offset, uint64_t val,
                         unsigned size)
{
    IMXRTLPUART *s = opaque;
    uint32_t v = (uint32_t)val << ((offset & 3) * 8);

    switch (offset & ~3) {
    case R_GLOBAL:
        s->global = v & GLOBAL_RST;
        if (v & GLOBAL_RST) {
            lpuart_soft_reset(s);
        }
        break;
    case R_PINCFG: s->pincfg = v; break;
    case R_BAUD: s->baud = v; break;
    case R_STAT:
        s->stat &= ~(v & STAT_W1C);
        break;
    case R_CTRL: s->ctrl = v; break;
    case R_DATA:
        if (s->ctrl & CTRL_TE) {
            uint8_t b = v;

            SP404_TRACE("lpuart", "%s tx %02x", s->name ? s->name : "?", b);
            if (s->tx) {
                s->tx(s->tx_opaque, b);
            }
            qemu_chr_fe_write_all(&s->chr, &b, 1);
        }
        break;
    case R_MATCH: s->match = v; break;
    case R_MODIR: s->modir = v; break;
    case R_FIFO:
        if (v & FIFO_RXFLUSH) {
            s->rx_head = s->rx_count = 0;
        }
        s->fifo = (v & 0x0000ff88) | FIFO_SIZES;
        s->fifo &= ~(FIFO_RXFLUSH | FIFO_TXFLUSH);
        break;
    case R_WATER: s->water = v; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-lpuart: write 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    lpuart_update(s);
}

static const MemoryRegionOps lpuart_ops = {
    .read = lpuart_read,
    .write = lpuart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpuart_reset(DeviceState *dev)
{
    IMXRTLPUART *s = IMXRT_LPUART(dev);

    s->global = 0;
    lpuart_soft_reset(s);
    lpuart_update(s);
}

static void lpuart_realize(DeviceState *dev, Error **errp)
{
    IMXRTLPUART *s = IMXRT_LPUART(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &lpuart_ops, s,
                          "imxrt.lpuart", 0x4000);
    /*
     * The eDMA writes back into this device from inside its own register
     * handlers (a request raised by a register write is serviced at once).
     */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_out_named(dev, &s->tx_dreq, "tx-dreq", 1);
    qdev_init_gpio_out_named(dev, &s->rx_dreq, "rx-dreq", 1);
    qemu_chr_fe_set_handlers(&s->chr, lpuart_can_receive, lpuart_chr_receive,
                             NULL, NULL, s, NULL, true);
}

static const Property lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", IMXRTLPUART, chr),
    DEFINE_PROP_STRING("name", IMXRTLPUART, name),
};

static void lpuart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpuart_realize;
    device_class_set_legacy_reset(dc, lpuart_reset);
    device_class_set_props(dc, lpuart_properties);
}

static const TypeInfo lpuart_info = {
    .name = TYPE_IMXRT_LPUART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTLPUART),
    .class_init = lpuart_class_init,
};

static void lpuart_register_types(void)
{
    type_register_static(&lpuart_info);
}

type_init(lpuart_register_types)
