/*
 * The SP-404MKII's USB port as a USB/IP server: a USB/IP client on the
 * computer (usbip-win2 on Windows, usbip on Linux) lists and attaches the
 * unit, and sees the device its firmware presents, driven through the
 * emulated USB controller (imxrt-usb.c).
 *
 * The chardev is a TCP server (usually port 3240). The protocol, all big
 * endian: OP_REQ_DEVLIST (answered with the one device, busid "1-1") and
 * OP_REQ_IMPORT on a fresh connection; after an import, USBIP_CMD_SUBMIT /
 * RET_SUBMIT per URB and CMD_UNLINK / RET_UNLINK to cancel one.
 *
 * The first client "plugs the cable in": the port attaches, the bus resets
 * and the firmware's descriptors are read for the device list. An import
 * resets the bus again and gives the unit address 1 (USB/IP clients keep
 * SET_ADDRESS to themselves), so what follows finds it addressed.
 * Isochronous URBs (USB audio streaming) are refused for now.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/queue.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/arm/sp404/sp404.h"

#define USBIP_VERSION       0x0111
#define OP_REQ_DEVLIST      0x8005
#define OP_REP_DEVLIST      0x0005
#define OP_REQ_IMPORT       0x8003
#define OP_REP_IMPORT       0x0003
#define CMD_SUBMIT          1
#define CMD_UNLINK          2
#define RET_SUBMIT          3
#define RET_UNLINK          4
#define BUSID               "1-1"
#define ENUM_TIMEOUT_NS     (3 * NANOSECONDS_PER_SECOND)
#define RESET_SETTLE_NS     (100 * SCALE_MS)
#define RX_SIZE             (1024 * 1024 + 64)

typedef struct Urb Urb;
struct Urb {
    IMXRTUSBXfer x;
    SP404USBIP *u;
    QTAILQ_ENTRY(Urb) link;
    uint32_t seqnum;
    bool in;
};

typedef enum {
    ENUM_IDLE, ENUM_RESET, ENUM_DEVICE, ENUM_CONFIG9, ENUM_CONFIG, ENUM_ADDRESS,
} EnumStep;

struct SP404USBIP {
    IMXRTUSB *usb;
    CharFrontend chr;
    bool connected, imported;
    uint8_t *rx;                /* RX_SIZE */
    uint32_t rx_len;
    QTAILQ_HEAD(, Urb) urbs;

    /* The device's descriptors, for the list and the import answer. */
    bool have_info;
    uint8_t dev_desc[18];
    uint8_t config[1024];
    uint32_t config_len;

    /* Getting them (or addressing the device), then answering. */
    EnumStep step;
    bool want_list, want_import, address_only;
    IMXRTUSBXfer ex;
    uint8_t ebuf[1024];
    QEMUTimer *timer;
};

static void usbip_send(SP404USBIP *u, const void *d, size_t n)
{
    if (u->connected && n) {
        qemu_chr_fe_write_all(&u->chr, d, n);
    }
}

/* ---------------------------------------------------------------------- */
/* Replies. */

/* The device record (usbip_usb_device): 312 bytes. */
static void put_device(SP404USBIP *u, uint8_t *p)
{
    const uint8_t *d = u->dev_desc;

    memset(p, 0, 312);
    snprintf((char *)p, 256, "/sys/devices/dm-404/usb1/" BUSID);
    snprintf((char *)p + 256, 32, BUSID);
    stl_be_p(p + 288, 1);                   /* busnum */
    stl_be_p(p + 292, 2);                   /* devnum */
    stl_be_p(p + 296, 3);                   /* speed: high */
    stw_be_p(p + 300, lduw_le_p(d + 8));    /* idVendor */
    stw_be_p(p + 302, lduw_le_p(d + 10));   /* idProduct */
    stw_be_p(p + 304, lduw_le_p(d + 12));   /* bcdDevice */
    p[306] = d[4];
    p[307] = d[5];
    p[308] = d[6];
    p[309] = u->config_len > 5 ? u->config[5] : 1;     /* bConfigurationValue */
    p[310] = d[17];                                     /* bNumConfigurations */
    p[311] = u->config_len > 4 ? u->config[4] : 0;     /* bNumInterfaces */
}

static void reply_devlist(SP404USBIP *u)
{
    uint8_t out[12 + 312 + 32 * 4] = { 0 };
    size_t n = 12;

    stw_be_p(out, USBIP_VERSION);
    stw_be_p(out + 2, OP_REP_DEVLIST);
    stl_be_p(out + 8, u->have_info ? 1 : 0);
    if (u->have_info) {
        put_device(u, out + n);
        n += 312;
        /* Each interface's class (the first alternate setting of each). */
        for (uint32_t i = 0; i + 2 <= u->config_len && n + 4 <= sizeof(out);
             i += MAX(u->config[i], 1)) {
            const uint8_t *dsc = u->config + i;

            if (dsc[1] == 4 && dsc[0] >= 9 && i + 9 <= u->config_len && dsc[3] == 0) {
                out[n] = dsc[5];
                out[n + 1] = dsc[6];
                out[n + 2] = dsc[7];
                n += 4;
            }
        }
    }
    usbip_send(u, out, n);
}

static void reply_import(SP404USBIP *u, bool ok)
{
    uint8_t out[8 + 312] = { 0 };

    stw_be_p(out, USBIP_VERSION);
    stw_be_p(out + 2, OP_REP_IMPORT);
    stl_be_p(out + 4, ok ? 0 : 1);
    if (ok) {
        put_device(u, out + 8);
    }
    usbip_send(u, out, ok ? sizeof(out) : 8);
    u->imported = ok;
}

/* ---------------------------------------------------------------------- */
/* Enumeration: the descriptors for the list, and addressing on import. */

static void enum_finish(SP404USBIP *u, bool ok)
{
    timer_del(u->timer);
    u->step = ENUM_IDLE;
    if (!ok) {
        qemu_log("sp404-usbip: the unit did not answer on USB\n");
    }
    if (u->want_list) {
        u->want_list = false;
        reply_devlist(u);
    }
    if (u->want_import) {
        u->want_import = false;
        reply_import(u, ok && u->have_info);
    }
}

static void enum_control(SP404USBIP *u, uint8_t type, uint8_t req,
                         uint16_t value, uint16_t len);

static void enum_done(void *opaque, IMXRTUSBXfer *x)
{
    SP404USBIP *u = opaque;

    if (x->status) {
        enum_finish(u, false);
        return;
    }
    switch (u->step) {
    case ENUM_DEVICE:
        memcpy(u->dev_desc, u->ebuf, MIN(x->done, 18));
        u->step = ENUM_CONFIG9;
        enum_control(u, 0x80, 6, 0x0200, 9);
        break;
    case ENUM_CONFIG9:
        u->step = ENUM_CONFIG;
        enum_control(u, 0x80, 6, 0x0200,
                     MIN(lduw_le_p(u->ebuf + 2), sizeof(u->config)));
        break;
    case ENUM_CONFIG:
        memcpy(u->config, u->ebuf, x->done);
        u->config_len = x->done;
        u->have_info = true;
        qemu_log("sp404-usbip: the unit is %04x:%04x, %u interfaces\n",
                 lduw_le_p(u->dev_desc + 8), lduw_le_p(u->dev_desc + 10),
                 u->config[4]);
        u->step = ENUM_ADDRESS;
        enum_control(u, 0x00, 5, 1, 0);
        break;
    case ENUM_ADDRESS:
        enum_finish(u, true);
        break;
    default:
        break;
    }
}

static void enum_control(SP404USBIP *u, uint8_t type, uint8_t req,
                         uint16_t value, uint16_t len)
{
    IMXRTUSBXfer *x = &u->ex;

    memset(x, 0, sizeof(*x));
    x->ep = 0;
    x->control = true;
    x->in = type & 0x80;
    x->setup[0] = type;
    x->setup[1] = req;
    stw_le_p(x->setup + 2, value);
    stw_le_p(x->setup + 6, len);
    x->buf = u->ebuf;
    x->len = len;
    x->complete = enum_done;
    x->opaque = u;
    imxrt_usb_host_submit(u->usb, x);
}

/* The bus has settled after its reset: ask. */
static void enum_timer(void *opaque)
{
    SP404USBIP *u = opaque;

    if (u->step == ENUM_RESET) {
        timer_mod(u->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ENUM_TIMEOUT_NS);
        if (u->address_only && u->have_info) {
            u->step = ENUM_ADDRESS;
            enum_control(u, 0x00, 5, 1, 0);
        } else {
            u->step = ENUM_DEVICE;
            enum_control(u, 0x80, 6, 0x0100, 18);
        }
        return;
    }
    /* Timed out. */
    imxrt_usb_host_cancel(u->usb, &u->ex);
    enum_finish(u, false);
}

/* Plug in (if not yet), reset the bus, and get the descriptors or address. */
static void enum_start(SP404USBIP *u, bool address_only)
{
    if (u->step != ENUM_IDLE) {
        return;                 /* already on it: the answer comes then */
    }
    if (!imxrt_usb_host_running(u->usb)) {
        /* The firmware has not started USB (yet): no device to show. */
        u->usb->attached = true;
        enum_finish(u, false);
        return;
    }
    if (!u->usb->attached) {
        imxrt_usb_host_attach(u->usb, true);
    }
    imxrt_usb_host_reset(u->usb);
    u->address_only = address_only;
    u->step = ENUM_RESET;
    timer_mod(u->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RESET_SETTLE_NS);
}

/* ---------------------------------------------------------------------- */
/* URBs. */

static void urb_free(Urb *r)
{
    QTAILQ_REMOVE(&r->u->urbs, r, link);
    g_free(r->x.buf);
    g_free(r);
}

static void urb_done(void *opaque, IMXRTUSBXfer *x)
{
    Urb *r = opaque;
    SP404USBIP *u = r->u;
    uint8_t h[48] = { 0 };
    uint32_t actual = x->status ? 0 : x->done;

    stl_be_p(h, RET_SUBMIT);
    stl_be_p(h + 4, r->seqnum);
    stl_be_p(h + 20, (uint32_t)x->status);
    stl_be_p(h + 24, actual);
    usbip_send(u, h, sizeof(h));
    if (r->in && actual) {
        usbip_send(u, x->buf, actual);
    }
    urb_free(r);
}

/* A SUBMIT: the header at p, OUT data at p + 48. */
static void urb_submit(SP404USBIP *u, const uint8_t *p, uint32_t len,
                       int32_t npackets)
{
    Urb *r = g_new0(Urb, 1);
    uint32_t ep = ldl_be_p(p + 16);

    r->u = u;
    r->seqnum = ldl_be_p(p + 4);
    r->in = ldl_be_p(p + 12) == 1;
    r->x.ep = ep & 0xf;
    r->x.in = r->in;
    r->x.control = r->x.ep == 0;
    memcpy(r->x.setup, p + 40, 8);
    r->x.buf = g_malloc0(MAX(len, 1));
    r->x.len = len;
    if (!r->in) {
        memcpy(r->x.buf, p + 48, len);
    }
    r->x.complete = urb_done;
    r->x.opaque = r;
    QTAILQ_INSERT_TAIL(&u->urbs, r, link);

    if (npackets > 0 || r->x.ep >= IMXRT_USB_EPS) {
        /* Isochronous (audio streaming): not yet. */
        uint8_t h[48] = { 0 };

        stl_be_p(h, RET_SUBMIT);
        stl_be_p(h + 4, r->seqnum);
        stl_be_p(h + 20, (uint32_t)-EXDEV);
        stl_be_p(h + 32, npackets > 0 ? npackets : 0);
        stl_be_p(h + 36, npackets > 0 ? npackets : 0);
        usbip_send(u, h, sizeof(h));
        for (int i = 0; i < npackets; i++) {
            uint8_t d[16] = { 0 };

            memcpy(d, p + 48 + (r->in ? 0 : len) + i * 16, 8);   /* offset, length */
            stl_be_p(d + 12, (uint32_t)-EXDEV);
            usbip_send(u, d, 16);
        }
        urb_free(r);
        return;
    }
    imxrt_usb_host_submit(u->usb, &r->x);
}

static void urb_unlink(SP404USBIP *u, const uint8_t *p)
{
    uint32_t victim = ldl_be_p(p + 20);
    uint8_t h[48] = { 0 };
    int status = 0;
    Urb *r;

    QTAILQ_FOREACH(r, &u->urbs, link) {
        if (r->seqnum == victim) {
            if (imxrt_usb_host_cancel(u->usb, &r->x)) {
                urb_free(r);
                status = -ECONNRESET;
            }
            break;
        }
    }
    stl_be_p(h, RET_UNLINK);
    stl_be_p(h + 4, ldl_be_p(p + 4));
    stl_be_p(h + 20, (uint32_t)status);
    usbip_send(u, h, sizeof(h));
}

static void urbs_drop(SP404USBIP *u)
{
    Urb *r, *next;

    QTAILQ_FOREACH_SAFE(r, &u->urbs, link, next) {
        imxrt_usb_host_cancel(u->usb, &r->x);
        urb_free(r);
    }
}

/* ---------------------------------------------------------------------- */
/* The byte stream. */

/* How long the message at the front is, or 0 if more must come first. */
static uint32_t message_len(SP404USBIP *u)
{
    const uint8_t *p = u->rx;

    if (!u->imported) {
        if (u->rx_len < 8) {
            return 0;
        }
        return lduw_be_p(p + 2) == OP_REQ_IMPORT ? 40 : 8;
    }
    if (u->rx_len < 48) {
        return 0;
    }
    if (ldl_be_p(p) == CMD_SUBMIT) {
        uint32_t len = ldl_be_p(p + 24);
        int32_t np = (int32_t)ldl_be_p(p + 32);
        uint32_t n = 48 + (ldl_be_p(p + 12) == 0 ? len : 0);

        if (np > 0) {
            n += np * 16;
        }
        return n;
    }
    return 48;
}

static void handle(SP404USBIP *u, const uint8_t *p)
{
    if (!u->imported) {
        switch (lduw_be_p(p + 2)) {
        case OP_REQ_DEVLIST:
            if (u->have_info) {
                reply_devlist(u);
            } else {
                u->want_list = true;
                enum_start(u, false);
            }
            break;
        case OP_REQ_IMPORT:
            if (strncmp((const char *)p + 8, BUSID, 32)) {
                reply_import(u, false);
                break;
            }
            u->want_import = true;
            enum_start(u, true);
            break;
        default:
            qemu_log("sp404-usbip: unknown op %04x\n", lduw_be_p(p + 2));
        }
        return;
    }
    switch (ldl_be_p(p)) {
    case CMD_SUBMIT:
        urb_submit(u, p, ldl_be_p(p + 24), (int32_t)ldl_be_p(p + 32));
        break;
    case CMD_UNLINK:
        urb_unlink(u, p);
        break;
    default:
        qemu_log("sp404-usbip: unknown command %u\n", ldl_be_p(p));
    }
}

static int usbip_can_receive(void *opaque)
{
    SP404USBIP *u = opaque;

    return RX_SIZE - u->rx_len;
}

static void usbip_receive(void *opaque, const uint8_t *buf, int size)
{
    SP404USBIP *u = opaque;
    uint32_t n;

    memcpy(u->rx + u->rx_len, buf, size);
    u->rx_len += size;
    while ((n = message_len(u)) && n <= u->rx_len) {
        handle(u, u->rx);
        memmove(u->rx, u->rx + n, u->rx_len - n);
        u->rx_len -= n;
    }
    if (n > RX_SIZE) {
        qemu_log("sp404-usbip: a message too large (%u bytes): dropped\n", n);
        u->rx_len = 0;
    }
}

static void usbip_event(void *opaque, QEMUChrEvent event)
{
    SP404USBIP *u = opaque;

    if (event == CHR_EVENT_OPENED) {
        u->connected = true;
        u->imported = false;
        u->rx_len = 0;
    } else if (event == CHR_EVENT_CLOSED) {
        u->connected = false;
        u->imported = false;
        u->want_list = u->want_import = false;
        urbs_drop(u);
    }
}

void sp404_usbip_init(IMXRTUSB *usb, Chardev *chr)
{
    SP404USBIP *u = g_new0(SP404USBIP, 1);

    u->usb = usb;
    u->rx = g_malloc(RX_SIZE);
    QTAILQ_INIT(&u->urbs);
    u->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, enum_timer, u);
    qemu_chr_fe_init(&u->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&u->chr, usbip_can_receive, usbip_receive,
                             usbip_event, NULL, u, NULL, true);
}
