/*
 * SP-404MKII emulator: shared definitions.
 *
 * The SP-404MKII is an NXP i.MX RT1060 (Cortex-M7) running micro T-Kernel,
 * with a companion microcontroller (the BMC) on LPUART3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SP404_H
#define HW_ARM_SP404_H

#include "qemu/units.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "qom/object.h"
#include "qemu/bitmap.h"
#include "hw/ssi/ssi.h"
#include "chardev/char-fe.h"
#include "ui/console.h"

/*
 * Device tracing: SP404_TRACE=edma,lpspi,... (or "all") in the environment
 * enables the named devices' trace lines in the QEMU log (-D).
 */
bool sp404_trace_enabled(const char *dev);
#define SP404_TRACE(dev, fmt, ...) do {                                   \
        static int on_ = -1;                                                \
        if (on_ < 0) {                                                      \
            on_ = sp404_trace_enabled(dev);                                 \
        }                                                                   \
        if (on_) {                                                          \
            qemu_log(dev ": " fmt "\n", ## __VA_ARGS__);                    \
        }                                                                   \
    } while (0)

/* i.MX RT1060 memory map, as the SP-404MKII uses it. */
#define SP404_ITCM_BASE         0x00000000
#define SP404_ITCM_SIZE         (512 * KiB)
#define SP404_BOOTROM_BASE      0x00200000
#define SP404_BOOTROM_SIZE      (128 * KiB)
#define SP404_DTCM_BASE         0x20000000
#define SP404_DTCM_SIZE         (512 * KiB)
#define SP404_OCRAM_BASE        0x20200000
#define SP404_OCRAM_SIZE        (1 * MiB)
#define SP404_PERIPH_BASE       0x40000000
#define SP404_PERIPH_SIZE       0x00400000
#define SP404_FLEXSPI_BASE      0x60000000
#define SP404_FLEXSPI_SIZE      (4 * MiB)
#define SP404_FLEXSPI2_BASE     0x70000000
#define SP404_FLEXSPI2_SIZE     (16 * MiB)
#define SP404_SDRAM_BASE        0x80000000
#define SP404_SDRAM_SIZE        (64 * MiB)

/* Where Roland's updater programs SP404MKII_APP1.bin in the QSPI NOR. */
#define SP404_APP1_FLASH_OFFSET 0x00080000

#define SP404_NUM_IRQ           160
#define SP404_CPU_HZ            600000000

/* Bits forced on (set) or off (clear) when a stubbed register is read. */
typedef struct SP404StubBits {
    hwaddr addr;
    uint32_t set;
    uint32_t clear;
} SP404StubBits;

/*
 * A range of registers laid out in i.MX "SCT" groups of four words: the
 * register, then SET, CLR and TOG aliases that act on it.
 */
typedef struct SP404StubRange {
    hwaddr start, end;          /* end is exclusive */
} SP404StubRange;

#define TYPE_SP404_REGSTUB "sp404-regstub"
OBJECT_DECLARE_SIMPLE_TYPE(SP404RegStub, SP404_REGSTUB)

struct SP404RegStub {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *name;
    uint64_t base;
    uint64_t size;
    const SP404StubBits *bits;  /* zero-terminated, set by the machine */
    const SP404StubRange *sct;  /* zero-terminated, set by the machine */
    uint32_t *regs;
    uint8_t *seen;
};

#define TYPE_IMXRT_FLEXSPI "imxrt-flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTFlexSPI, IMXRT_FLEXSPI)

struct IMXRTFlexSPI {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    MemoryRegion *flash;        /* the NOR's AHB window, a ROM region */
    uint64_t flash_base;
    uint32_t jedec_id;
    /* Told of every program/erase, to keep a backing file in step. */
    void (*flash_dirty)(void *opaque, uint32_t offset, uint32_t len);
    void *flash_dirty_opaque;

    uint32_t regs[0x300 / 4];
    int cur_cmd;
    bool wel;                   /* NOR write-enable latch */
    uint8_t sr2;
    uint8_t rxbuf[64 * KiB];
    uint32_t rx_len, rx_pos;
    uint8_t txbuf[64 * KiB];
    uint32_t tx_len, tx_expected;
    uint8_t tfdr[128];
};

#define TYPE_IMXRT_GPIO "imxrt-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTGPIO, IMXRT_GPIO)

struct IMXRTGPIO {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[10];           /* 0-15, 16-31, then pins 0-7 singly */
    qemu_irq out[32];
    /* Computes the input levels when the pads are read (key matrices). */
    uint32_t (*in_hook)(void *opaque, uint32_t in);
    void *in_hook_opaque;
    char *name;
    uint32_t reset_in;
    uint32_t dr, gdir, icr1, icr2, imr, isr, edge_sel;
    uint32_t in, last_out;
};

#define TYPE_IMXRT_EDMA "imxrt-edma"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTEDMA, IMXRT_EDMA)

#define IMXRT_EDMA_CHANNELS  32
#define IMXRT_DMAMUX_SOURCES 128

struct IMXRTEDMA {
    SysBusDevice parent_obj;
    MemoryRegion iomem, mux_iomem;
    qemu_irq irq[17];           /* channels n and n+16 share n; 16: error */
    QEMUBH *bh;
    uint32_t cr, es, erq, eei, intr, err, hrs, ears;
    uint8_t dchpri[IMXRT_EDMA_CHANNELS];
    uint8_t tcd[IMXRT_EDMA_CHANNELS][32];
    uint32_t chcfg[IMXRT_EDMA_CHANNELS];
    DECLARE_BITMAP(dreq, IMXRT_DMAMUX_SOURCES);
    uint32_t pending_start;
    bool busy, again;
};

#define TYPE_IMXRT_LPSPI "imxrt-lpspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTLPSPI, IMXRT_LPSPI)

#define IMXRT_LPSPI_FIFO 16

struct IMXRTLPSPI {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq, tx_dreq, rx_dreq;
    qemu_irq cs[4];
    SSIBus *bus;
    uint32_t cr, sr, ier, der, cfgr0, cfgr1, dmr0, dmr1, ccr, fcr, tcr;
    uint32_t txf[IMXRT_LPSPI_FIFO], rxf[IMXRT_LPSPI_FIFO];
    unsigned tx_head, tx_count, rx_head, rx_count;
    int cs_active;
};

#define TYPE_IMXRT_LPUART "imxrt-lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTLPUART, IMXRT_LPUART)

#define IMXRT_LPUART_RXQ 256

struct IMXRTLPUART {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq, tx_dreq, rx_dreq;
    CharFrontend chr;
    char *name;
    /* A peer modelled in the machine: gets every byte sent. */
    void (*tx)(void *opaque, uint8_t byte);
    void *tx_opaque;
    uint32_t global, pincfg, baud, stat, ctrl, match, modir, fifo, water;
    uint8_t rxq[IMXRT_LPUART_RXQ];
    unsigned rx_head, rx_count;
};

/* Bytes from the peer, as if they had arrived on the RX line. */
void imxrt_lpuart_receive(IMXRTLPUART *s, const uint8_t *buf, int len);

#define TYPE_SSD1309 "ssd1309"
OBJECT_DECLARE_SIMPLE_TYPE(SSD1309State, SSD1309)

#define SSD1309_WIDTH  128
#define SSD1309_HEIGHT 64

struct SSD1309State {
    SSIPeripheral parent_obj;
    QemuConsole *con;
    bool dc, in_reset, redraw;
    uint8_t cmd[8];
    int cmd_len;
    uint8_t ram[SSD1309_WIDTH * SSD1309_HEIGHT / 8];
    int mode, col, page, col_start, col_end, page_start, page_end;
    int start_line;
    bool seg_remap, com_remap, invert, all_on, on;
    uint8_t contrast;
    uint64_t frames;
};

/* The pixel shown at (x, y), (0, 0) top left, as the panel shows it. */
bool ssd1309_pixel(SSD1309State *s, int x, int y);

#define TYPE_IMXRT_SAI "imxrt-sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTSAI, IMXRT_SAI)

#define IMXRT_SAI_FIFO      32
#define IMXRT_SAI_MAX_WORDS 32

typedef struct IMXRTSAIFifo {
    uint32_t data[IMXRT_SAI_FIFO];
    unsigned head, count;
} IMXRTSAIFifo;

/*
 * Called once per frame with the words sent on each TX line; fills in the
 * words to be received on each RX line.
 */
typedef void IMXRTSAIFrameHook(void *opaque,
                               uint32_t tx[4][IMXRT_SAI_MAX_WORDS],
                               unsigned tx_words,
                               uint32_t rx[4][IMXRT_SAI_MAX_WORDS],
                               unsigned rx_words);

struct IMXRTSAI {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq, tx_dreq, rx_dreq;
    QEMUTimer *timer;
    char *name;
    uint32_t rate;
    IMXRTSAIFrameHook *frame_hook;
    void *frame_opaque;
    /* [0] transmitter, [1] receiver */
    uint32_t csr[2], cr1[2], cr2[2], cr3[2], cr4[2], cr5[2], mr[2];
    IMXRTSAIFifo fifo[2][4];
    int64_t next_tick;
    uint64_t frames;
};

#define TYPE_IMXRT_PIT "imxrt-pit"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTPIT, IMXRT_PIT)

typedef struct IMXRTPITChannel {
    struct IMXRTPIT *pit;
    QEMUTimer *timer;
    uint32_t ldval, tctrl;
    bool tif;
    int64_t start;
} IMXRTPITChannel;

struct IMXRTPIT {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq trigger[4];
    uint32_t freq, mcr;
    IMXRTPITChannel ch[4];
};

#define TYPE_IMXRT_XBAR "imxrt-xbar"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTXBAR, IMXRT_XBAR)

#define IMXRT_XBAR_INPUTS  128
#define IMXRT_XBAR_OUTPUTS 132

struct IMXRTXBAR {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq out[IMXRT_XBAR_OUTPUTS];
    uint8_t sel[IMXRT_XBAR_OUTPUTS];
    bool in_level[IMXRT_XBAR_INPUTS];
    uint16_t ctrl[2];
};

#define TYPE_IMXRT_ADC "imxrt-adc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTADC, IMXRT_ADC)

typedef struct IMXRTADCUnit {
    uint32_t hc[8], r[8], hs, cfg, gc, gs, cv, ofs, cal;
} IMXRTADCUnit;

typedef struct IMXRTADCTrig {
    uint32_t ctrl, counter, chain[4], result[4];
} IMXRTADCTrig;

struct IMXRTADC {
    SysBusDevice parent_obj;
    MemoryRegion mmio[3];
    qemu_irq irq[6];
    /* The analog world: 12-bit value on ADC adc (0, 1), channel ch. */
    uint16_t (*sample)(void *opaque, int adc, int ch);
    void *sample_opaque;
    IMXRTADCUnit adc[2];
    IMXRTADCTrig trig[8];
    uint32_t etc_ctrl, done0_1, done2_err, dma_ctrl, trig_level;
};

/*
 * The SP-404's audio path as seen from the SAI: TX line 3 carries the
 * mix to the main outputs, and RX line 0 brings back the inputs plus the
 * resampling loopback.
 */
typedef struct SP404Audio {
    FILE *wav;
    uint64_t wav_frames;
    int32_t slot_peak[4][16];
    int peak_frames;
    /* Stereo output, for the frontend link. */
    void (*out)(void *opaque, const int16_t *lr, int frames);
    void *out_opaque;
} SP404Audio;

void sp404_audio_init(SP404Audio *a, IMXRTSAI *sai);

/*
 * The BMC: the companion microcontroller on LPUART3, modelled at the level
 * of the packets it exchanges with the i.MX.
 */
typedef struct SP404Link SP404Link;

typedef struct SP404BMC {
    IMXRTLPUART *uart;
    SP404Link *link;            /* copies of what the firmware sends */
    uint8_t pkt[4];
    unsigned pkt_len;
} SP404BMC;

void sp404_bmc_init(SP404BMC *bmc, IMXRTLPUART *uart);
/* A packet from the BMC to the firmware (pads, SHIFT, ...). */
void sp404_bmc_inject(SP404BMC *bmc, const uint8_t *pkt);

/* The frontend link (sp404-link.c has the protocol). */
struct SP404Link {
    CharFrontend chr;
    bool connected, resend;
    SSD1309State *oled;
    QEMUTimer *timer;
    uint8_t last_img[SSD1309_WIDTH * SSD1309_HEIGHT / 8];
    uint8_t audio[64 * 4];
    int audio_len;
    uint8_t rx[4 + 256];
    int rx_len;
    void *opaque;
    void (*key)(void *opaque, int row, int col, bool pressed);
    void (*knob)(void *opaque, int adc, int ch, int mux, uint16_t value);
    void (*bmc_rx)(void *opaque, const uint8_t *pkt);
    void (*encoder)(void *opaque, int steps);
};

void sp404_link_init(SP404Link *l, Chardev *chr, SSD1309State *oled);
void sp404_link_audio(void *opaque, const int16_t *lr, int frames);
void sp404_link_bmc_tx(SP404Link *l, const uint8_t *pkt);

#endif
