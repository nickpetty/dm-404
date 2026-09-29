# doom-404: a Roland SP-404MKII emulator

Runs Roland's own SP-404MKII firmware (System Program 5.52) on an emulated
NXP i.MX RT1060 in a QEMU fork, with a hardware-lookalike JUCE frontend to
come. Named after Dr. Doom, not the game.

Roland's firmware is not in the repo: `firmware/` holds the user's copy of
the 5.52 system program (`SP404MKII_APP1.bin` sha256 `4a3d6771…0d80`,
`SP404MKII_APP0.bin`), git-ignored. Never commit it.

## Layout

- `core/qemu/` — everything we add to QEMU: `hw/arm/sp404/*.c` (machine and
  device models), `include/hw/arm/sp404/sp404.h`, and `patches/` for the few
  edits to upstream files. `third_party/qemu` is a plain clone of v11.1.2
  (git-ignored); `tools/qemu_sync.sh` applies the patches and copies our
  files over it, `tools/build.sh` does that and builds
  `build/qemu/qemu-system-arm.exe` (MSYS2 UCRT64, ninja).
- `tools/` — the debugging kit (below). `build/` — all outputs, ignored.

## The hardware, as established

i.MX RT1060, Cortex-M7 @ 600 MHz, micro T-Kernel 2.00.44, NXP MCUXpresso SDK
drivers. APP1 is flashed at QSPI 0x60080000; its Arm Compiler scatter table
unpacks code to ITCM (0x400), DTCM, OCRAM and SDRAM 0x80000000
(`tools/unpack.py`). The machine starts the CPU at APP1's own vectors
(offset 0x6ec), skipping the boot ROM.

| Block | Where | What it is on the SP |
|---|---|---|
| FlexSPI | 0x402A8000 | 4 MB NOR; flash backed by `build/flash.bin` |
| LPSPI4 CS0 | 0x403A0000 | SSD1309 128x64 OLED; D/C = GPIO2.1, RES# = GPIO2.10 |
| LPUART3 | 0x4018C000 | BMC link, 1 Mbaud, USB-MIDI-framed 4-byte packets |
| SAI1 | 0x40384000 | codec: TDM 16x32-bit slots, 4 TX lines, eDMA ch0-3 chained, RX ch4 |
| uSDHC1 / uSDHC2 | 0x402C0000 / 0x402C4000 | SD card (A:) / eMMC (B:, exFAT) |
| PIT ch0 → XBAR 56→103 → ADC_ETC trig0 → ADC1 ch 4,5,6,3 | | knob scan, 2 kHz, through a mux addressed by GPIO2.23-25 |
| GPIO2 18-22,26,28 | | button matrix columns, active low; rows are GPIO2.23-25 |

BMC protocol (sp404-bmc.c has the details): byte 0 is cable<<4|CIN and the
app dispatches on CIN (table 0x800ebc00); CIN 0/1 are system messages
"xx FF cmd arg", the rest MIDI. Known: 01 FF 05 01 → 00 FF 04 01 (main waits
on it), hello 01 FF 00 01 → 00 FF 00 01, 01 FE 11 00 → 00 FF FE '0',
00 FF FF nn = SHIFT (panel key 0x2A) held/released. 01 00 nn 00 (83 of them)
look like LED/pad settings.

Firmware facts worth knowing: 94 UI pages, handler table 0x8023b4fc; the
startup/main page is page 68 (handler 0x80051b90, object 0x1aa8 bytes);
DrawString is FUN_800ee530; file open is FUN_800b5d88(handle, path, mode);
the kernel's current-task pointer is 0x202bbbfc and TCBs are 0x90 apart
with names at +0x88 (`tools/tasks.py`).

## Status

Boots to the "SP-404" splash, brings up every task, loads project 01 from
the eMMC (`B:/ROLAND/SP-404MKII/PROJECT_01/...`: SMPL/BANKx-yy.SMP,
PADCONF.BIN, PTN/PTNnnnnn.BIN, PICTURE/startup_*.bmp), runs audio DMA at
48 kHz and the knob scan, then shows a blank screen: the page loop runs and
draws, but DrawString is never called. With the user's real projects on
the eMMC (FAT32 via `tools/mkdisk.py`) it loads all samples and patterns,
the current page ID (0x80245880) is 3, key presses reach the firmware
(FUN_8005da10), and the panel/debug frontend runs, but the screen stays
blank: pages 3/68 draw nothing themselves and DrawString is never called.
Also: the firmware reads DWT_CYCCNT (0xE0001004) for tempo timing, which
QEMU does not implement.

## Working here

- `sh tools/build.sh` then `sh tools/run.sh [secs]` (headless boot with a
  summary: exceptions, last new registers, polling hot spots).
- `SP404_TRACE=edma,lpspi,lpuart,bmc,sai,oled,gpio,gpio-rd,adc,pit,xbar`
  turns on our device traces (in the -D log).
- `python tools/mon.py -t SECS "cmd" sleep:0.1 ...` — QEMU monitor; with
  `sh tools/screen.sh SECS NAME` for an OLED screenshot (build/logs/NAME.png).
- `sh tools/gdbrun.sh SECS script.gdb` — GDB against the running firmware;
  `tools/fopen.gdb` logs every file opened, `tools/draws.gdb` every string.
- `sh tools/gdec.sh ADDR...` / `sh tools/gxref.sh ADDR...` — Ghidra
  decompile / cross-references (project from `sh tools/ghidra_import.sh`).
- `python tools/tasks.py [secs]` — T-Kernel task table.

Pitfalls met: Python on Windows writes CRLF in text mode — open files with
`newline='\n'`. Git Bash rewrites arguments starting with `/`; use
`MSYS_NO_PATHCONV=1`. Heredocs with `\n` inside Python strings get mangled;
write a script file. QEMU blocks a device re-entering its own MMIO (the eDMA
writing into the device whose register write raised the request): such
devices set `iomem.disable_reentrancy_guard`.
