/*
 * SP-404MKII BMC, the companion microcontroller on LPUART3.
 *
 * On the real unit it owns USB audio/MIDI and holds the i.MX's boot mode
 * lines. It and the application exchange four-byte packets framed as
 * USB-MIDI event packets: byte 0 is cable << 4 | code index number, and the
 * application dispatches on the CIN (the BMC task at 0x800eba9a, table at
 * 0x800ebc00). CIN 0 and 1, reserved in USB-MIDI, carry the BMC's own
 * system messages, "xx FF cmd arg"; the rest is MIDI.
 *
 * What the application does with system messages from the BMC (CIN 0, the
 * handler at 0x800665a8):
 *   00 FF 00 01        go to page 3, the blank power-off page
 *                      (FUN_800da598(3)); never sent at boot
 *   00 FF 00 10..13    USB state
 *   00 FF 04 nn        answer to "01 FF 05 01": records nn and signals the
 *                      semaphore main() waits on before bringing up the UI
 *   00 FF 10..16 cc    a 7-character string, one character each
 *   00 FF 20..26 cc    another 7-character string
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/sp404/sp404.h"

static void bmc_send(SP404BMC *bmc, uint8_t b0, uint8_t b1, uint8_t b2,
                     uint8_t b3)
{
    uint8_t p[4] = { b0, b1, b2, b3 };

    SP404_TRACE("bmc", "to i.MX:   %02x %02x %02x %02x", b0, b1, b2, b3);
    imxrt_lpuart_receive(bmc->uart, p, sizeof(p));
}

void sp404_bmc_inject(SP404BMC *bmc, const uint8_t *pkt)
{
    bmc_send(bmc, pkt[0], pkt[1], pkt[2], pkt[3]);
}

static void bmc_packet(SP404BMC *bmc, const uint8_t *p)
{
    SP404_TRACE("bmc", "from i.MX: %02x %02x %02x %02x", p[0], p[1], p[2],
                p[3]);
    if (bmc->link) {
        sp404_link_bmc_tx(bmc->link, p);
    }
    if ((p[0] & 0xf) == 1 && p[1] == 0xfe && p[2] == 0x11) {
        /*
         * Asked at the end of project load (FUN_8013dbe8), which polls for
         * ten seconds for the answer "00 FF FE digit" and treats '4' as a
         * special mode. Normal is taken to be '0'.
         */
        bmc_send(bmc, 0x00, 0xff, 0xfe, '0');
        return;
    }
    if ((p[0] & 0xf) == 1 && p[1] == 0xff) {
        switch (p[2]) {
        case 0x00:
            if (p[3] == 0x01) {         /* the application's hello */
                /*
                 * Not answered with "00 FF 00 01": the firmware takes that
                 * as a request for page 3, a blank power-off screen
                 * (FUN_800da598(3) in the CIN 0 handler).
                 */
                /*
                 * "00 FF FF nn" is the SHIFT key (panel key 0x2A): 01 held,
                 * anything else released. Held at power-on means the
                 * updater, so report it released.
                 */
                bmc_send(bmc, 0x00, 0xff, 0xff, 0x00);
            }
            break;
        case 0x05:
            bmc_send(bmc, 0x00, 0xff, 0x04, 0x01);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "sp404-bmc: system message %02x %02x\n",
                          p[2], p[3]);
        }
    }
}

static void bmc_rx_byte(void *opaque, uint8_t byte)
{
    SP404BMC *bmc = opaque;

    bmc->pkt[bmc->pkt_len++] = byte;
    if (bmc->pkt_len == sizeof(bmc->pkt)) {
        bmc->pkt_len = 0;
        bmc_packet(bmc, bmc->pkt);
    }
}

void sp404_bmc_init(SP404BMC *bmc, IMXRTLPUART *uart)
{
    bmc->uart = uart;
    bmc->pkt_len = 0;
    uart->tx = bmc_rx_byte;
    uart->tx_opaque = bmc;
}
