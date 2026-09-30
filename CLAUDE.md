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
| ADC1 ch6 mux 1/2/3, mux 4 | | CTRL 1/2/3; SUB PAD (pressure, key 0x13, LED 0x34) |
| GPIO2.23-25 rows × GPIO2.20,21,22,28,26 cols | | key matrix, active low; IDs from table 0x82e48774 (FUN_8005da10) |
| GPIO2.18/19 | | VALUE encoder quadrature, active low, 4 transitions per detent |
| DWT_CYCCNT | 0xE0001004 | modelled in the machine (tempo clock reads it) |

Key IDs follow the panel's reading order (BUS FX 0x10, HOLD 0x11, EXT
SOURCE 0x12, SUB PAD 0x13 (analog), PATTERN SELECT 0x14 ... ROLL 0x21,
EXIT 0x22, COPY 0x23, REMAIN 0x24, A/F-E/J 0x25-29, SHIFT 0x2a,
FILTER+DRIVE-MFX 0x2b-30, VALUE push 0x31 = matrix row 7 col 3: menus take
it as enter), and each key's LED index is its ID + 0x21. The VALUE turn
reaches menus as keys 0x3f/0x40.
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
Boot reads ~22 MB of them: patch 0004 makes QEMU's card read multi-block
transfers a block at a time (not byte by byte) through a 256 KiB read-ahead,
which took the main screen from ~11 s to ~6 s; most of the rest is the
firmware's own waits (a kernel tick between its two CMD13 polls per read,
as on the unit). `python tools/boottime.py` times the screens and CPU use.

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
(0x1f is the backlight level; a pad whose sample ends gets a dim colour). Page 6
blinks the LED between the given value and its page-0/1 one (START/END's
option keys and current pad, BUS FX while choosing, pattern-select pads);
a later page-0/1 write stops it. Page 9 pulses slowly (MARK, once skip
back has triggered data), page 7 pulses too; pages 4 and 5 come once at
boot (idx 0 = cc) and are not LEDs. Mode changes also send a Roland DT1 SysEx
(F0 41 10 00 00 00 00 08 12 02 02 00 00 ...), apparently the tempo (00 03 07 00 = 88.0).

Effects are not computed by the i.MX: the TX lines carry dry buses
(pads on line 3 words 2/3 = bus 1) to Roland's BMC sound chip, which mixes
them and runs the effects. The app only sends it parameters, as DT1 SysEx
over the BMC UART (F0 41 10 00 00 00 00 08 12 addr[4] data.. sum F7; ~2000
at boot). APP0 is the BMC's firmware: 0-0x4c000 encrypted (entropy 8),
Thumb code at 0x140000, float tables from 0x1f0000. So core/fx stands in:
sp404fx.dll (DaisySP, `sh tools/build_fx.sh`) takes the DT1 writes and the
TX stems and makes the mix and the loopback; core/fx/README.md has the
parameter map (from the firmware's own tables), value scales and routing.
Tools: `tools/fxmap.py` (drive the panel, print the DT1 writes),
`tools/fxtable.py`, `tools/fxbench.py` (offline), `tools/fxcheck.py`.

Inputs: the frontend streams the audio device's input over the link; it is
mixed into SAI RX line 0 words 0/1, where sampling and the REC meter read.
The same words carry the resampling loopback (TX line 3 words 0-7, at
unity: 16-bit samples in 20-bit slots). Skip back sampling (MARK) records
them too: the audio task (FUN_0001ca80) feeds the recorder at 0x82fe4f08
(pointer at 0x82e0e970), whose ring only starts keeping audio once a
sample reaches the threshold at 0x80bcf238 (0x40c); until then MARK says
"No SKIP BACK Triggered Data".

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

## Storage (the app)

Nothing of Roland's or the user's ships: on first run the app asks for the
System Program (Roland's zip or SP404MKII_APP1.bin; sha256 checked, other
versions allowed with a warning). Frontend/Source/Storage.cpp keeps the unit
in %LOCALAPPDATA%\Doom-404: firmware/, system.bin (the NOR flash),
internal.img (B:, 16 GB sparse exFAT, no partition table) and sdcard.img
(A:, 16 GB sparse FAT32 with an MBR, IMPORT and EXPORT made). A dev
checkout's firmware/, build/flash.bin and build/emmc.img are copied over
once; tools/ still use build/. The firmware boots from blank drives (an
empty project). QEMU writes the images directly, so there is nothing to
save at exit.

The SD slot: card detect is GPIO1 pin 18, low with a card in, polled by
FUN_00008bce ~20/s; the machine derives it from the slot's SD bus, so eject
and insert (link 0x86, answered by 0x04; or the monitor's eject/change on
drive sd0) show up in the firmware ("No SD Card!"). The slot always exists
(`-drive if=sd,index=0` with no file = empty). mkdisk.py's (pyfatfs) FAT32
cards read "Unsupported SD Card!"; FatFs-made ones (FAT32 or exFAT, with or
without an MBR) work. Frontend/Source/FatImage.cpp wraps FatFs (vendored in
frontend/ThirdParty/fatfs, R0.15a) for the SD card window (browse, drag in
and out, delete, format; changing the card takes it out of the unit, closing
the window puts it back) and internal-drive backup/restore (to/from a folder
of files). `sh tools/build_fatimg.sh` builds the same code as a CLI,
build/fatimg.exe (create/info/ls/add/get/rm), for making test cards.

## DAW plugin (Doom-404 Link)

frontend/Plugin: a VST3 (AU on macOS) effect that carries the running
app's unit into a DAW track: the track's audio goes in as USB audio (link
0x87, its own queue in sp404-audio.c, played from a steady 20 ms backlog,
mixed into the inputs), the unit's output (pre-VOLUME, link 0x02) comes
back, scaled by the app's VOLUME (shared) and the plugin's trim. App and
plugin share daw-link.shm in the data folder (frontend/Source/DawLink.h,
version 3: SPSC rings of 48 kHz s16 each way (8 channels from the unit), stamped MIDI rings
each way, heartbeats, a session counter, one owning instance);
DawBridge.cpp is the app's end. The plugin holds a 60 ms cushion and
steers both rate converters by up to 0.5% to hold it; offline renders are
silent. The app mutes itself while a plugin plays (Options menu).

MIDI: the firmware takes MIDI as USB-MIDI packets from the BMC and plays
cables 8 and 9 (others ignored); its MIDI out goes on cable 9 (taken as
bit 3 USB + bit 0 the OUT jack), with the pad's note (pad 3 = note 50 on
channel 1). Frontend/Source/UsbMidi.h converts. Timing is by frames of the
unit's output (link out_frames, reset at connection): MIDI in (0x88) is
stamped with the output frame it should sound at (the plugin's output
position + its cushion + midiLead 20 ms) and handed to the firmware when
the unit gets there, so its sound comes back exactly one reported latency
(80 ms) later whatever the buffers do; MIDI out (0x05) is stamped with the
frame it happened at and the plugin emits it at that frame's sample. The
firmware reacts within ~4 ms. The plugin sends MIDI clock, Song Position
and Start/Continue/Stop from the DAW transport; the unit follows it as it
is (its tempo DT1 went to 140.0). The standalone app has the MIDI IN/OUT
jacks as computer MIDI ports (Options menu, cable 9 in, bit 0 out).
It also makes a virtual MIDI port pair named "Doom-404" (never the
Roland/SP-404 name) as the unit's USB MIDI (cable 8 in, bit 3 out) where
the system allows apps to (JUCE createNewDevice: macOS, Linux). Windows
cannot yet: Windows MIDI Services can (MidiVirtualDeviceManager, shown to
WinMM apps as MIDI 1.0 ports), but its App SDK is still a preview runtime
that apps may not redistribute (checked 2026-09); revisit when it ships in
Windows.

LinkHostTest (frontend/Tests) loads the VST3 like a DAW and runs it in
real time at 44.1 kHz against the running app: `LinkHostTest PLUGIN.vst3
SECS [--notes] [--play BPM]` (notes and their sound's delay, a fake
transport, the unit's MIDI out, the cushion's range).
`INSTALL_PLUGIN=1 sh tools/build_frontend.sh` copies it to
%LOCALAPPDATA%\Programs\Common\VST3.

Separate outputs: the effects engine (sp404fx version 3; QEMU's
sp404-fx.c has its own copy of the number, keep them equal or the engine
is refused and the mix goes dry) also gives DRY, BUS 1 and BUS 2 as they
reach the master effects (BUS 3/4), and the input as heard. QEMU sends the
buses (0x06) after each 0x02 block while the app asks (0x89, only while a
plugin plays); DawLink version 3 carries 8 channels from the unit; the
plugin has DRY / BUS 1 / BUS 2 as extra stereo outputs.

EXT SOURCE: the firmware sends the BMC InputVolume (02 00 00 02) 0xff on,
0 off; the engine mixes the input (after the input FX) at that level ahead
of the master effects; the resampling loopback leaves it out (the inputs
are added to RX as they are). INPUT SETTING's ROUTING (Mix / ExtIn,
02 01 00 1e) is the resample source, not this.

## USB port

imxrt-usb.c models USB OTG1 (0x402e0000, IRQ 113) in device mode (dQH /
dTD in guest memory, priming, completion, SETUP, stalls, bus reset, attach
through OTGSC B-session valid, which the firmware's NXP stack watches);
sp404-usbip.c serves it as USB/IP on a chardev (`-M ...,usbip=ID`; the
first client plugs the cable in, descriptors come from the firmware;
import resets and addresses it). `python tools/usbip_probe.py` lists,
imports and reads it: Roland 0582:02e7 "Roland SP-404MKII", one CDC serial
interface (interrupt 81, bulk 82/03). That is all the i.MX presents: the
unit's USB audio and MIDI belong to the BMC (the firmware sends it the USB
audio settings, USBAudio_*, Gain_UsbInput, Midi_UsbThru; USB-MIDI reaches
the firmware from the BMC as cable 8), whose firmware is encrypted, so a
USB audio/MIDI device for Roland's driver would have to be written here,
from the real unit's descriptors. Isochronous URBs are refused.

The panel can take a background picture (View menu; copied to the data
folder as background.*; stretched, in place of the body and backdrops) and
a text colour for what is printed on it, set on its own (settings: background,
textColour).

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
