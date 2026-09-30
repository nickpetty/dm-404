# doom-404

An emulator of the Roland SP-404MKII sampler that runs **Roland's own
firmware** (System Program 5.52) on an emulated NXP i.MX RT1060, in a fork of
QEMU, with a desktop app that looks and plays like the hardware and a DAW
plugin that brings it into your sessions.

Named after Dr. Doom, not the game. An independent project: not affiliated
with or endorsed by Roland (see [Licensing](#licensing)).

> **You need your own copy of Roland's firmware.** Nothing of Roland's is in
> this repository or in anything built from it. The app asks for the System
> Program on first run: download it free from Roland's SP-404MKII support
> page.

## What works

- **The unit.** It boots to the main screen with blank drives or your own
  projects. The panel's keys, pads (velocity), CTRL knobs, VALUE encoder and
  LEDs work, including blinking and pulsing LEDs. SHIFT latches on click and
  lets go after the next key.
- **Audio.** Pads, the metronome, sampling, resampling and skip back all
  work. So do the unit's inputs, from your audio device, heard through EXT
  SOURCE with about 20 ms of delay inside the emulator.
- **Effects.** All 48 effects, on BUS 1-4 and the input FX. They run in a
  stand-in for the unit's effects chip, built on DaisySP, since that chip's
  own program is encrypted.
- **Storage.**
  - A 16 GB internal drive.
  - An SD card you can take out and put back while the unit runs, with a
    window for copying files on and off it by drag-and-drop.
  - Backup and restore of the internal drive.
- **MIDI.**
  - The unit's MIDI IN/OUT jacks, mapped to any MIDI ports on your computer.
  - A "Doom-404" MIDI port of the app's own: built in on macOS and Linux; on
    Windows it needs Windows MIDI Services (see below).
  - MIDI clock in and out; the unit follows tempo.
- **DAW plugin (Doom-404 Link, VST3).** It puts the unit in a DAW track:
  - The track's audio and MIDI go in.
  - The unit's output and MIDI come back, in sync, with the plugin's latency
    reported so the DAW can compensate.
  - DRY, BUS 1 and BUS 2 are also available as separate outputs.
  - The DAW's tempo and transport drive the unit as MIDI clock.
- **USB port.** The unit's USB serial port, as the firmware presents it
  (Roland 0582:02E7), served as USB/IP, for tools such as Roland's app.
  Needs a USB/IP client (see below).
- **Panel options.** A custom background picture and text colour, and a
  debug drawer shown and hidden with the <kbd>`</kbd> key.

Not there yet:
- The unit's USB audio and MIDI device. On the hardware that comes from the
  effects chip, whose firmware is encrypted.
- Offline (faster than real time) bouncing in a DAW.
- macOS and Linux builds of the app. The code is written to be portable, but
  it is only built and tested on Windows so far.

## What you need to run it

- Windows 10 or 11, 64-bit, with an audio device.
- **The SP-404MKII System Program 5.52** from Roland: the zip, or the
  `SP404MKII_APP1.bin` inside it. The app checks it against its checksum;
  other versions are allowed with a warning but untested.
- Optional:
  - A DAW that loads VST3 plugins, for Doom-404 Link.
  - [usbip-win2](https://github.com/vadimgrn/usbip-win2), to attach the
    unit's USB port to Windows (`usbip attach -r 127.0.0.1 -b 1-1` while the
    app runs). Linux has `usbip` built in.
  - Windows MIDI Services, for the "Doom-404" MIDI port on Windows. The app
    uses it when Windows provides it (it is being built into Windows 11), or
    when you have put Microsoft's `Windows.Devices.Midi2.dll` in the data
    folder yourself. Doom-404 does not ship it.

The unit's drives, settings and your copy of the firmware live in
`%LOCALAPPDATA%\Doom-404`:
- `firmware/`: your copy of the System Program.
- `system.bin`: the unit's flash (its settings).
- `internal.img`: the internal drive (exFAT).
- `sdcard.img`: the SD card (FAT32).

The drives are sparse files: they take only the space that is used. The
emulator writes them as it goes, so there is nothing to save when you quit.

## Building

Everything below is for Windows, run from **Git Bash** in the repository.

### Prerequisites

- **Git**, and **CMake 3.25 or newer**.
- **Visual Studio 2026**, with the *Desktop development with C++* workload
  and a Windows 11 SDK (10.0.26100). The build uses the SDK's `cppwinrt`.
- **MSYS2**, installed at `C:\msys64`, for building QEMU and the effects
  engine. From an MSYS2 shell:

  ```bash
  pacman -S --needed git make bison flex diffutils mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-distlib mingw-w64-ucrt-x86_64-zstd
  ```

- Optional: **Python 3** with `numpy`, `pyfatfs` and `fs`, for the tools in
  `tools/`.

The build fetches, the first time:
- QEMU v11.1.2
- DaisySP (and its LGPL half)
- JUCE 9.0.2 (or give it your own: `JUCE_DIR=/path/to/JUCE`)
- the Windows MIDI Services SDK metadata

Everything fetched goes into `third_party/` or `build/`; both are ignored by
git.

### Steps

1. **The emulator core.** This clones QEMU v11.1.2 into `third_party/qemu`,
   applies `core/qemu/patches/`, lays `core/qemu/` over it, and builds
   `build/qemu/qemu-system-arm.exe`.

   ```bash
   sh tools/build.sh
   ```

2. **The effects engine.** This builds `build/fx/sp404fx.dll` with DaisySP
   and copies it next to the emulator, which loads it at run time.

   ```bash
   sh tools/build_fx.sh
   ```

3. **The app, the plugin and the plugin test host.** This builds
   `build/frontend/Doom404_artefacts/Release/Doom-404.exe`, the VST3 in
   `build/frontend/Doom404Link_artefacts/Release/VST3/`, and `LinkHostTest`.
   With `INSTALL_PLUGIN=1` it also copies the VST3 to
   `%LOCALAPPDATA%\Programs\Common\VST3`.

   ```bash
   INSTALL_PLUGIN=1 sh tools/build_frontend.sh
   ```

4. **Run it.** Start `Doom-404.exe`. On first run it asks for the System
   Program, then makes blank drives. In a development checkout it finds the
   emulator in `build/qemu/`.

5. **A release package.** This lays out `build/dist/Doom-404-VERSION-win64/`
   and zips it:
   - the app, with the emulator in `qemu/` beside it (stripped of debug
     info), its effects engine, and the MSYS2 runtime DLLs they need (found
     from their imports);
   - the VST3;
   - this project's and every bundled component's licence texts,
     `THIRD-PARTY.txt`, and a `SOURCE.txt` pointing at the exact commit.

   ```bash
   python tools/package.py
   ```

`CLAUDE.md` is the developer's notebook: how the hardware was worked out, the
link protocols, and the debugging tools (`tools/run.sh`, `tools/mon.py`,
the Ghidra scripts, `tools/fxmap.py`, `tools/usbip_probe.py`, and more).

## Using it

- **Pads** play when you click them: nearer the top hits harder. **Knobs**
  turn by dragging or with the mouse wheel. **VALUE** turns by dragging or
  the wheel, and a click pushes it (enter).
- **SHIFT**: a click holds it until the next key or pad, or hold the
  computer's Shift key. **Ctrl-click** any other key to hold it (for example
  MFX while choosing an effect), and click it again to let go.
- **Unit menu**: the SD card window, take the card out or put it back, back
  up and restore the internal drive, restart, choose the System Program, and
  open the data folder.
- **View menu**: the debug drawer (<kbd>`</kbd>), background image, text
  colour.
- **Options menu**:
  - audio device;
  - muting the app while a DAW plugin plays the unit;
  - the MIDI IN/OUT jack ports;
  - the "Doom-404" MIDI port.
- **In a DAW**, put *Doom-404 Link* on a track while the app runs.

## Licensing

doom-404 combines projects under different licences, so its parts are
licensed differently. [`LICENSE`](LICENSE) has the details, and the full
texts are in [`LICENSES/`](LICENSES/).

| Part | Licence |
|---|---|
| `core/qemu/`: the machine and device models, and patches to QEMU | **GPL-2.0-or-later**, as part of QEMU |
| `core/fx/`: the effects engine | **MIT** |
| `sp404fx.dll`, as built | MIT, plus **LGPL-2.1** for the DaisySP-LGPL parts it links (compressor, ReverbSc) |
| `frontend/`: the app, the Doom-404 Link plugin, tests | **AGPL-3.0-or-later**, as required by JUCE's open-source licence |
| `frontend/ThirdParty/fatfs/`: ChaN's FatFs | FatFs licence (BSD-style) |
| `tools/`, documentation, everything else | **MIT** |

Third-party projects and their licences:

| Project | Licence | How it is used |
|---|---|---|
| [QEMU](https://www.qemu.org) v11.1.2 | GPL-2.0 | fetched and patched at build time; the emulator binary is GPL |
| [JUCE](https://juce.com) 9 | AGPLv3 or commercial | fetched at build time; the app and plugin are AGPLv3 |
| Steinberg VST3 SDK (within JUCE) | MIT | the plugin format |
| [DaisySP](https://github.com/electro-smith/DaisySP) | MIT | fetched at build time, linked into the effects engine |
| DaisySP-LGPL | LGPL-2.1 | the same; its source is public, so the engine can be rebuilt or relinked |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15a | BSD-style (one clause) | vendored, for the drive images |
| [Windows MIDI Services SDK](https://aka.ms/midi) | MIT | metadata only, at build time; not shipped |

If you pass on built binaries, keep their licences:
- Offer the source for the GPL and AGPL parts (this repository, plus QEMU
  v11.1.2).
- Keep the licence texts and copyright notices with them.

**Roland.** The SP-404MKII firmware is Roland's copyright and is never
included; each user supplies their own copy. "Roland" and "SP-404" are
trademarks of Roland Corporation, used here only to say what hardware is
emulated. The app's own names (the "Doom-404" MIDI port, the plugin) avoid
Roland's names.
