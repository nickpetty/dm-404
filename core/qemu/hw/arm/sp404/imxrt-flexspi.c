/*
 * i.MX RT FlexSPI controller with a QSPI NOR flash behind it.
 *
 * Reads through the AHB window go straight to the flash's memory region;
 * this models the register side: IP commands, which run a LUT sequence
 * against the flash. Each sequence is interpreted as the one serial NOR
 * transaction it encodes: the command byte decides what happens, the
 * address comes from IPCR0 and data moves through the IP RX/TX FIFOs the
 * way NXP's SDK drives them (a watermark's worth per IPRXWA/IPTXWE pop).
 *
 * Program and erase change the flash region through address_space_write_rom
 * so translated code from flash is invalidated. The flash can be backed by
 * a file (the machine's "flash" option) so settings survive a restart.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "hw/arm/sp404/sp404.h"

#define R_MCR0          (0x000 / 4)
#define   MCR0_SWRESET  (1u << 0)
#define R_INTEN         (0x010 / 4)
#define R_INTR          (0x014 / 4)
#define   INTR_IPCMDDONE (1u << 0)
#define   INTR_IPCMDGE  (1u << 1)
#define   INTR_IPCMDERR (1u << 3)
#define   INTR_IPRXWA   (1u << 5)
#define   INTR_IPTXWE   (1u << 6)
#define R_IPCR0         (0x0a0 / 4)
#define R_IPCR1         (0x0a4 / 4)
#define R_IPCMD         (0x0b0 / 4)
#define R_IPRXFCR       (0x0b8 / 4)
#define R_IPTXFCR       (0x0bc / 4)
#define R_STS0          (0x0e0 / 4)
#define R_STS2          (0x0e8 / 4)
#define R_IPRXFSTS      (0x0f0 / 4)
#define R_IPTXFSTS      (0x0f4 / 4)
#define R_RFDR          (0x100 / 4)
#define R_TFDR          (0x180 / 4)
#define R_LUT           (0x200 / 4)
#define FLEXSPI_REGS    (0x300 / 4)

#define FIFO_BYTES      128

/* LUT opcodes, SDR and DDR alike once the DDR bit (0x20) is dropped. */
enum {
    OP_STOP = 0x00, OP_CMD = 0x01, OP_RADDR = 0x02, OP_CADDR = 0x03,
    OP_MODE1 = 0x04, OP_MODE2 = 0x05, OP_MODE4 = 0x06, OP_MODE8 = 0x07,
    OP_WRITE = 0x08, OP_READ = 0x09, OP_LEARN = 0x0a, OP_DATSZ = 0x0b,
    OP_DUMMY = 0x0c, OP_DUMMY_RWDS = 0x0d, OP_JMP_ON_CS = 0x1f,
};

static void flexspi_update_irq(IMXRTFlexSPI *s)
{
    uint32_t intr = s->regs[R_INTR] & ~(INTR_IPRXWA | INTR_IPTXWE);

    if (s->rx_len > s->rx_pos) {
        intr |= INTR_IPRXWA;
    }
    if (s->tx_expected > s->tx_len) {
        intr |= INTR_IPTXWE;
    }
    s->regs[R_INTR] = intr;
    qemu_set_irq(s->irq, !!(intr & s->regs[R_INTEN]));
}

static unsigned flexspi_rx_watermark(IMXRTFlexSPI *s)
{
    return (((s->regs[R_IPRXFCR] >> 2) & 0x1f) + 1) * 8;
}

static unsigned flexspi_tx_watermark(IMXRTFlexSPI *s)
{
    return (((s->regs[R_IPTXFCR] >> 2) & 0x1f) + 1) * 8;
}

static uint8_t *flexspi_flash(IMXRTFlexSPI *s)
{
    return memory_region_get_ram_ptr(s->flash);
}

static void flexspi_flash_write(IMXRTFlexSPI *s, uint32_t addr,
                                const uint8_t *buf, uint32_t len)
{
    address_space_write_rom(&address_space_memory, s->flash_base + addr,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    if (s->flash_dirty) {
        s->flash_dirty(s->flash_dirty_opaque, addr, len);
    }
}

/* Carry out one NOR transaction. Returns false for an unknown command. */
static bool flexspi_nor(IMXRTFlexSPI *s, uint8_t cmd, uint32_t addr,
                        const uint8_t *wdata, uint32_t wlen,
                        uint8_t *rdata, uint32_t rlen)
{
    uint64_t size = memory_region_size(s->flash);
    uint8_t *flash = flexspi_flash(s);
    uint32_t i, erase = 0;

    addr &= size - 1;
    switch (cmd) {
    case 0x03: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
    case 0x13: case 0x0c: case 0x3c: case 0x6c: case 0xbc: case 0xec:
        for (i = 0; i < rlen; i++) {
            rdata[i] = flash[(addr + i) & (size - 1)];
        }
        return true;
    case 0x05:                  /* read status 1: never busy */
        memset(rdata, s->wel ? 0x02 : 0x00, rlen);
        return true;
    case 0x35:                  /* read status 2 */
        memset(rdata, s->sr2, rlen);
        return true;
    case 0x15:                  /* read status 3 */
        memset(rdata, 0, rlen);
        return true;
    case 0x01:                  /* write status */
        if (wlen > 1) {
            s->sr2 = wdata[1];
        }
        s->wel = false;
        return true;
    case 0x31:                  /* write status 2 */
        if (wlen) {
            s->sr2 = wdata[0];
        }
        s->wel = false;
        return true;
    case 0x9f:                  /* JEDEC ID */
        for (i = 0; i < rlen; i++) {
            rdata[i] = i < 3 ? s->jedec_id >> (16 - 8 * i) : 0;
        }
        return true;
    case 0x06:
        s->wel = true;
        return true;
    case 0x04:
        s->wel = false;
        return true;
    case 0x02: case 0x32: case 0x12: case 0x34: {
        /* Page program wraps within the 256-byte page, and only clears bits. */
        uint8_t page[256];
        uint32_t base = addr & ~0xffu;

        if (!s->wel) {
            qemu_log_mask(LOG_GUEST_ERROR, "flexspi: program without WREN\n");
            return true;
        }
        memcpy(page, flash + base, sizeof(page));
        for (i = 0; i < wlen; i++) {
            page[(addr + i) & 0xff] &= wdata[i];
        }
        flexspi_flash_write(s, base, page, sizeof(page));
        s->wel = false;
        return true;
    }
    case 0x20: case 0x21: erase = 4 * KiB; break;
    case 0x52: case 0x5c: erase = 32 * KiB; break;
    case 0xd8: case 0xdc: erase = 64 * KiB; break;
    case 0x60: case 0xc7: erase = size; break;
    case 0x66: case 0x99: case 0xb7: case 0xe9: case 0xab: case 0xb9:
    case 0x50: case 0x38: case 0xff:
        return true;            /* resets, modes, power: nothing to model */
    case 0x5a:                  /* SFDP: none */
        memset(rdata, 0xff, rlen);
        return true;
    default:
        return false;
    }
    if (!s->wel) {
        qemu_log_mask(LOG_GUEST_ERROR, "flexspi: erase without WREN\n");
        return true;
    }
    {
        g_autofree uint8_t *ff = g_malloc(erase);

        memset(ff, 0xff, erase);
        flexspi_flash_write(s, addr & ~(erase - 1), ff, erase);
    }
    s->wel = false;
    return true;
}

/* Walk sequence 'seq' (and the next ISEQNUM ones) to find what it does. */
static void flexspi_run(IMXRTFlexSPI *s)
{
    uint32_t ipcr1 = s->regs[R_IPCR1];
    unsigned seq = (ipcr1 >> 16) & 0xf;
    unsigned nseq = ((ipcr1 >> 24) & 7) + 1;
    uint32_t datsz = ipcr1 & 0xffff;
    int cmd = -1;
    bool read = false, write = false;
    unsigned n, i;

    for (n = 0; n < nseq && seq + n < 16; n++) {
        for (i = 0; i < 8; i++) {
            uint32_t w = s->regs[R_LUT + (seq + n) * 4 + i / 2];
            uint16_t ins = i & 1 ? w >> 16 : w & 0xffff;
            uint8_t op = (ins >> 10) & 0x1f, operand = ins & 0xff;

            if ((ins >> 10) == 0) {
                goto done;
            }
            if ((ins >> 10) == OP_JMP_ON_CS) {
                goto done;
            }
            switch (op) {
            case OP_CMD:
                if (cmd < 0) {
                    cmd = operand;
                }
                break;
            case OP_READ:
                read = true;
                break;
            case OP_WRITE:
                write = true;
                break;
            }
        }
    }
done:
    s->cur_cmd = cmd;
    s->rx_len = s->rx_pos = 0;
    s->tx_len = s->tx_expected = 0;
    if (cmd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "flexspi: sequence %u has no command\n",
                      seq);
        s->regs[R_INTR] |= INTR_IPCMDDONE;
        return;
    }
    if (write && datsz) {
        /* Data arrives through the TX FIFO after the trigger. */
        s->tx_expected = MIN(datsz, sizeof(s->txbuf));
        return;
    }
    if (read) {
        s->rx_len = MIN(datsz ? datsz : 1, sizeof(s->rxbuf));
    }
    if (!flexspi_nor(s, cmd, s->regs[R_IPCR0], NULL, 0, s->rxbuf, s->rx_len)) {
        qemu_log_mask(LOG_UNIMP, "flexspi: NOR command 0x%02x\n", cmd);
    }
    s->regs[R_INTR] |= INTR_IPCMDDONE;
}

static void flexspi_tx_push(IMXRTFlexSPI *s)
{
    unsigned wm = flexspi_tx_watermark(s);
    unsigned take = MIN(wm, s->tx_expected - s->tx_len);

    memcpy(s->txbuf + s->tx_len, s->tfdr, take);
    s->tx_len += take;
    if (s->tx_len >= s->tx_expected) {
        if (!flexspi_nor(s, s->cur_cmd, s->regs[R_IPCR0], s->txbuf, s->tx_len,
                         NULL, 0)) {
            qemu_log_mask(LOG_UNIMP, "flexspi: NOR command 0x%02x\n",
                          s->cur_cmd);
        }
        s->tx_expected = s->tx_len = 0;
        s->regs[R_INTR] |= INTR_IPCMDDONE;
    }
}

static uint64_t flexspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTFlexSPI *s = opaque;
    unsigned idx = offset / 4;

    switch (idx) {
    case R_STS0:
        return 0x3;             /* SEQIDLE | ARBIDLE */
    case R_STS2:
        return 0x00030003;      /* both ports' DLLs locked */
    case R_IPRXFSTS:
        return DIV_ROUND_UP(s->rx_len - s->rx_pos, 8);
    case R_IPTXFSTS:
        return 0;
    }
    if (idx >= R_RFDR && idx < R_RFDR + 32) {
        unsigned at = s->rx_pos + (idx - R_RFDR) * 4;
        uint32_t v = 0;
        unsigned i;

        for (i = 0; i < 4; i++) {
            if (at + i < s->rx_len) {
                v |= s->rxbuf[at + i] << (8 * i);
            }
        }
        return v;
    }
    if (idx >= R_TFDR && idx < R_TFDR + 32) {
        return 0;
    }
    return s->regs[idx];
}

static void flexspi_write(void *opaque, hwaddr offset, uint64_t val,
                          unsigned size)
{
    IMXRTFlexSPI *s = opaque;
    unsigned idx = offset / 4;

    if (idx >= R_TFDR && idx < R_TFDR + 32) {
        stl_le_p(s->tfdr + (idx - R_TFDR) * 4, val);
        return;
    }
    if (idx >= R_RFDR && idx < R_RFDR + 32) {
        return;
    }
    switch (idx) {
    case R_MCR0:
        s->regs[idx] = val & ~MCR0_SWRESET;     /* reset completes at once */
        break;
    case R_INTR:
        if (val & INTR_IPRXWA) {
            s->rx_pos = MIN(s->rx_pos + flexspi_rx_watermark(s), s->rx_len);
        }
        if ((val & INTR_IPTXWE) && s->tx_expected) {
            flexspi_tx_push(s);
        }
        s->regs[idx] &= ~val;
        break;
    case R_IPCMD:
        if (val & 1) {
            flexspi_run(s);
        }
        break;
    case R_IPRXFCR:
        if (val & 1) {
            s->rx_len = s->rx_pos = 0;
        }
        s->regs[idx] = val & ~1u;
        break;
    case R_IPTXFCR:
        if (val & 1) {
            s->tx_len = 0;
        }
        s->regs[idx] = val & ~1u;
        break;
    default:
        s->regs[idx] = val;
        break;
    }
    flexspi_update_irq(s);
}

static const MemoryRegionOps flexspi_ops = {
    .read = flexspi_read,
    .write = flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void flexspi_reset(DeviceState *dev)
{
    IMXRTFlexSPI *s = IMXRT_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->rx_len = s->rx_pos = s->tx_len = s->tx_expected = 0;
    s->wel = false;
    s->sr2 = 0x02;              /* QE set: quad reads work from the start */
}

static void flexspi_realize(DeviceState *dev, Error **errp)
{
    IMXRTFlexSPI *s = IMXRT_FLEXSPI(dev);

    if (!s->flash) {
        error_setg(errp, "imxrt-flexspi: no flash region");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &flexspi_ops, s,
                          "imxrt.flexspi", FLEXSPI_REGS * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const Property flexspi_properties[] = {
    DEFINE_PROP_LINK("flash", IMXRTFlexSPI, flash, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("flash-base", IMXRTFlexSPI, flash_base, 0x60000000),
    DEFINE_PROP_UINT32("jedec-id", IMXRTFlexSPI, jedec_id, 0xef4016),
};

static void flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flexspi_realize;
    device_class_set_legacy_reset(dc, flexspi_reset);
    device_class_set_props(dc, flexspi_properties);
}

static const TypeInfo flexspi_info = {
    .name = TYPE_IMXRT_FLEXSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTFlexSPI),
    .class_init = flexspi_class_init,
};

static void flexspi_register_types(void)
{
    type_register_static(&flexspi_info);
}

type_init(flexspi_register_types)
