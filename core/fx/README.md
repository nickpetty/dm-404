# sp404fx: the BMC's effects

On the SP-404MKII the i.MX firmware does not process audio for effects:
it sends dry buses to Roland's BMC sound chip on SAI1 TX line 3 and drives
the chip with parameter writes. The BMC's own program (the APP0 file) is
encrypted, so this directory is a stand-in: the same parameters, turned
into sound with DaisySP (`third_party/DaisySP`, MIT, plus its LGPL half for
ReverbSc and Compressor).

`tools/build_fx.sh` builds `build/fx/sp404fx.dll` and copies it next to
the emulator, which loads it at start (`core/qemu/hw/arm/sp404/sp404-fx.c`;
`SP404_FX=path` or `SP404_FX=off`). Without it the mix stays dry.

## Parameters

Every write is a Roland DT1 SysEx over the BMC UART:
`F0 41 10 00 00 00 00 08 12 a1 a2 a3 a4 d.. sum F7`. Each data byte is a
nibble, most significant first (two for a byte, four for the tempo).

| Address | What |
|---|---|
| `02 00 00 aa` | system parameters from ID 33 on, aa = 2 x (ID - 33): Volume, InputVolume, ..., InputFxOnOff (0x12), Bus1Mute (0x14), Bus1OnOff (0x16), Bus2..4 at +0x0a each, AudioMute (0x3e) |
| `02 01 00 aa` | IDs 82 on: MIDI, gains, Resample_Routing ... |
| `02 02 00 00` | Tempo_Master, BPM x 10 |
| `02 02 00 04`, `06` | Effect_BusRouting, Effect_InputAssign (the bus the input joins) |
| `02 02 00 08..10` | Effect_FxType0-4: the effect in slot 0 (input FX) and slots 1-4 (BUS 1-4), by effect ID |
| `03 hi lo aa` | effect parameters: hi:lo = (effect - 1) x 5 + slot as 7-bit halves, aa = 2 x parameter index |

The IDs and names come from the firmware's own tables (5.52):

- effect names by ID at 0x8020b3d8 (0 Bypass, 1 Filter+Drive ... 48 DJFX Delay);
- the parameter database's names at 0x80245ee4 (627 entries; effect
  parameters from ID 163, each effect's contiguous);
- per effect, the six parameters it sends at boot, CTRL 1-3 first, at
  0x802e5b40 (24-byte records by effect ID).

`tools/fxtable.py` combines them with a boot capture (`boot_dt1.txt`, from
`tools/fxmap.py -o`) into `fxmap.json` and `fxparams.h`, and adds each CTRL
knob's range from `ctrl_sweep.txt` (every MFX effect's knobs driven to both
ends with `tools/fxmap.py`).

## Value scales

The firmware sends raw values; what they mean was read off its screen with
the knobs at their ends and middle (`tools/fxmap.py ... shot:`). Most
0-255 parameters display 0-100. Others:

| Parameter | Scale |
|---|---|
| Filter+Drive CUTOFF | 20 Hz - 16 kHz, exponential |
| TimeCtrlDly / Tape Echo TIME | 10-800 ms, about square law |
| Sync Delay / Ko-Da-Ma TIME (0-15) | 1/32 1/16T 1/32D 1/16 1/8T 1/16D 1/8 1/4T 1/8D 1/4 1/2T 1/4D 1/2 1/1T 1/2D 1/1 |
| Slicer SPEED, To-Gu-Ro RATE | 22 lengths, 2/1 ... 1/64T |
| Phaser / Flanger RATE | 4 - 0.016 bars per cycle |
| Wah / Tremolo RATE | 1 - 0.01 bars |
| Isolator LOW/MID/HIGH | -INF .. 0 dB (middle) .. +12 dB |
| Equalizer gains, EQ LOW/HIGH (0-30) | -15 .. +15 dB |
| WrmSaturator DRIVE / EQ (0-48) | 0-48 dB / -24..+24 dB |
| Chromatic PS PITCH (0-36) | -24 .. +12 semitones |
| Cloud Delay PITCH (0-120) | -12 .. +12 in 0.2 |
| Resonator ROOT (0-127) | a MIDI note |
| DJFX Looper SPEED (0-200) | -100 .. +100 (backwards to forwards) |
| Crusher FILTER | 331 Hz - 15.4 kHz, square law |
| Super Filter FLT TYPE | 0 LPF, 1 BPF, 2 HPF |
| Reverb TYPE | AMBI, ROOM, HALL1, HALL2 |

Parameters outside the six the firmware sends at boot keep the BMC's own
defaults, which cannot be seen; EQ-style gains start at 0 dB here.

## Routing

TX line 3 words 0-7 are four stereo stems: pads on BUS 1 play on words 2/3
(seen); 4/5 is taken as BUS 2 and 0/1, 6/7 as DRY. BUS 1 and BUS 2 go
through slots 1 and 2, then the whole mix through slots 3 and 4 (BUS 3 and
BUS 4 in series). The input goes through the input FX (slot 0), then, at
InputVolume (EXT SOURCE on: 255, off: 0), joins the bus Effect_InputAssign
(`02 02 00 06`, EFX SET > OTHER > Input Bus) names: 0 DRY, 1 BUS1 (the
default), 2 BUS2. The metronome words (12, 14, 15) bypass the engine.

What is sampled (SAI RX words 0/1, `sp404fx_process_buses` channels 6/7)
follows INPUT SETTING's ROUTING (`02 01 00 1e`): Mix (0), the main output,
the input in it as its bus made it; ExtIn (1), the input alone, after the
input FX and at InputVolume.

## Checking

- `python tools/fxbench.py [knob] [ids]`: each effect on white noise,
  offline through the DLL: level, brightness, difference, tail.
- `python tools/fxcheck.py [-o dir]`: in the emulator, each MFX effect on a
  looping pad, dry against wet.
