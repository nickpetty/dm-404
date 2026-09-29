/*
 * SSD1309 128x64 monochrome OLED controller on 4-wire SPI, as used for the
 * SP-404MKII's display.
 *
 * GPIO input 0 is D/C (high: data), input 1 is RES# (low holds it in
 * reset). Horizontal, vertical and page addressing are modelled, with the
 * segment/COM remaps, start line, inverse, entire-display-on and display
 * on/off. Contrast is kept for the frontend but not applied here.
 *
 * The panel is shown on a graphic console, scaled up, so the monitor's
 * screendump works even headless; sp404_display_* hands the raw 1-bit
 * image to the frontend link.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/ssi.h"
#include "hw/core/irq.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/arm/sp404/sp404.h"

#define W       SSD1309_WIDTH
#define H       SSD1309_HEIGHT
#define PAGES   (H / 8)
#define SCALE   4

/* Arguments each multi-byte command takes after its first byte. */
static int ssd1309_nargs(uint8_t cmd)
{
    switch (cmd) {
    case 0x20: case 0x81: case 0x8d: case 0xa8: case 0xd3: case 0xd5:
    case 0xd9: case 0xda: case 0xdb: case 0xfd: case 0xad: case 0xd8:
        return 1;
    case 0x21: case 0x22: case 0xa3:
        return 2;
    case 0x29: case 0x2a:
        return 5;
    case 0x26: case 0x27: case 0x2c: case 0x2d:
        return 6;
    default:
        return 0;
    }
}

static void ssd1309_reset_state(SSD1309State *s)
{
    s->mode = 2;                /* page addressing */
    s->col = s->page = 0;
    s->col_start = 0;
    s->col_end = W - 1;
    s->page_start = 0;
    s->page_end = PAGES - 1;
    s->start_line = 0;
    s->seg_remap = s->com_remap = false;
    s->invert = s->all_on = s->on = false;
    s->contrast = 0x7f;
    s->cmd_len = 0;
    s->redraw = true;
}

static void ssd1309_command(SSD1309State *s)
{
    uint8_t *c = s->cmd;

    switch (c[0]) {
    case 0x00 ... 0x0f:
        s->col = (s->col & 0xf0) | (c[0] & 0xf);
        break;
    case 0x10 ... 0x1f:
        s->col = (s->col & 0x0f) | ((c[0] & 0xf) << 4);
        break;
    case 0x20:
        s->mode = c[1] & 3;
        break;
    case 0x21:
        s->col_start = s->col = c[1] & 0x7f;
        s->col_end = c[2] & 0x7f;
        break;
    case 0x22:
        s->page_start = s->page = c[1] & 7;
        s->page_end = c[2] & 7;
        break;
    case 0x40 ... 0x7f:
        s->start_line = c[0] & 0x3f;
        break;
    case 0x81:
        s->contrast = c[1];
        break;
    case 0xa0: case 0xa1:
        s->seg_remap = c[0] & 1;
        break;
    case 0xa4: case 0xa5:
        s->all_on = c[0] & 1;
        break;
    case 0xa6: case 0xa7:
        s->invert = c[0] & 1;
        break;
    case 0xae: case 0xaf:
        s->on = c[0] & 1;
        break;
    case 0xb0 ... 0xb7:
        s->page = c[0] & 7;
        break;
    case 0xc0: case 0xc8:
        s->com_remap = c[0] & 8;
        break;
    default:
        break;                  /* timing, scrolling setup, lock: no effect */
    }
    SP404_TRACE("oled", "cmd %02x (%d args)", c[0], s->cmd_len - 1);
    s->redraw = true;
}

static void ssd1309_data(SSD1309State *s, uint8_t v)
{
    s->ram[s->page * W + s->col] = v;
    s->redraw = true;
    switch (s->mode) {
    case 0:                     /* horizontal */
        if (s->col++ >= s->col_end) {
            s->col = s->col_start;
            if (s->page++ >= s->page_end) {
                s->page = s->page_start;
            }
        }
        break;
    case 1:                     /* vertical */
        if (s->page++ >= s->page_end) {
            s->page = s->page_start;
            if (s->col++ >= s->col_end) {
                s->col = s->col_start;
            }
        }
        break;
    default:                    /* page: the column wraps within the page */
        s->col = (s->col + 1) & (W - 1);
        break;
    }
    if (s->col >= W) {
        s->col = 0;
    }
}

static uint32_t ssd1309_transfer(SSIPeripheral *dev, uint32_t data)
{
    SSD1309State *s = SSD1309(dev);

    if (s->in_reset) {
        return 0;
    }
    if (s->dc) {
        ssd1309_data(s, data);
        return 0;
    }
    s->cmd[s->cmd_len++] = data;
    if (s->cmd_len > ssd1309_nargs(s->cmd[0])) {
        ssd1309_command(s);
        s->cmd_len = 0;
    }
    return 0;
}

static void ssd1309_gpio(void *opaque, int n, int level)
{
    SSD1309State *s = opaque;

    if (n == 0) {
        s->dc = level;
    } else {
        s->in_reset = !level;
        if (s->in_reset) {
            ssd1309_reset_state(s);
        }
    }
}

bool ssd1309_pixel(SSD1309State *s, int x, int y)
{
    int col = s->seg_remap ? x : W - 1 - x;
    int row = s->com_remap ? y : H - 1 - y;
    int line = (row + s->start_line) & (H - 1);
    bool on;

    if (!s->on) {
        return false;
    }
    on = s->all_on || ((s->ram[(line / 8) * W + col] >> (line & 7)) & 1);
    return on ^ s->invert;
}

static bool ssd1309_update_display(void *opaque)
{
    SSD1309State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t fg = rgb_to_pixel32(0xe8, 0xf0, 0xff), bg = 0;
    uint8_t *dest;
    int x, y, i, j, stride;

    if (!s->redraw || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    dest = surface_data(surface);
    stride = surface_stride(surface);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t px = ssd1309_pixel(s, x, y) ? fg : bg;

            for (j = 0; j < SCALE; j++) {
                uint32_t *d = (uint32_t *)(dest + (y * SCALE + j) * stride) +
                              x * SCALE;
                for (i = 0; i < SCALE; i++) {
                    d[i] = px;
                }
            }
        }
    }
    s->redraw = false;
    s->frames++;
    qemu_console_update(s->con, 0, 0, W * SCALE, H * SCALE);
    return true;
}

static void ssd1309_invalidate(void *opaque)
{
    SSD1309State *s = opaque;

    s->redraw = true;
}

static const GraphicHwOps ssd1309_ops = {
    .invalidate = ssd1309_invalidate,
    .gfx_update = ssd1309_update_display,
};

static void ssd1309_realize(SSIPeripheral *d, Error **errp)
{
    DeviceState *dev = DEVICE(d);
    SSD1309State *s = SSD1309(d);

    ssd1309_reset_state(s);
    memset(s->ram, 0, sizeof(s->ram));
    s->con = qemu_graphic_console_create(dev, 0, &ssd1309_ops, s);
    qemu_console_resize(s->con, W * SCALE, H * SCALE);
    qdev_init_gpio_in(dev, ssd1309_gpio, 2);
}

static void ssd1309_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = ssd1309_realize;
    k->transfer = ssd1309_transfer;
    k->cs_polarity = SSI_CS_LOW;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ssd1309_info = {
    .name = TYPE_SSD1309,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SSD1309State),
    .class_init = ssd1309_class_init,
};

static void ssd1309_register_types(void)
{
    type_register_static(&ssd1309_info);
}

type_init(ssd1309_register_types)
