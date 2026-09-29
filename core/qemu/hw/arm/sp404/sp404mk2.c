/*
 * Roland SP-404MKII machine.
 *
 * An NXP i.MX RT1060 (Cortex-M7, 600 MHz) with 4 MB QSPI NOR on FlexSPI and
 * 64 MB SDRAM on SEMC. Roland's SP404MKII_APP1.bin is given with -bios and
 * placed where the updater programs it, flash offset 0x80000. The i.MX boot
 * ROM, which would read the flash's IVT and run its DCD to bring up SDRAM,
 * is replaced by starting the CPU straight at the application's entry: SDRAM
 * is simply always there.
 *
 * The machine option flash=FILE backs the whole 4 MB NOR with a file, so
 * what the firmware writes there (settings, factory data) persists; -bios
 * is then optional, and when given it is laid over the APP1 area as
 * Roland's updater would program it.
 *
 * Peripheral space is covered by a logging register stub; real models are
 * mapped over it as they are written.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "chardev/char.h"
#include "hw/arm/sp404/sp404.h"

/* The APP1 image's vector table: initial SP and reset handler. */
#define APP1_VECTORS_OFFSET 0x6ec

#define SP404_FLEXSPI_REG_BASE  0x402a8000
#define SP404_FLEXSPI_IRQ       108

/* GPIO1-5: base, combined IRQ for pins 0-15 (16-31 is the next one). */
static const struct {
    hwaddr base;
    int irq;
} sp404_gpio[] = {
    { 0x401b8000, 80 }, { 0x401bc000, 82 }, { 0x401c0000, 84 },
    { 0x401c4000, 86 }, { 0x400c0000, 88 },
};
#define SP404_GPIO1_INT0_IRQ    72

#define SP404_EDMA_BASE         0x400e8000
#define SP404_DMAMUX_BASE       0x400ec000
#define SP404_EDMA_ERROR_IRQ    16

/* LPUART1-8: base; IRQs are 20-27. LPUART3 is the BMC link. */
static const hwaddr sp404_lpuart_base[] = {
    0x40184000, 0x40188000, 0x4018c000, 0x40190000,
    0x40194000, 0x40198000, 0x4019c000, 0x401a0000,
};
#define SP404_LPUART1_IRQ       20
#define SP404_BMC_LPUART        2       /* LPUART3 */

/* SAI1-3: base, IRQ, DMAMUX sources for RX and TX. SAI1 is the codec link. */
static const struct {
    hwaddr base;
    int irq, rx_src, tx_src;
} sp404_sai[] = {
    { 0x40384000, 56, 19, 20 },     /* as the firmware programs DMAMUX */
    { 0x40388000, 57, 21, 22 },     /* sources unverified */
    { 0x4038c000, 59, 85, 86 },     /* sources unverified; RX IRQ is 58 */
};

/* PIT, XBAR1, ADC1/2 and ADC_ETC: the knob scan's trigger chain. */
#define SP404_PIT_BASE          0x40084000
#define SP404_PIT_IRQ           122
#define SP404_XBAR1_BASE        0x403bc000
#define SP404_XBAR_IN_PIT0      56      /* PIT_TRIGGER0-3 */
#define SP404_XBAR_OUT_ETC0     103     /* ADC_ETC_XBAR0_TRIG0-3, XBAR1_TRIG0-3 */
#define SP404_ADC1_BASE         0x400c4000
#define SP404_ADC2_BASE         0x400c8000
#define SP404_ADC_ETC_BASE      0x403b0000
static const int sp404_adc_irq[6] = { 67, 68, 118, 119, 120, 121 };

/*
 * The analog inputs go through a multiplexer whose address is GPIO2
 * pins 23-25, stepped by the firmware between scans.
 */
#define SP404_MUX_GPIO          1       /* GPIO2 */
#define SP404_MUX_SHIFT         23

/* uSDHC1-2: base, IRQ. uSDHC1 is the SD card slot (A:), uSDHC2 the eMMC (B:). */
static const struct {
    hwaddr base;
    int irq;
} sp404_usdhc[] = {
    { 0x402c0000, 110 }, { 0x402c4000, 111 },
};

/* The OLED: an SSD1309 on LPSPI4 CS0, D/C on GPIO2 pin 1, RES# on pin 10. */
#define SP404_OLED_LPSPI        3
#define SP404_OLED_GPIO         1       /* GPIO2 */
#define SP404_OLED_DC_PIN       1
#define SP404_OLED_RES_PIN      10

/* LPSPI1-4: base, IRQ, DMAMUX request sources for RX and TX. */
static const struct {
    hwaddr base;
    int irq, rx_src, tx_src;
} sp404_lpspi[] = {
    { 0x40394000, 32, 13, 14 },     /* sources unverified */
    { 0x40398000, 33, 77, 78 },     /* sources unverified */
    { 0x4039c000, 34, 15, 16 },     /* sources unverified */
    { 0x403a0000, 35, 79, 80 },     /* as the firmware programs DMAMUX */
};

/*
 * Button matrix: rows driven on GPIO2 pins 23-25 (shared with the analog
 * mux address), columns read active-low on GPIO2 pins 18-22, 26 and 28
 * (FUN_80046e90 scans it from the ADC_ETC interrupt).
 */
#define SP404_KEY_GPIO          1       /* GPIO2 */
#define SP404_KEY_COLUMNS       0x147c0000u
/* The GPIO2 bit of each column, in the order the firmware packs them. */
static const int sp404_key_col_bit[7] = { 20, 21, 22, 28, 26, 18, 19 };

/*
 * Pad levels at power-on, per bank. GPIO1 pin 0 is waited on high early in
 * init (FUN_80000b08) before anything else is brought up: taken to be a
 * power-good or companion-ready line, and held high. The key matrix
 * columns idle high (no button pressed).
 */
static const uint32_t sp404_gpio_reset_in[] = {
    0x00000001, SP404_KEY_COLUMNS, 0, 0, 0
};

/*
 * Bits the firmware polls for, forced on reads of stubbed registers.
 * Each entry names the register and why.
 */
static const SP404StubBits sp404_stub_bits[] = {
    /* CCM_ANALOG PLLs: LOCK. The SDK's clock setup waits on each. */
    { 0x400d8000, 1u << 31 },   /* PLL_ARM */
    { 0x400d8010, 1u << 31 },   /* PLL_USB1 */
    { 0x400d8020, 1u << 31 },   /* PLL_USB2 */
    { 0x400d8030, 1u << 31 },   /* PLL_SYS */
    { 0x400d8070, 1u << 31 },   /* PLL_AUDIO */
    { 0x400d80a0, 1u << 31 },   /* PLL_VIDEO */
    { 0x400d80e0, 1u << 31 },   /* PLL_ENET */
    /* PMU regulators: OK_VDDxPx. */
    { 0x400d8110, 1u << 17 },   /* REG_1P1 */
    { 0x400d8120, 1u << 17 },   /* REG_3P0 */
    { 0x400d8130, 1u << 17 },   /* REG_2P5 */
    { 0x400d8150, 1u << 15 },   /* MISC0: OSC_XTALOK */
    { 0x400d8180, 1u << 2 },    /* TEMPMON TEMPSENSE0: FINISHED */
    { 0x400d8270, 1u << 16 },   /* XTALOSC24M LOWPWR_CTRL: XTALOSC_PWRUP_STAT */
    { 0x40080000, 1u << 31 },   /* DCDC REG0: STS_DC_OK */
    /* USB OTG1/2 USBCMD: RST completes at once. Nothing is ever attached. */
    { 0x402e0140, 0, 1u << 1 },
    { 0x402e0340, 0, 1u << 1 },
    { 0 }
};

/* Blocks whose registers come with SET/CLR/TOG aliases. */
static const SP404StubRange sp404_stub_sct[] = {
    { 0x400d8000, 0x400d9000 }, /* CCM_ANALOG, PMU, TEMPMON, XTALOSC24M */
    { 0x400d9000, 0x400db000 }, /* USBPHY1, USBPHY2 */
    { 0 }
};

typedef struct SP404Machine {
    MachineState parent_obj;
    ARMv7MState armv7m;
    SP404RegStub periph;
    IMXRTFlexSPI flexspi;
    IMXRTGPIO gpio[5];
    IMXRTEDMA edma;
    IMXRTLPSPI lpspi[4];
    IMXRTLPUART lpuart[8];
    SP404BMC bmc;
    DeviceState *oled;
    SDHCIState usdhc[2];
    IMXRTSAI sai[3];
    IMXRTPIT pit;
    IMXRTXBAR xbar;
    IMXRTADC adc;
    /* Analog levels (12-bit) by ADC, channel and mux address. */
    uint16_t analog[2][16][8];
    SP404Audio audio;
    MemoryRegion itcm, dtcm, ocram, bootrom, flash, sdram;
    Clock *sysclk, *refclk;
    char *flash_file;
    char *link_id;
    SP404Link link;
    uint8_t keys[8];            /* pressed columns, by matrix row */
    int flash_fd;
} SP404Machine;

#define TYPE_SP404_MACHINE MACHINE_TYPE_NAME("sp404mk2")
OBJECT_DECLARE_SIMPLE_TYPE(SP404Machine, SP404_MACHINE)

static void sp404_ram(MemoryRegion *mr, const char *name, hwaddr base,
                      uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void sp404_flash_sync(void *opaque, uint32_t offset, uint32_t len)
{
    SP404Machine *m = opaque;
    uint8_t *flash = memory_region_get_ram_ptr(&m->flash);

    if (m->flash_fd < 0) {
        return;
    }
    if (lseek(m->flash_fd, offset, SEEK_SET) != offset ||
        write(m->flash_fd, flash + offset, len) != len) {
        warn_report("sp404mk2: could not write flash file: %s",
                    strerror(errno));
    }
}

/*
 * Fill the NOR: from the flash file if there is one, else erased, then
 * APP1 from -bios over its area. Returns the APP1 vector table.
 */
static void sp404_load_flash(SP404Machine *m, uint32_t vectors[2])
{
    MachineState *machine = MACHINE(m);
    uint8_t *flash = memory_region_get_ram_ptr(&m->flash);
    g_autofree gchar *data = NULL;
    gsize len = 0;
    bool had_file = false;

    memset(flash, 0xff, SP404_FLEXSPI_SIZE);
    m->flash_fd = -1;
    if (m->flash_file) {
        m->flash_fd = open(m->flash_file, O_RDWR | O_CREAT | O_BINARY, 0644);
        if (m->flash_fd < 0) {
            error_report("sp404mk2: cannot open flash file '%s': %s",
                         m->flash_file, strerror(errno));
            exit(1);
        }
        had_file = read(m->flash_fd, flash, SP404_FLEXSPI_SIZE) ==
                   SP404_FLEXSPI_SIZE;
        if (!had_file) {
            memset(flash, 0xff, SP404_FLEXSPI_SIZE);
        }
    }
    if (machine->firmware) {
        if (!g_file_get_contents(machine->firmware, &data, &len, NULL)) {
            error_report("sp404mk2: could not read '%s'", machine->firmware);
            exit(1);
        }
        if (len > SP404_FLEXSPI_SIZE - SP404_APP1_FLASH_OFFSET) {
            error_report("sp404mk2: '%s' is too big for APP1",
                         machine->firmware);
            exit(1);
        }
        memcpy(flash + SP404_APP1_FLASH_OFFSET, data, len);
    } else if (!had_file) {
        error_report("sp404mk2: give Roland's SP404MKII_APP1.bin with -bios, "
                     "or an existing flash image with -M sp404mk2,flash=FILE");
        exit(1);
    }
    if (!had_file) {
        sp404_flash_sync(m, 0, SP404_FLEXSPI_SIZE);
    } else if (machine->firmware) {
        sp404_flash_sync(m, SP404_APP1_FLASH_OFFSET, len);
    }
    memcpy(vectors, flash + SP404_APP1_FLASH_OFFSET + APP1_VECTORS_OFFSET, 8);
    vectors[0] = le32_to_cpu(vectors[0]);
    vectors[1] = le32_to_cpu(vectors[1]);
}

/* The key matrix: the row being scanned is the mux address on GPIO2. */
static uint32_t sp404_key_inputs(void *opaque, uint32_t in)
{
    SP404Machine *m = opaque;
    IMXRTGPIO *g = &m->gpio[SP404_KEY_GPIO];
    unsigned row = ((g->dr & g->gdir) >> SP404_MUX_SHIFT) & 7;

    in |= SP404_KEY_COLUMNS;
    for (int c = 0; c < 7; c++) {
        if (m->keys[row] & (1u << c)) {
            in &= ~(1u << sp404_key_col_bit[c]);
        }
    }
    return in;
}

static void sp404_link_key(void *opaque, int row, int col, bool pressed)
{
    SP404Machine *m = opaque;

    if (row >= 0 && row < 8 && col >= 0 && col < 7) {
        m->keys[row] = deposit32(m->keys[row], col, 1, pressed);
    }
}

static void sp404_link_knob(void *opaque, int adc, int ch, int mux,
                            uint16_t value)
{
    SP404Machine *m = opaque;

    if (adc >= 0 && adc < 2 && ch >= 0 && ch < 16 && mux >= 0 && mux < 8) {
        m->analog[adc][ch][mux] = value & 0xfff;
    }
}

static void sp404_link_bmc(void *opaque, const uint8_t *pkt)
{
    SP404Machine *m = opaque;

    sp404_bmc_inject(&m->bmc, pkt);
}

static uint16_t sp404_analog_sample(void *opaque, int adc, int ch)
{
    SP404Machine *m = opaque;
    IMXRTGPIO *g = &m->gpio[SP404_MUX_GPIO];
    unsigned mux = ((g->dr & g->gdir) >> SP404_MUX_SHIFT) & 7;

    return m->analog[adc][ch & 15][mux];
}

static void sp404_init(MachineState *machine)
{
    SP404Machine *m = SP404_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    hwaddr app1 = SP404_FLEXSPI_BASE + SP404_APP1_FLASH_OFFSET;
    uint32_t vectors[2];
    DeviceState *dev;

    m->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(m->sysclk, SP404_CPU_HZ);
    m->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(m->refclk, 100000);

    sp404_ram(&m->itcm, "sp404.itcm", SP404_ITCM_BASE, SP404_ITCM_SIZE);
    sp404_ram(&m->dtcm, "sp404.dtcm", SP404_DTCM_BASE, SP404_DTCM_SIZE);
    sp404_ram(&m->ocram, "sp404.ocram", SP404_OCRAM_BASE, SP404_OCRAM_SIZE);
    sp404_ram(&m->sdram, "sp404.sdram", SP404_SDRAM_BASE, SP404_SDRAM_SIZE);

    memory_region_init_rom(&m->bootrom, NULL, "sp404.bootrom",
                           SP404_BOOTROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SP404_BOOTROM_BASE, &m->bootrom);
    memory_region_init_rom(&m->flash, NULL, "sp404.flexspi",
                           SP404_FLEXSPI_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SP404_FLEXSPI_BASE, &m->flash);

    object_initialize_child(OBJECT(machine), "periph", &m->periph,
                            TYPE_SP404_REGSTUB);
    dev = DEVICE(&m->periph);
    qdev_prop_set_string(dev, "name", "sp404.periph");
    qdev_prop_set_uint64(dev, "base", SP404_PERIPH_BASE);
    qdev_prop_set_uint64(dev, "size", SP404_PERIPH_SIZE);
    m->periph.bits = sp404_stub_bits;
    m->periph.sct = sp404_stub_sct;
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, SP404_PERIPH_BASE, -1000);

    object_initialize_child(OBJECT(machine), "armv7m", &m->armv7m,
                            TYPE_ARMV7M);
    dev = DEVICE(&m->armv7m);
    qdev_prop_set_uint32(dev, "num-irq", SP404_NUM_IRQ);
    qdev_prop_set_uint8(dev, "num-prio-bits", 4);
    qdev_prop_set_uint32(dev, "mpu-ns-regions", 16);
    qdev_prop_set_string(dev, "cpu-type", machine->cpu_type);
    qdev_connect_clock_in(dev, "cpuclk", m->sysclk);
    qdev_connect_clock_in(dev, "refclk", m->refclk);
    object_property_set_link(OBJECT(dev), "memory", OBJECT(sysmem),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    object_initialize_child(OBJECT(machine), "flexspi", &m->flexspi,
                            TYPE_IMXRT_FLEXSPI);
    object_property_set_link(OBJECT(&m->flexspi), "flash", OBJECT(&m->flash),
                             &error_abort);
    m->flexspi.flash_dirty = sp404_flash_sync;
    m->flexspi.flash_dirty_opaque = m;
    sysbus_realize(SYS_BUS_DEVICE(&m->flexspi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->flexspi), 0, SP404_FLEXSPI_REG_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->flexspi), 0,
                       qdev_get_gpio_in(DEVICE(&m->armv7m),
                                        SP404_FLEXSPI_IRQ));

    for (int i = 0; i < ARRAY_SIZE(m->gpio); i++) {
        DeviceState *cpu = DEVICE(&m->armv7m);
        SysBusDevice *sbd;

        object_initialize_child(OBJECT(machine), "gpio[*]", &m->gpio[i],
                                TYPE_IMXRT_GPIO);
        sbd = SYS_BUS_DEVICE(&m->gpio[i]);
        qdev_prop_set_uint32(DEVICE(sbd), "reset-in", sp404_gpio_reset_in[i]);
        {
            g_autofree char *name = g_strdup_printf("gpio%d", i + 1);
            qdev_prop_set_string(DEVICE(sbd), "name", name);
        }
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, sp404_gpio[i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(cpu, sp404_gpio[i].irq));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(cpu,
                                                    sp404_gpio[i].irq + 1));
        if (i == 0) {
            for (int p = 0; p < 8; p++) {
                sysbus_connect_irq(sbd, 2 + p, qdev_get_gpio_in(cpu,
                                   SP404_GPIO1_INT0_IRQ + p));
            }
        }
    }

    object_initialize_child(OBJECT(machine), "edma", &m->edma,
                            TYPE_IMXRT_EDMA);
    sysbus_realize(SYS_BUS_DEVICE(&m->edma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->edma), 0, SP404_EDMA_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->edma), 1, SP404_DMAMUX_BASE);
    for (int i = 0; i < 16; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->edma), i,
                           qdev_get_gpio_in(DEVICE(&m->armv7m), i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->edma), 16,
                       qdev_get_gpio_in(DEVICE(&m->armv7m),
                                        SP404_EDMA_ERROR_IRQ));

    for (int i = 0; i < ARRAY_SIZE(m->lpspi); i++) {
        SysBusDevice *sbd;

        object_initialize_child(OBJECT(machine), "lpspi[*]", &m->lpspi[i],
                                TYPE_IMXRT_LPSPI);
        sbd = SYS_BUS_DEVICE(&m->lpspi[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, sp404_lpspi[i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&m->armv7m),
                                                    sp404_lpspi[i].irq));
        qdev_connect_gpio_out_named(DEVICE(sbd), "rx-dreq", 0,
            qdev_get_gpio_in_named(DEVICE(&m->edma), "dreq",
                                   sp404_lpspi[i].rx_src));
        qdev_connect_gpio_out_named(DEVICE(sbd), "tx-dreq", 0,
            qdev_get_gpio_in_named(DEVICE(&m->edma), "dreq",
                                   sp404_lpspi[i].tx_src));
    }

    for (int i = 0; i < ARRAY_SIZE(m->lpuart); i++) {
        SysBusDevice *sbd;
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);

        object_initialize_child(OBJECT(machine), "lpuart[*]", &m->lpuart[i],
                                TYPE_IMXRT_LPUART);
        sbd = SYS_BUS_DEVICE(&m->lpuart[i]);
        qdev_prop_set_string(DEVICE(sbd), "name", name);
        if (i == 0) {
            /* A debug console, if the firmware has one, would be here. */
            qdev_prop_set_chr(DEVICE(sbd), "chardev", serial_hd(0));
        }
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, sp404_lpuart_base[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&m->armv7m),
                                                    SP404_LPUART1_IRQ + i));
    }
    sp404_bmc_init(&m->bmc, &m->lpuart[SP404_BMC_LPUART]);

    m->oled = ssi_create_peripheral(m->lpspi[SP404_OLED_LPSPI].bus,
                                    TYPE_SSD1309);
    qdev_connect_gpio_out_named(DEVICE(&m->lpspi[SP404_OLED_LPSPI]), "cs", 0,
                                qdev_get_gpio_in_named(m->oled, SSI_GPIO_CS,
                                                       0));
    qdev_connect_gpio_out_named(DEVICE(&m->gpio[SP404_OLED_GPIO]), "out",
                                SP404_OLED_DC_PIN,
                                qdev_get_gpio_in(m->oled, 0));
    qdev_connect_gpio_out_named(DEVICE(&m->gpio[SP404_OLED_GPIO]), "out",
                                SP404_OLED_RES_PIN,
                                qdev_get_gpio_in(m->oled, 1));

    for (int i = 0; i < ARRAY_SIZE(m->usdhc); i++) {
        SysBusDevice *sbd;
        DriveInfo *di = drive_get(IF_SD, 0, i);

        object_initialize_child(OBJECT(machine), "usdhc[*]", &m->usdhc[i],
                                TYPE_IMX_USDHC);
        sbd = SYS_BUS_DEVICE(&m->usdhc[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, sp404_usdhc[i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&m->armv7m),
                                                    sp404_usdhc[i].irq));
        if (di) {
            DeviceState *card = qdev_new(i == 1 ? TYPE_EMMC : TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                    &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(DEVICE(sbd),
                                                            "sd-bus"),
                                   &error_fatal);
        }
    }

    for (int i = 0; i < ARRAY_SIZE(m->sai); i++) {
        SysBusDevice *sbd;
        g_autofree char *name = g_strdup_printf("sai%d", i + 1);

        object_initialize_child(OBJECT(machine), "sai[*]", &m->sai[i],
                                TYPE_IMXRT_SAI);
        sbd = SYS_BUS_DEVICE(&m->sai[i]);
        qdev_prop_set_string(DEVICE(sbd), "name", name);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, sp404_sai[i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&m->armv7m),
                                                    sp404_sai[i].irq));
        qdev_connect_gpio_out_named(DEVICE(sbd), "rx-dreq", 0,
            qdev_get_gpio_in_named(DEVICE(&m->edma), "dreq",
                                   sp404_sai[i].rx_src));
        qdev_connect_gpio_out_named(DEVICE(sbd), "tx-dreq", 0,
            qdev_get_gpio_in_named(DEVICE(&m->edma), "dreq",
                                   sp404_sai[i].tx_src));
    }
    sp404_audio_init(&m->audio, &m->sai[0]);

    object_initialize_child(OBJECT(machine), "pit", &m->pit, TYPE_IMXRT_PIT);
    sysbus_realize(SYS_BUS_DEVICE(&m->pit), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->pit), 0, SP404_PIT_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->pit), 0,
                       qdev_get_gpio_in(DEVICE(&m->armv7m), SP404_PIT_IRQ));

    object_initialize_child(OBJECT(machine), "xbar1", &m->xbar,
                            TYPE_IMXRT_XBAR);
    sysbus_realize(SYS_BUS_DEVICE(&m->xbar), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->xbar), 0, SP404_XBAR1_BASE);

    object_initialize_child(OBJECT(machine), "adc", &m->adc, TYPE_IMXRT_ADC);
    m->adc.sample = sp404_analog_sample;
    m->adc.sample_opaque = m;
    for (int a = 0; a < 2; a++) {
        for (int c = 0; c < 16; c++) {
            for (int x = 0; x < 8; x++) {
                m->analog[a][c][x] = 2048;
            }
        }
    }
    sysbus_realize(SYS_BUS_DEVICE(&m->adc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->adc), 0, SP404_ADC1_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->adc), 1, SP404_ADC2_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&m->adc), 2, SP404_ADC_ETC_BASE);
    for (int i = 0; i < 6; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->adc), i,
                           qdev_get_gpio_in(DEVICE(&m->armv7m),
                                            sp404_adc_irq[i]));
    }
    for (int i = 0; i < 4; i++) {
        qdev_connect_gpio_out_named(DEVICE(&m->pit), "trigger", i,
            qdev_get_gpio_in_named(DEVICE(&m->xbar), "in",
                                   SP404_XBAR_IN_PIT0 + i));
    }
    for (int i = 0; i < 8; i++) {
        qdev_connect_gpio_out_named(DEVICE(&m->xbar), "out",
                                    SP404_XBAR_OUT_ETC0 + i,
            qdev_get_gpio_in_named(DEVICE(&m->adc), "trig", i));
    }

    m->gpio[SP404_KEY_GPIO].in_hook = sp404_key_inputs;
    m->gpio[SP404_KEY_GPIO].in_hook_opaque = m;

    if (m->link_id) {
        Chardev *chr = qemu_chr_find(m->link_id);

        if (!chr) {
            error_report("sp404mk2: no chardev '%s' for link", m->link_id);
            exit(1);
        }
        m->link.opaque = m;
        m->link.key = sp404_link_key;
        m->link.knob = sp404_link_knob;
        m->link.bmc_rx = sp404_link_bmc;
        sp404_link_init(&m->link, chr, SSD1309(m->oled));
        m->bmc.link = &m->link;
        m->audio.out = sp404_link_audio;
        m->audio.out_opaque = &m->link;
    }

    sp404_load_flash(m, vectors);
    if (vectors[1] - app1 >= SP404_FLEXSPI_SIZE - SP404_APP1_FLASH_OFFSET ||
        !(vectors[1] & 1)) {
        error_report("sp404mk2: the flash holds no SP-404MKII application "
                     "(reset vector 0x%08x)", vectors[1]);
        exit(1);
    }
    /*
     * The boot ROM would jump to the entry; here the CPU's own reset reads
     * SP and PC from address 0, which is ITCM, so put them there. The
     * application's scatter load then copies its real vectors over them.
     */
    vectors[0] = cpu_to_le32(vectors[0]);
    vectors[1] = cpu_to_le32(vectors[1]);
    rom_add_blob_fixed("sp404.reset-vectors", vectors, sizeof(vectors),
                       SP404_ITCM_BASE);
    /* No kernel: this only registers the CPU reset, run after ROM loading. */
    armv7m_load_kernel(m->armv7m.cpu, NULL, 0, 0);
}

static char *sp404_get_flash(Object *obj, Error **errp)
{
    return g_strdup(SP404_MACHINE(obj)->flash_file);
}

static void sp404_set_flash(Object *obj, const char *value, Error **errp)
{
    SP404Machine *m = SP404_MACHINE(obj);

    g_free(m->flash_file);
    m->flash_file = g_strdup(value);
}

static char *sp404_get_link(Object *obj, Error **errp)
{
    return g_strdup(SP404_MACHINE(obj)->link_id);
}

static void sp404_set_link(Object *obj, const char *value, Error **errp)
{
    SP404Machine *m = SP404_MACHINE(obj);

    g_free(m->link_id);
    m->link_id = g_strdup(value);
}

static void sp404_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m7"),
        NULL
    };

    mc->desc = "Roland SP-404MKII (NXP i.MX RT1060, Cortex-M7)";
    mc->init = sp404_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m7");
    mc->valid_cpu_types = valid_cpu_types;
    mc->ignore_memory_transaction_failures = false;
    object_class_property_add_str(oc, "flash", sp404_get_flash,
                                  sp404_set_flash);
    object_class_property_set_description(oc, "flash",
        "File backing the 4 MB QSPI NOR (created if missing)");
    object_class_property_add_str(oc, "link", sp404_get_link, sp404_set_link);
    object_class_property_set_description(oc, "link",
        "Chardev id of the frontend link (panel, screen, audio)");
}

static const TypeInfo sp404_machine_info = {
    .name = TYPE_SP404_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SP404Machine),
    .class_init = sp404_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void sp404_machine_register(void)
{
    type_register_static(&sp404_machine_info);
}

type_init(sp404_machine_register)
