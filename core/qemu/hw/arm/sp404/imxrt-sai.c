/*
 * i.MX RT SAI (I2S/TDM) with four TX and four RX data lines.
 *
 * A timer on the virtual clock stands in for the bit clock: every tick
 * it runs a block of frames (FRSZ+1 words per frame on each enabled line),
 * popping each enabled TX FIFO and pushing each enabled RX FIFO one word
 * per slot, and raising the DMA requests as the FIFOs cross their
 * watermarks. The eDMA refills and drains the FIFOs synchronously in
 * reaction, so the firmware's audio interrupt rate follows the sample
 * rate the machine gives this device.
 *
 * What happens to the transmitted frames, and what is received, is up to
 * the machine through the frame hook: the SP-404's hook mixes TX line 3
 * down to stereo and loops it back as the codec path does.
 *
 * DMA requests are GPIO outputs "tx-dreq" and "rx-dreq".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/runstate.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/sp404/sp404.h"

#define R_VERID 0x00
#define R_PARAM 0x04
#define R_TCSR  0x08
#define R_TCR1  0x0c
#define R_TCR2  0x10
#define R_TCR3  0x14
#define R_TCR4  0x18
#define R_TCR5  0x1c
#define R_TDR0  0x20
#define R_TFR0  0x40
#define R_TMR   0x60
#define RX      0x80            /* the receiver's registers sit 0x80 above */

#define CSR_FRDE (1u << 0)
#define CSR_FWDE (1u << 1)
#define CSR_FRIE (1u << 8)
#define CSR_FWIE (1u << 9)
#define CSR_FEIE (1u << 10)
#define CSR_SEIE (1u << 11)
#define CSR_WSIE (1u << 12)
#define CSR_FRF  (1u << 16)
#define CSR_FWF  (1u << 17)
#define CSR_FEF  (1u << 18)
#define CSR_SEF  (1u << 19)
#define CSR_WSF  (1u << 20)
#define CSR_SR   (1u << 24)
#define CSR_FR   (1u << 25)
#define CSR_BCE  (1u << 28)
#define CSR_EN   (1u << 31)
#define CSR_W1C  (CSR_FEF | CSR_SEF | CSR_WSF)

#define FIFO     IMXRT_SAI_FIFO
#define BLOCK    64              /* frames per timer tick */

static unsigned fifo_count(IMXRTSAIFifo *f)
{
    return f->count;
}

static void fifo_push(IMXRTSAIFifo *f, uint32_t v, uint32_t *csr)
{
    if (f->count == FIFO) {
        *csr |= CSR_FEF;
        return;
    }
    f->data[(f->head + f->count++) % FIFO] = v;
}

static uint32_t fifo_pop(IMXRTSAIFifo *f, uint32_t *csr)
{
    uint32_t v;

    if (!f->count) {
        *csr |= CSR_FEF;
        return 0;
    }
    v = f->data[f->head];
    f->head = (f->head + 1) % FIFO;
    f->count--;
    return v;
}

static unsigned sai_lines(IMXRTSAI *s, bool rx)
{
    return (s->cr3[rx] >> 16) & 0xf;
}

static void sai_update(IMXRTSAI *s)
{
    bool irq = false;

    for (int rx = 0; rx < 2; rx++) {
        unsigned lines = sai_lines(s, rx), wm = s->cr1[rx] & 0x1f;
        uint32_t csr = s->csr[rx] & ~(CSR_FRF | CSR_FWF);
        bool frf = false, fwf = false;

        for (int l = 0; l < 4; l++) {
            unsigned n;

            if (!(lines & (1u << l))) {
                continue;
            }
            n = fifo_count(&s->fifo[rx][l]);
            if (rx ? n > wm : n <= wm) {
                frf = true;
            }
            if (rx ? n == FIFO : n == 0) {
                fwf = true;
            }
        }
        if (frf) {
            csr |= CSR_FRF;
        }
        if (fwf) {
            csr |= CSR_FWF;
        }
        s->csr[rx] = csr;
        irq |= ((csr & CSR_FRIE) && (csr & CSR_FRF)) ||
               ((csr & CSR_FWIE) && (csr & CSR_FWF)) ||
               ((csr & CSR_FEIE) && (csr & CSR_FEF)) ||
               ((csr & CSR_SEIE) && (csr & CSR_SEF)) ||
               ((csr & CSR_WSIE) && (csr & CSR_WSF));
    }
    qemu_set_irq(s->irq, irq);
    qemu_set_irq(s->tx_dreq, (s->csr[0] & CSR_EN) && (s->csr[0] & CSR_FRDE) &&
                 (s->csr[0] & CSR_FRF));
    qemu_set_irq(s->rx_dreq, (s->csr[1] & CSR_EN) && (s->csr[1] & CSR_FRDE) &&
                 (s->csr[1] & CSR_FRF));
}

static bool sai_running(IMXRTSAI *s)
{
    return (s->csr[0] | s->csr[1]) & CSR_EN;
}

/* Clock one frame through: TX FIFOs out, RX FIFOs in. */
static void sai_frame(IMXRTSAI *s)
{
    unsigned words = ((s->cr4[0] >> 16) & 0x1f) + 1;
    unsigned rwords = ((s->cr4[1] >> 16) & 0x1f) + 1;
    uint32_t tx[4][IMXRT_SAI_MAX_WORDS] = {}, rx[4][IMXRT_SAI_MAX_WORDS] = {};
    unsigned l, w;

    if (s->csr[0] & CSR_EN) {
        for (l = 0; l < 4; l++) {
            if (sai_lines(s, false) & (1u << l)) {
                for (w = 0; w < words; w++) {
                    tx[l][w] = fifo_pop(&s->fifo[0][l], &s->csr[0]);
                }
            }
        }
    }
    if (s->frame_hook) {
        s->frame_hook(s->frame_opaque, tx, words, rx, rwords);
    }
    if (s->csr[1] & CSR_EN) {
        for (l = 0; l < 4; l++) {
            if (sai_lines(s, true) & (1u << l)) {
                for (w = 0; w < rwords; w++) {
                    fifo_push(&s->fifo[1][l], rx[l][w], &s->csr[1]);
                }
            }
        }
    }
    /* Requests follow the FIFOs; the eDMA answers them before we return. */
    sai_update(s);
}

/*
 * Clock the next block if it is due and the software is ready for it.
 * Returns when to look again (virtual ns), or -1 when stopped.
 *
 * The hardware plays on regardless, but here the emulated CPU runs in a
 * different thread from this clock: a block clocked out before the
 * firmware has rendered the next buffer would play stale data. So a due
 * block waits (briefly: a stuck handler must not stop the clock) until the
 * machine's ready hook agrees, and blocks that fell behind are caught up
 * one at a time, each waiting for the software again.
 */
static int64_t sai_step(IMXRTSAI *s, int64_t now)
{
    int i;

    if (!sai_running(s)) {
        return -1;
    }
    if (now < s->next_tick) {
        return s->next_tick;
    }
    if (s->ready && !s->ready(s->ready_opaque)) {
        if (!s->stall_start) {
            s->stall_start = now;
        }
        if (now - s->stall_start < 5 * SCALE_MS) {
            return now + 100 * SCALE_US;
        }
        s->late_blocks++;
        SP404_TRACE("sai", "%s: block played late (%" PRIu64 " so far)",
                    s->name ? s->name : "?", s->late_blocks);
    }
    if (s->stall_start) {
        int64_t waited = now - s->stall_start;

        s->stall_total += waited;
        s->stall_max = MAX(s->stall_max, waited);
    }
    s->stall_start = 0;
    if (++s->stat_blocks == 750) {
        SP404_TRACE("sai-stall", "%s: 750 blocks, waited %" PRId64 " us in all, "
                    "longest %" PRId64 " us", s->name ? s->name : "?",
                    s->stall_total / 1000, s->stall_max / 1000);
        s->stat_blocks = 0;
        s->stall_total = s->stall_max = 0;
    }
    for (i = 0; i < BLOCK; i++) {
        sai_frame(s);
    }
    s->frames += BLOCK;
    s->next_tick += (int64_t)BLOCK * NANOSECONDS_PER_SECOND / s->rate;
    if (s->next_tick < now - NANOSECONDS_PER_SECOND / 10) {
        s->next_tick = now;     /* fell far behind: do not try to catch up */
    }
    /* Due again already (catching up)? Give the software its turn first. */
    return MAX(s->next_tick, now + 20 * SCALE_US);
}

static void sai_tick(void *opaque)
{
    IMXRTSAI *s = opaque;
    int64_t next = sai_step(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    if (next >= 0 && !s->precise) {
        timer_mod(s->timer, next);
    }
}

/*
 * The precise clock: a host thread that sleeps with sub-millisecond
 * accuracy (QEMU's own timers wake the main loop only every millisecond or
 * so on Windows, which bunches blocks and lets the waits add up to lost
 * real time) and runs sai_step under the BQL.
 */
static void sai_sleep_until(IMXRTSAI *s, int64_t deadline)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (deadline <= now) {
        return;
    }
#ifdef _WIN32
    if (s->hr_timer) {
        LARGE_INTEGER due;

        due.QuadPart = -((deadline - now) / 100);       /* 100 ns units */
        if (due.QuadPart == 0) {
            due.QuadPart = -1;
        }
        SetWaitableTimer(s->hr_timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(s->hr_timer, INFINITE);
        return;
    }
#endif
    g_usleep(MAX(1, (deadline - now) / 1000));
}

static void *sai_clock_thread(void *opaque)
{
    IMXRTSAI *s = opaque;

#ifdef _WIN32
    s->hr_timer = CreateWaitableTimerExW(NULL, NULL,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
#endif
    while (!qatomic_read(&s->clock_stop)) {
        int64_t next;

        bql_lock();
        next = runstate_is_running() ?
               sai_step(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) : -1;
        bql_unlock();
        sai_sleep_until(s, next >= 0 ? next :
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * SCALE_MS);
    }
    return NULL;
}

void imxrt_sai_start_precise_clock(IMXRTSAI *s)
{
    s->precise = true;
    timer_del(s->timer);
    qemu_thread_create(&s->clock_thread, "sai-clock", sai_clock_thread, s,
                       QEMU_THREAD_JOINABLE);
}

void imxrt_sai_kick(IMXRTSAI *s)
{
    /*
     * Waiting on the software, which has just caught up: go on at once
     * rather than at the next poll, which on some hosts (Windows timers
     * have millisecond granularity) comes late enough to lose real time.
     */
    if (s->stall_start && sai_running(s)) {
        sai_tick(s);
    }
}

static void sai_start_stop(IMXRTSAI *s, bool was_running)
{
    if (sai_running(s) && !was_running) {
        s->next_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       (int64_t)BLOCK * NANOSECONDS_PER_SECOND / s->rate;
        if (!s->precise) {
            timer_mod(s->timer, s->next_tick);
        }
        SP404_TRACE("sai", "%s running at %u Hz", s->name ? s->name : "?",
                    s->rate);
    } else if (!sai_running(s)) {
        timer_del(s->timer);
    }
}

static uint64_t sai_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRTSAI *s = opaque;
    bool rx = offset >= RX;
    hwaddr reg = rx ? offset - RX : offset;
    uint32_t v = 0;

    switch (reg) {
    case R_VERID: v = rx ? 0 : 0x03010000; break;
    case R_PARAM: v = rx ? 0 : 0x00050504; break;  /* 32-word FIFOs, 4 lines */
    case R_TCSR: v = s->csr[rx]; break;
    case R_TCR1: v = s->cr1[rx]; break;
    case R_TCR2: v = s->cr2[rx]; break;
    case R_TCR3: v = s->cr3[rx]; break;
    case R_TCR4: v = s->cr4[rx]; break;
    case R_TCR5: v = s->cr5[rx]; break;
    case R_TDR0 ... R_TDR0 + 0xc:
        if (rx) {
            v = fifo_pop(&s->fifo[1][(reg - R_TDR0) / 4], &s->csr[1]);
            sai_update(s);
        }
        break;
    case R_TFR0 ... R_TFR0 + 0xc: {
        IMXRTSAIFifo *f = &s->fifo[rx][(reg - R_TFR0) / 4];
        /* Pointers with a wrap bit: fill = WFP - RFP. */
        unsigned rfp = f->head % (2 * FIFO), wfp = (f->head + f->count) % (2 * FIFO);
        v = rfp | (wfp << 16);
        break;
    }
    case R_TMR: v = s->mr[rx]; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-sai: read 0x%" HWADDR_PRIx "\n",
                      offset);
    }
    return v;
}

static void sai_write(void *opaque, hwaddr offset, uint64_t val,
                      unsigned size)
{
    IMXRTSAI *s = opaque;
    bool rx = offset >= RX;
    hwaddr reg = rx ? offset - RX : offset;
    bool was_running = sai_running(s);
    uint32_t v = val;

    switch (reg) {
    case R_TCSR:
        if (v & CSR_FR) {
            for (int l = 0; l < 4; l++) {
                s->fifo[rx][l].head = s->fifo[rx][l].count = 0;
            }
        }
        s->csr[rx] = (s->csr[rx] & (CSR_FRF | CSR_FWF | CSR_W1C)) & ~(v & CSR_W1C);
        s->csr[rx] |= v & ~(CSR_FRF | CSR_FWF | CSR_W1C | CSR_FR);
        break;
    case R_TCR1: s->cr1[rx] = v; break;
    case R_TCR2: s->cr2[rx] = v; break;
    case R_TCR3: s->cr3[rx] = v; break;
    case R_TCR4: s->cr4[rx] = v; break;
    case R_TCR5: s->cr5[rx] = v; break;
    case R_TDR0 ... R_TDR0 + 0xc:
        if (!rx) {
            fifo_push(&s->fifo[0][(reg - R_TDR0) / 4], v, &s->csr[0]);
        }
        break;
    case R_TMR: s->mr[rx] = v; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt-sai: write 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    sai_update(s);
    sai_start_stop(s, was_running);
}

static const MemoryRegionOps sai_ops = {
    .read = sai_read,
    .write = sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* The SP-404 reads 16-bit samples from RDR: one pop per access. */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sai_reset(DeviceState *dev)
{
    IMXRTSAI *s = IMXRT_SAI(dev);

    timer_del(s->timer);
    memset(s->csr, 0, sizeof(s->csr));
    memset(s->cr1, 0, sizeof(s->cr1));
    memset(s->cr2, 0, sizeof(s->cr2));
    memset(s->cr3, 0, sizeof(s->cr3));
    memset(s->cr4, 0, sizeof(s->cr4));
    memset(s->cr5, 0, sizeof(s->cr5));
    memset(s->mr, 0, sizeof(s->mr));
    memset(s->fifo, 0, sizeof(s->fifo));
    sai_update(s);
}

static void sai_realize(DeviceState *dev, Error **errp)
{
    IMXRTSAI *s = IMXRT_SAI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &sai_ops, s, "imxrt.sai",
                          0x4000);
    /* The eDMA fills the FIFOs from inside our own register writes. */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_out_named(dev, &s->tx_dreq, "tx-dreq", 1);
    qdev_init_gpio_out_named(dev, &s->rx_dreq, "rx-dreq", 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sai_tick, s);
}

static const Property sai_properties[] = {
    DEFINE_PROP_STRING("name", IMXRTSAI, name),
    DEFINE_PROP_UINT32("rate", IMXRTSAI, rate, 48000),
};

static void sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sai_realize;
    device_class_set_legacy_reset(dc, sai_reset);
    device_class_set_props(dc, sai_properties);
}

static const TypeInfo sai_info = {
    .name = TYPE_IMXRT_SAI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRTSAI),
    .class_init = sai_class_init,
};

static void sai_register_types(void)
{
    type_register_static(&sai_info);
}

type_init(sai_register_types)
