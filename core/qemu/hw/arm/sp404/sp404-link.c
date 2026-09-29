/*
 * SP-404MKII frontend link: the emulated unit's panel, screen and audio
 * over a chardev (normally a TCP socket to the frontend app).
 *
 * Every message is a 4-byte header, type (u8), 0 (u8), payload length
 * (u16 LE), then the payload.
 *
 * Emulator to frontend:
 *   0x01 DISPLAY  1024 bytes: 128x64, one bit per pixel, rows top to
 *                 bottom, 16 bytes a row, MSB leftmost. Sent when the
 *                 picture changes, at most every 20 ms.
 *   0x02 AUDIO    stereo 16-bit little-endian frames at 48 kHz.
 *   0x03 BMC      4-byte packets the firmware sent the BMC (LEDs, MIDI).
 *
 * Frontend to emulator:
 *   0x81 KEY      row (u8, 0-7), column (u8, 0-6), pressed (u8)
 *   0x82 KNOB     adc (u8), channel (u8), mux (u8), value (u16 LE, 0-4095)
 *   0x83 BMC      a 4-byte packet, as if from the BMC (SHIFT)
 *   0x84 ENCODER  detents to turn the VALUE encoder (s8, + clockwise)
 *   0x85 AUDIO    input audio, stereo 16-bit LE frames at 48 kHz
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/arm/sp404/sp404.h"

#define DISPLAY_PERIOD_NS   (20 * 1000 * 1000)
#define AUDIO_BLOCK         64

static void link_send(SP404Link *l, uint8_t type, const void *data,
                      uint16_t len)
{
    uint8_t hdr[4] = { type, 0, len & 0xff, len >> 8 };

    if (!l->connected) {
        return;
    }
    qemu_chr_fe_write_all(&l->chr, hdr, sizeof(hdr));
    if (len) {
        qemu_chr_fe_write_all(&l->chr, data, len);
    }
}

static void link_display_tick(void *opaque)
{
    SP404Link *l = opaque;
    uint8_t img[SSD1309_WIDTH * SSD1309_HEIGHT / 8] = {};

    for (int y = 0; y < SSD1309_HEIGHT; y++) {
        for (int x = 0; x < SSD1309_WIDTH; x++) {
            if (ssd1309_pixel(l->oled, x, y)) {
                img[y * 16 + x / 8] |= 0x80 >> (x & 7);
            }
        }
    }
    if (memcmp(img, l->last_img, sizeof(img)) || l->resend) {
        memcpy(l->last_img, img, sizeof(img));
        l->resend = false;
        link_send(l, 0x01, img, sizeof(img));
    }
    timer_mod(l->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              DISPLAY_PERIOD_NS);
}

void sp404_link_audio(void *opaque, const int16_t *lr, int frames)
{
    SP404Link *l = opaque;

    for (int i = 0; i < frames; i++) {
        stw_le_p(&l->audio[l->audio_len * 4], lr[2 * i]);
        stw_le_p(&l->audio[l->audio_len * 4 + 2], lr[2 * i + 1]);
        if (++l->audio_len == AUDIO_BLOCK) {
            link_send(l, 0x02, l->audio, AUDIO_BLOCK * 4);
            l->audio_len = 0;
        }
    }
}

void sp404_link_bmc_tx(SP404Link *l, const uint8_t *pkt)
{
    link_send(l, 0x03, pkt, 4);
}

static void link_message(SP404Link *l, uint8_t type, const uint8_t *p,
                         uint16_t len)
{
    switch (type) {
    case 0x81:
        if (len >= 3 && l->key) {
            l->key(l->opaque, p[0], p[1], p[2]);
        }
        break;
    case 0x82:
        if (len >= 5 && l->knob) {
            l->knob(l->opaque, p[0], p[1], p[2], lduw_le_p(p + 3));
        }
        break;
    case 0x83:
        if (len >= 4 && l->bmc_rx) {
            l->bmc_rx(l->opaque, p);
        }
        break;
    case 0x85:
        if (l->audio_in) {
            int16_t lr[256 * 2];
            int frames = MIN(len / 4, 256);

            for (int i = 0; i < frames * 2; i++) {
                lr[i] = (int16_t)lduw_le_p(p + i * 2);
            }
            l->audio_in(l->opaque, lr, frames);
        }
        break;
    case 0x84:
        if (len >= 1 && l->encoder) {
            l->encoder(l->opaque, (int8_t)p[0]);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sp404-link: message type 0x%02x\n",
                      type);
    }
}

static int link_can_receive(void *opaque)
{
    return 4096;
}

static void link_receive(void *opaque, const uint8_t *buf, int size)
{
    SP404Link *l = opaque;

    for (int i = 0; i < size; i++) {
        l->rx[l->rx_len++] = buf[i];
        if (l->rx_len >= 4) {
            uint16_t len = l->rx[2] | (l->rx[3] << 8);

            if (len > sizeof(l->rx) - 4) {
                l->rx_len = 0;          /* garbage: resynchronise */
                continue;
            }
            if (l->rx_len == 4 + len) {
                link_message(l, l->rx[0], l->rx + 4, len);
                l->rx_len = 0;
            }
        }
    }
}

static void link_event(void *opaque, QEMUChrEvent event)
{
    SP404Link *l = opaque;

    if (event == CHR_EVENT_OPENED) {
        l->connected = true;
        l->resend = true;
        l->rx_len = 0;
    } else if (event == CHR_EVENT_CLOSED) {
        l->connected = false;
    }
}

void sp404_link_init(SP404Link *l, Chardev *chr, SSD1309State *oled)
{
    l->oled = oled;
    qemu_chr_fe_init(&l->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&l->chr, link_can_receive, link_receive,
                             link_event, NULL, l, NULL, true);
    l->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, link_display_tick, l);
    timer_mod(l->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              DISPLAY_PERIOD_NS);
}
