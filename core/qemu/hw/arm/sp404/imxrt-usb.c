/*
 * i.MX RT USB OTG controller (ChipIdea / EHCI-style), device mode only: what
 * the SP-404MKII's USB port is to its firmware (NXP's USB device stack,
 * USB1 at 0x402e0000).
 *
 * The firmware keeps its endpoints as queue heads (dQH, 64 bytes, two per
 * endpoint: OUT then IN) at ENDPTLISTADDR, each pointing at a chain of
 * transfer descriptors (dTD, 32 bytes: next, token, five buffer pages).
 * Priming an endpoint (ENDPTPRIME) makes its next dTD current; a dTD is
 * retired (active cleared, remaining bytes written back, ENDPTCOMPLETE if
 * IOC) when its bytes have moved or a short packet ends it; the chain is
 * followed by re-reading next, so dTDs appended while primed are taken.
 * SETUP packets land in the ep0 OUT dQH (+0x28) with ENDPTSETUPSTAT.
 *
 * The host side is not a bus but an API (imxrt_usb_host_*): a transfer on
 * an endpoint, completed by callback once the firmware has fed or drained
 * it; control transfers run their setup, data and status stages here. The
 * USB/IP server (sp404-usbip.c) drives it. Attach and bus reset are modelled
 * as the port and USBSTS bits the driver looks at; the device is always
 * high speed. Isochronous endpoints are not modelled yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/arm/sp404/sp404.h"

#define R_ID            0x000
#define R_HWGENERAL     0x004
#define R_HWHOST        0x008
#define R_HWDEVICE      0x00c
#define R_HWTXBUF       0x010
#define R_HWRXBUF       0x014
#define R_CAPLENGTH     0x100
#define R_HCSPARAMS     0x104
#define R_HCCPARAMS     0x108
#define R_DCIVERSION    0x120
#define R_DCCPARAMS     0x124
#define R_USBCMD        0x140
#define   CMD_RS        (1u << 0)
#define   CMD_RST       (1u << 1)
#define R_USBSTS        0x144
#define   STS_UI        (1u << 0)
#define   STS_UEI       (1u << 1)
#define   STS_PCI       (1u << 2)
#define   STS_URI       (1u << 6)
#define   STS_SRI       (1u << 7)
#define   STS_SLI       (1u << 8)
#define   STS_HCH       (1u << 12)
#define R_USBINTR       0x148
#define R_FRINDEX       0x14c
#define R_DEVICEADDR    0x154
#define R_ENDPTLISTADDR 0x158
#define R_BURSTSIZE     0x160
#define R_TXFILLTUNING  0x164
#define R_ENDPTNAK      0x178
#define R_ENDPTNAKEN    0x17c
#define R_CONFIGFLAG    0x180
#define R_PORTSC1       0x184
#define   PORT_CCS      (1u << 0)
#define   PORT_PE       (1u << 2)
#define   PORT_SUSP     (1u << 7)
#define   PORT_PR       (1u << 8)
#define   PORT_PP       (1u << 12)
#define   PORT_PSPD_HS  (2u << 26)
#define R_OTGSC         0x1a4
#define   OTG_ID        (1u << 8)
#define   OTG_AVV       (1u << 9)
#define   OTG_ASV       (1u << 10)
#define   OTG_BSV       (1u << 11)
#define   OTG_STATUS_W1C 0x007f0000u
#define R_USBMODE       0x1a8
#define R_ENDPTSETUPSTAT 0x1ac
#define R_ENDPTPRIME    0x1b0
#define R_ENDPTFLUSH    0x1b4
#define R_ENDPTSTAT     0x1b8
#define R_ENDPTCOMPLETE 0x1bc
#define R_ENDPTCTRL0    0x1c0
#define   EPCTRL_RXS    (1u << 0)
#define   EPCTRL_TXS    (1u << 16)

#define DTD_T           1u              /* next pointer: terminate */
#define TOK_ACTIVE      0x80u
#define TOK_IOC         (1u << 15)

/* The endpoint bit for ENDPTPRIME/STAT/COMPLETE: OUT n at n, IN n at 16 + n. */
static uint32_t ep_bit(int ep, bool in)
{
    return 1u << (ep + (in ? 16 : 0));
}

static hwaddr qh_addr(IMXRTUSB *s, int ep, bool in)
{
    return (s->listaddr & ~0x7ffu) + (ep * 2 + in) * 64;
}

static uint32_t mem_ld(hwaddr a)
{
    uint32_t v = 0;

    dma_memory_read(&address_space_memory, a, &v, 4, MEMTXATTRS_UNSPECIFIED);
    return le32_to_cpu(v);
}

static void mem_st(hwaddr a, uint32_t v)
{
    v = cpu_to_le32(v);
    dma_memory_write(&address_space_memory, a, &v, 4, MEMTXATTRS_UNSPECIFIED);
}

static void usb_update_irq(IMXRTUSB *s)
{
    /* USBSTS against USBINTR, and OTGSC's status bits against its enables
     * (the firmware watches B-session valid: the cable). */
    bool otg = (s->otgsc & (s->otgsc >> 8) & OTG_STATUS_W1C) != 0;

    qemu_set_irq(s->irq, (s->sts & s->intr & 0x030107ffu) != 0 || otg);
}

static IMXRTUSBEndpoint *endpoint(IMXRTUSB *s, int ep, bool in)
{
    return &s->eps[ep][in];
}

static uint32_t maxpkt(IMXRTUSB *s, int ep, bool in)
{
    uint32_t m = (mem_ld(qh_addr(s, ep, in)) >> 16) & 0x7ff;

    return m ? m : 64;
}

/* The byte at offset off of the current dTD's buffer. */
static hwaddr dtd_buf(IMXRTUSBEndpoint *e, uint32_t off)
{
    uint32_t pos = (e->page[0] & 0xfff) + off;
    unsigned page = pos >> 12;

    if (page > 4) {
        page = 4;
    }
    return (e->page[page] & ~0xfffu) + (pos & 0xfff);
}

/* Make the dTD at addr current on the endpoint, if it is active. */
static bool ep_load(IMXRTUSB *s, int ep, bool in, uint32_t addr)
{
    IMXRTUSBEndpoint *e = endpoint(s, ep, in);
    uint32_t token;

    if (addr & DTD_T) {
        return false;
    }
    addr &= ~0x1fu;
    token = mem_ld(addr + 4);
    if (!(token & TOK_ACTIVE)) {
        return false;
    }
    e->dtd = addr;
    e->total = (token >> 16) & 0x7fff;
    e->off = 0;
    e->ioc = token & TOK_IOC;
    for (int i = 0; i < 5; i++) {
        e->page[i] = mem_ld(addr + 8 + 4 * i);
    }
    e->primed = true;
    s->stat |= ep_bit(ep, in);
    /* The dQH's current pointer and overlay follow. */
    mem_st(qh_addr(s, ep, in) + 4, addr);
    mem_st(qh_addr(s, ep, in) + 8, mem_ld(addr));
    mem_st(qh_addr(s, ep, in) + 12, token);
    return true;
}

/* Retire the current dTD and move on down the chain. */
static void ep_retire(IMXRTUSB *s, int ep, bool in)
{
    IMXRTUSBEndpoint *e = endpoint(s, ep, in);
    uint32_t token = mem_ld(e->dtd + 4), next;

    token = (token & ~(TOK_ACTIVE | (0x7fffu << 16))) |
            ((e->total - e->off) << 16);
    mem_st(e->dtd + 4, token);
    mem_st(qh_addr(s, ep, in) + 12, token);
    if (e->ioc) {
        s->complete |= ep_bit(ep, in);
        s->sts |= STS_UI;
    }
    next = mem_ld(e->dtd);
    mem_st(qh_addr(s, ep, in) + 8, next);
    e->primed = false;
    s->stat &= ~ep_bit(ep, in);
    ep_load(s, ep, in, next);
    usb_update_irq(s);
}

static bool ep_stalled(IMXRTUSB *s, int ep, bool in)
{
    return s->epctrl[ep] & (in ? EPCTRL_TXS : EPCTRL_RXS);
}

/*
 * IN: take up to max bytes the firmware has queued. *end says whether the
 * host's transfer ends here (a short or empty packet, or a dTD ending on a
 * packet boundary). Returns the bytes taken; 0 with *end false: nothing yet.
 */
static uint32_t ep_take(IMXRTUSB *s, int ep, uint8_t *dst, uint32_t max,
                        bool *end)
{
    IMXRTUSBEndpoint *e = endpoint(s, ep, true);
    uint32_t mp, n;

    *end = false;
    if (!e->primed) {
        return 0;
    }
    mp = maxpkt(s, ep, true);
    n = MIN(max, e->total - e->off);
    if (n) {
        /* Pages can end within the transfer: read page by page. */
        for (uint32_t done = 0; done < n;) {
            hwaddr a = dtd_buf(e, e->off + done);
            uint32_t chunk = MIN(n - done, 0x1000 - (a & 0xfff));

            dma_memory_read(&address_space_memory, a, dst + done, chunk,
                            MEMTXATTRS_UNSPECIFIED);
            done += chunk;
        }
    }
    e->off += n;
    if (e->off == e->total) {
        /* The dTD is done; a short (or empty) last packet ends the transfer. */
        *end = e->total == 0 || (e->total % mp) != 0;
        ep_retire(s, ep, true);
    }
    return n;
}

/*
 * OUT: give the firmware n bytes; short means they end the host's transfer
 * (the last packet was short or empty). Returns the bytes taken (fewer when
 * the firmware has no more dTDs queued yet).
 */
static uint32_t ep_give(IMXRTUSB *s, int ep, const uint8_t *src, uint32_t n,
                        bool short_end)
{
    uint32_t done = 0;

    while (true) {
        IMXRTUSBEndpoint *e = endpoint(s, ep, false);
        uint32_t k;

        if (!e->primed) {
            return done;
        }
        k = MIN(n - done, e->total - e->off);
        for (uint32_t w = 0; w < k;) {
            hwaddr a = dtd_buf(e, e->off + w);
            uint32_t chunk = MIN(k - w, 0x1000 - (a & 0xfff));

            dma_memory_write(&address_space_memory, a, src + done + w, chunk,
                             MEMTXATTRS_UNSPECIFIED);
            w += chunk;
        }
        e->off += k;
        done += k;
        if (e->off == e->total) {
            ep_retire(s, ep, false);
            if (done == n && !short_end) {
                return done;
            }
            if (done == n) {
                return done;
            }
            continue;
        }
        if (done == n) {
            if (short_end) {
                ep_retire(s, ep, false);
            }
            return done;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The host's transfers. */

static void xfer_finish(IMXRTUSB *s, IMXRTUSBXfer *x, int status)
{
    QTAILQ_REMOVE(&s->xfers, x, link);
    x->status = status;
    if (x->complete) {
        x->complete(x->opaque, x);
    }
}

/* Move whatever can move for one transfer; true when it is finished. */
static bool xfer_pump(IMXRTUSB *s, IMXRTUSBXfer *x)
{
    bool end = false;

    if (x->ep == 0 && x->control) {
        bool in = x->setup[0] & 0x80;
        uint16_t wlen = x->setup[6] | (x->setup[7] << 8);

        switch (x->stage) {
        case 0:                 /* SETUP */
            for (int i = 0; i < 8; i++) {
                dma_memory_write(&address_space_memory,
                                 qh_addr(s, 0, false) + 0x28 + i, &x->setup[i],
                                 1, MEMTXATTRS_UNSPECIFIED);
            }
            /* A SETUP cancels what was queued on ep0 and clears its stall. */
            s->eps[0][0].primed = s->eps[0][1].primed = false;
            s->stat &= ~(ep_bit(0, false) | ep_bit(0, true));
            s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
            s->setupstat |= 1;
            s->sts |= STS_UI;
            usb_update_irq(s);
            x->stage = wlen ? 1 : 2;
            x->len = MIN(x->len, wlen);
            return false;
        case 1:                 /* DATA */
            if (ep_stalled(s, 0, in)) {
                s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
                xfer_finish(s, x, -EPIPE);
                return true;
            }
            if (s->setupstat & 1) {
                return false;   /* the firmware has not taken the SETUP yet */
            }
            if (in) {
                x->done += ep_take(s, 0, x->buf + x->done, x->len - x->done,
                                   &end);
                if (!end && x->done < x->len) {
                    return false;
                }
            } else {
                x->done += ep_give(s, 0, x->buf + x->done, x->len - x->done,
                                   true);
                if (x->done < x->len) {
                    return false;
                }
            }
            x->stage = 2;
            /* fall through */
        case 2:                 /* STATUS: the other way, empty */
            if (ep_stalled(s, 0, !in || !wlen)) {
                s->epctrl[0] &= ~(EPCTRL_RXS | EPCTRL_TXS);
                xfer_finish(s, x, -EPIPE);
                return true;
            }
            if (s->setupstat & 1) {
                return false;
            }
            if (in && wlen) {
                if (!endpoint(s, 0, false)->primed) {
                    return false;
                }
                ep_give(s, 0, NULL, 0, true);
            } else {
                uint8_t dummy[1];

                if (!endpoint(s, 0, true)->primed) {
                    return false;
                }
                ep_take(s, 0, dummy, 0, &end);
            }
            xfer_finish(s, x, 0);
            return true;
        }
        return false;
    }

    if (ep_stalled(s, x->ep, x->in)) {
        xfer_finish(s, x, -EPIPE);
        return true;
    }
    if (x->in) {
        uint32_t mp = maxpkt(s, x->ep, true);
        uint32_t room = x->len - x->done;

        /* Whole packets only, unless the host's buffer ends mid-packet. */
        x->done += ep_take(s, x->ep, x->buf + x->done, room, &end);
        if (end || x->done == x->len || (x->done && (x->done % mp))) {
            xfer_finish(s, x, 0);
            return true;
        }
        return false;
    }
    x->done += ep_give(s, x->ep, x->buf + x->done, x->len - x->done,
                       x->len % maxpkt(s, x->ep, false) || !x->len);
    if (x->done == x->len) {
        xfer_finish(s, x, 0);
        return true;
    }
    return false;
}

/* Run every transfer that can move, oldest first per endpoint. */
static void usb_pump(IMXRTUSB *s)
{
    bool moved = true;

    if (s->pumping) {
        return;
    }
    s->pumping = true;
    while (moved) {
        IMXRTUSBXfer *x, *next;
        uint32_t busy = 0;

        moved = false;
        QTAILQ_FOREACH_SAFE(x, &s->xfers, link, next) {
            uint32_t b = ep_bit(x->ep, x->in || x->control);

            if (busy & b) {
                continue;       /* an older one on this endpoint goes first */
            }
            busy |= b;
            if (xfer_pump(s, x)) {
                moved = true;
                break;          /* the list changed */
            }
        }
    }
    s->pumping = false;
}

void imxrt_usb_host_submit(IMXRTUSB *s, IMXRTUSBXfer *x)
{
    x->done = 0;
    x->stage = 0;
    x->status = 0;
    QTAILQ_INSERT_TAIL(&s->xfers, x, link);
    usb_pump(s);
}

bool imxrt_usb_host_cancel(IMXRTUSB *s, IMXRTUSBXfer *x)
{
    IMXRTUSBXfer *y;

    QTAILQ_FOREACH(y, &s->xfers, link) {
        if (y == x) {
            QTAILQ_REMOVE(&s->xfers, x, link);
            return true;
        }
    }
    return false;
}

static void usb_cancel_all(IMXRTUSB *s)
{
    IMXRTUSBXfer *x;

    while ((x = QTAILQ_FIRST(&s->xfers))) {
        xfer_finish(s, x, -ESHUTDOWN);
    }
}

bool imxrt_usb_host_running(IMXRTUSB *s)
{
    return (s->cmd & CMD_RS) && s->listaddr;
}

void imxrt_usb_host_attach(IMXRTUSB *s, bool on)
{
    s->attached = on;
    if (on) {
        s->portsc = PORT_CCS | PORT_PE | PORT_PP | PORT_PSPD_HS;
        s->otgsc |= OTG_BSV | OTG_ASV | OTG_AVV | (1u << 19);
    } else {
        usb_cancel_all(s);
        s->portsc = PORT_PP;
        s->otgsc = (s->otgsc & ~(OTG_BSV | OTG_ASV | OTG_AVV)) | (1u << 19);
    }
    s->sts |= STS_PCI;
    usb_update_irq(s);
}

void imxrt_usb_host_reset(IMXRTUSB *s)
{
    /* A bus reset: what was in flight is gone; the device is address 0. */
    usb_cancel_all(s);
    s->devaddr = 0;
    for (int ep = 0; ep < IMXRT_USB_EPS; ep++) {
        s->eps[ep][0].primed = s->eps[ep][1].primed = false;
    }
    s->stat = 0;
    s->sts |= STS_URI | STS_PCI;
    usb_update_irq(s);
}

/* ---------------------------------------------------------------------- */

static uint64_t usb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTUSB *s = opaque;
    uint32_t v = 0;

    switch (offset & ~3) {
    case R_ID:          v = 0xe4a1fa05; break;
    case R_HWGENERAL:   v = 0x35; break;
    case R_HWHOST:      v = 0x10020001; break;
    case R_HWDEVICE:    v = 0x11; break;            /* device, 8 endpoints */
    case R_HWTXBUF:     v = 0x80080b08; break;
    case R_HWRXBUF:     v = 0x0808; break;
    case R_CAPLENGTH:   v = 0x01000040; break;
    case R_HCSPARAMS:   v = 0x10011; break;
    case R_HCCPARAMS:   v = 0x6; break;
    case R_DCIVERSION:  v = 0x1; break;
    case R_DCCPARAMS:   v = 0x180 | IMXRT_USB_EPS; break;
    case R_USBCMD:      v = s->cmd & ~CMD_RST; break;
    case R_USBSTS:      v = s->sts | ((s->cmd & CMD_RS) ? 0 : STS_HCH); break;
    case R_USBINTR:     v = s->intr; break;
    case R_FRINDEX:
        v = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 125000) & 0x3fff;
        break;
    case R_DEVICEADDR:  v = s->devaddr; break;
    case R_ENDPTLISTADDR: v = s->listaddr; break;
    case R_BURSTSIZE:   v = s->burstsize; break;
    case R_TXFILLTUNING: v = s->txfill; break;
    case R_ENDPTNAK:    v = 0; break;
    case R_ENDPTNAKEN:  v = s->naken; break;
    case R_CONFIGFLAG:  v = 1; break;
    case R_PORTSC1:     v = s->portsc; break;
    case R_OTGSC:       v = s->otgsc | OTG_ID; break;
    case R_USBMODE:     v = s->mode; break;
    case R_ENDPTSETUPSTAT: v = s->setupstat; break;
    case R_ENDPTPRIME:  v = 0; break;
    case R_ENDPTFLUSH:  v = 0; break;
    case R_ENDPTSTAT:   v = s->stat; break;
    case R_ENDPTCOMPLETE: v = s->complete; break;
    default:
        if (offset >= R_ENDPTCTRL0 && offset < R_ENDPTCTRL0 + 4 * IMXRT_USB_EPS) {
            v = s->epctrl[(offset - R_ENDPTCTRL0) / 4];
            if (offset == R_ENDPTCTRL0) {
                v |= (1u << 23) | (1u << 7);    /* ep0 is always enabled */
            }
        } else {
            v = s->other[(offset & 0x1ff) / 4];
        }
    }
    SP404_TRACE("usb", "read  %03" HWADDR_PRIx " = %08x", offset, v);
    return v >> ((offset & 3) * 8);
}

static void usb_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMXRTUSB *s = opaque;
    uint32_t v = value;

    SP404_TRACE("usb", "write %03" HWADDR_PRIx " = %08x", offset, v);
    switch (offset) {
    case R_USBCMD:
        if (v & CMD_RST) {
            /* Controller reset: back to idle, done at once. */
            usb_cancel_all(s);
            s->sts = s->intr = s->setupstat = s->stat = s->complete = 0;
            s->devaddr = s->listaddr = 0;
            memset(s->epctrl, 0, sizeof(s->epctrl));
            memset(s->eps, 0, sizeof(s->eps));
            v &= ~CMD_RST;
        }
        s->cmd = v;
        if (s->cmd & CMD_RS && s->attached && !s->announced) {
            /* Pull-up on with the cable in: the host sees us and resets. */
            s->announced = true;
            imxrt_usb_host_reset(s);
        }
        if (!(s->cmd & CMD_RS)) {
            s->announced = false;
        }
        if (s->run_changed) {
            s->run_changed(s->run_opaque);
        }
        break;
    case R_USBSTS:
        s->sts &= ~v;
        break;
    case R_USBINTR:
        s->intr = v;
        break;
    case R_DEVICEADDR:
        s->devaddr = v;
        break;
    case R_ENDPTLISTADDR:
        s->listaddr = v & ~0x7ffu;
        break;
    case R_BURSTSIZE:
        s->burstsize = v;
        break;
    case R_TXFILLTUNING:
        s->txfill = v;
        break;
    case R_ENDPTNAKEN:
        s->naken = v;
        break;
    case R_PORTSC1:
        break;                  /* force-resume, suspend: not modelled */
    case R_OTGSC:
        s->otgsc = (s->otgsc & ~(v & OTG_STATUS_W1C)) |
                   (v & ~(OTG_STATUS_W1C | OTG_BSV | OTG_ASV | OTG_AVV | OTG_ID));
        s->otgsc &= ~(v & OTG_STATUS_W1C);
        break;
    case R_USBMODE:
        s->mode = v;
        break;
    case R_ENDPTSETUPSTAT:
        s->setupstat &= ~v;
        break;
    case R_ENDPTPRIME:
        for (int ep = 0; ep < IMXRT_USB_EPS; ep++) {
            for (int in = 0; in < 2; in++) {
                if ((v & ep_bit(ep, in)) && !endpoint(s, ep, in)->primed) {
                    ep_load(s, ep, in, mem_ld(qh_addr(s, ep, in) + 8));
                }
            }
        }
        break;
    case R_ENDPTFLUSH:
        for (int ep = 0; ep < IMXRT_USB_EPS; ep++) {
            for (int in = 0; in < 2; in++) {
                if (v & ep_bit(ep, in)) {
                    endpoint(s, ep, in)->primed = false;
                    s->stat &= ~ep_bit(ep, in);
                }
            }
        }
        break;
    case R_ENDPTCOMPLETE:
        s->complete &= ~v;
        break;
    default:
        if (offset >= R_ENDPTCTRL0 && offset < R_ENDPTCTRL0 + 4 * IMXRT_USB_EPS) {
            s->epctrl[(offset - R_ENDPTCTRL0) / 4] = v;
        } else if (offset < 0x200) {
            s->other[offset / 4] = v;
        }
    }
    usb_update_irq(s);
    usb_pump(s);
}

static const MemoryRegionOps usb_ops = {
    .read = usb_read,
    .write = usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void usb_reset(DeviceState *dev)
{
    IMXRTUSB *s = IMXRT_USB(dev);

    usb_cancel_all(s);
    s->cmd = 0x00080000;
    s->sts = s->intr = s->setupstat = s->stat = s->complete = 0;
    s->devaddr = s->listaddr = 0;
    s->mode = 0;
    s->portsc = s->attached ? (PORT_CCS | PORT_PE | PORT_PP | PORT_PSPD_HS) : PORT_PP;
    s->otgsc = s->attached ? (OTG_BSV | OTG_ASV | OTG_AVV) : 0;
    s->announced = false;
    memset(s->epctrl, 0, sizeof(s->epctrl));
    memset(s->eps, 0, sizeof(s->eps));
}

static void usb_realize(DeviceState *dev, Error **errp)
{
    IMXRTUSB *s = IMXRT_USB(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &usb_ops, s, "imxrt.usb",
                          0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    QTAILQ_INIT(&s->xfers);
}

static void usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = usb_realize;
    device_class_set_legacy_reset(dc, usb_reset);
}

static const TypeInfo usb_info = {
    .name = TYPE_IMXRT_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTUSB),
    .class_init = usb_class_init,
};

static void usb_register_types(void)
{
    type_register_static(&usb_info);
}

type_init(usb_register_types)
