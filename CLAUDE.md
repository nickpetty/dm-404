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
| PIT ch0 → XBAR 56→103 → ADC_ETC trig0 → ADC1 ch 4,5,6,3 | | analog scan, 2 kHz, through a mux addressed by GPIO2.27 (bit0), .30, .31 |
| ADC1 ch4/ch5 mux 0-7 | | the 16 pads (pressure; idle reads 4095; map in frontend/panel.json) |
| ADC1 ch6 mux 1/2/3, mux 4 | | CTRL 1/2/3; VALUE push (key 0x13) |
| GPIO2.23-25 rows × GPIO2.20,21,22,28,26 cols | | key matrix, active low; IDs from table 0x82e48774 (FUN_8005da10) |
| GPIO2.18/19 | | VALUE encoder quadrature, active low, 4 transitions per detent |
| DWT_CYCCNT | 0xE0001004 | modelled in the machine (tempo clock reads it) |

Key IDs follow the panel's reading order (BUS FX 0x10, HOLD 0x11, EXT
SOURCE 0x12, VALUE push 0x13, PATTERN SELECT 0x14 ... ROLL 0x21, EXIT 0x22,
COPY 0x23, REMAIN 0x24, A/F-E/J 0x25-29, SHIFT 0x2a, FILTER+DRIVE-MFX
0x2b-30, SUB PAD 0x31), and each key's LED index is its ID + 0x21.
frontend/panel.json holds the resulting bindings.

Audio timing: the firmware double-buffers 64-frame blocks (1.33 ms each).
The DMA interrupt (IRQ 3, FUN_80003408) re-arms channels 0-3 on the next
half at once and wakes the audio task (FUN_0001cf78, micro T-Kernel task 1),
which renders the other half and waits again. Emulated, the CPU and the SAI
run in different threads, so the SAI clocks a block only when the firmware
is ready for it (machine ready hook: DMA interrupts serviced, channels 0-4
re-armed, task 1's TCB state back to WAIT; up to 5 ms), and it runs on its
own host thread with high-resolution sleeps (QEMU timers on Windows wake
only every millisecond or so, and those waits added up to seconds of lag).
Projects exceed the 64 MB SDRAM and stream from the eMMC while playing.

VOLUME is an analog pot after the DAC: the firmware never reads it, the
frontend applies it as output gain. Audio: pads play into TX line 3 slots
2/3 at modest digital level.

BMC protocol (sp404-bmc.c has the details): byte 0 is cable<<4|CIN and the
app dispatches on CIN (table 0x800ebc00); CIN 0/1 are system messages
"xx FF cmd arg", the rest MIDI. Known: 01 FF 05 01 → 00 FF 04 01 (main waits
on it); 01 FE 11 00 → 00 FF FE '0'; the hello 01 FF 00 01 gets NO answer
(00 FF 00 01 means "go to page 3", a blank power-off page); 00 FF FF nn =
SHIFT (key 0x2A) held/released. LEDs go out as 01 00 idx value: idx 0-0x2f
pad RGB triplets (pad n at 3(n-1)), 0x30-0x52 button LEDs. Page 1
(01 01 idx value) writes the same LEDs and the latest write on either page
wins: keys and playing pads are lit on page 0 and dimmed back on page 1
(0x1f is the backlight level; a pad whose sample ends gets a dim colour) Page 6
blinks the LED between the given value and its page-0/1 one (START/END's
option keys and current pad, BUS FX while choosing, pattern-select pads);
a later page-0/1 write stops it. Mode changes also send a Roland DT1 SysEx
(F0 41 10 00 00 00 00 08 12 02 02 00 00 ...), apparently the tempo (00 03 07 00 = 88.0).

Inputs: the frontend streams the audio device's input over the link; it is
mixed into SAI RX line 0 words 0/1, where sampling and the REC meter read.

Firmware facts worth knowing: 94 UI pages, handler table 0x8023b4fc; the
current page ID is at 0x80245880 (page
registry 0x801f8f68, 16-byte records by ID; FUN_800da598(n) requests one);
the startup/main page object is page 68 (handler 0x80051b90, object 0x1aa8 bytes);
DrawString is FUN_800ee530; file open is FUN_800b5d88(handle, path, mode);
the kernel's current-task pointer is 0x202bbbfc and TCBs are 0x90 apart
with names at +0x88 (`tools/tasks.py`).

## Status

Boots to the main screen with the user's projects on the eMMC, pads play
(and light), CTRL knobs and effect buttons work, VALUE turns. The JUCE
frontend runs it all; bindings for the remaining panel buttons and their
LEDs are being filled in from key-map runs (tools/keymap.sh).

Test runs (tools/link.py) open the eMMC with snapshot=on: the firmware
keeps state there, and runs must not leak into each other or share a
writable image.

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
