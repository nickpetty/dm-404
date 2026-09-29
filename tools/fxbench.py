"""Bench the effects engine offline: every effect on white noise.

    python tools/fxbench.py [KNOB] [ID ...]

Loads build/fx/sp404fx.dll with ctypes and, for each effect (or the IDs
given), selects it on BUS 1 with its firmware defaults and CTRL 1-3 at KNOB
(0..1 of each knob's range, default 0.7), plays 1 s of -20 dBFS white noise
into BUS 1 then 1 s of silence, and prints: level change (dB), spectral
centroid (brightness) change, how much of the output differs from the input
(1 = unrelated), energy left in the silent second (tails), and NaN/clipping.
"""
import ctypes, json, os, sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SR = 48000


def load():
    lib = ctypes.CDLL(os.path.join(ROOT, 'build', 'fx', 'sp404fx.dll'))
    lib.sp404fx_new.restype = ctypes.c_void_p
    lib.sp404fx_new.argtypes = [ctypes.c_int]
    lib.sp404fx_free.argtypes = [ctypes.c_void_p]
    lib.sp404fx_dt1.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.sp404fx_process.argtypes = [ctypes.c_void_p] + [np.ctypeslib.ndpointer(np.float32)] * 3 + [ctypes.c_int]
    return lib


def dt1(lib, fx, addr, value, nibbles=2):
    data = bytes((value >> (4 * (nibbles - 1 - i))) & 0xf for i in range(nibbles))
    lib.sp404fx_dt1(fx, bytes(addr), data, nibbles)


def centroid(x):
    s = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return float((s * f).sum() / max(s.sum(), 1e-12))


def run(lib, e, knob, stems_in):
    fx = lib.sp404fx_new(SR)
    dt1(lib, fx, [2, 2, 0, 0], 1200, 4)             # 120 BPM
    dt1(lib, fx, [2, 0, 0, 0x3e], 0)                # AudioMute off
    dt1(lib, fx, [2, 2, 0, 0x0a], e['id'])          # BUS 1 effect
    block = (e['id'] - 1) * 5 + 1
    ctrl = set(e['ctrl'])
    for p in e['params']:
        v = (p.get('default') or {}).get('1')
        if p['name'] in ctrl and 'max' in p:
            v = int(round(knob * p['max']))
        if v is not None:
            dt1(lib, fx, [3, block >> 7, block & 0x7f, p['addr']], v)
    dt1(lib, fx, [2, 0, 0, 0x16], 1)                # BUS 1 on
    n = stems_in.shape[0]
    out = np.zeros((n, 2), np.float32)
    inp = np.zeros((n, 2), np.float32)
    lib.sp404fx_process(fx, stems_in, inp, out, n)
    lib.sp404fx_free(fx)
    return out


def main():
    args = sys.argv[1:]
    knob = float(args.pop(0)) if args and '.' in args[0] else 0.7
    ids = {int(a) for a in args}
    effects = json.load(open(os.path.join(ROOT, 'core', 'fx', 'fxmap.json')))
    lib = load()
    rng = np.random.default_rng(1)
    noise = (rng.standard_normal((SR, 2)) * 0.1).astype(np.float32)
    x = np.concatenate([noise, np.zeros((SR, 2), np.float32)])
    stems = np.zeros((len(x), 8), np.float32)
    stems[:, 2:4] = x                               # BUS 1 = words 2/3
    dry = x[SR // 2:SR, 0]
    dc = centroid(dry)
    print('%2s %-13s %7s %7s %6s %8s %s' % ('id', 'effect', 'level', 'bright', 'diff', 'tail', ''))
    for e in effects:
        if ids and e['id'] not in ids:
            continue
        out = run(lib, e, knob, np.ascontiguousarray(stems))
        bad = not np.isfinite(out).all()
        out = np.nan_to_num(out)
        wet = out[SR // 2:SR, 0]
        lvl = 20 * np.log10(max(np.sqrt((wet ** 2).mean()), 1e-9) / np.sqrt((dry ** 2).mean()))
        br = centroid(wet) / dc
        diff = np.sqrt(((wet - dry) ** 2).mean() / (dry ** 2).mean())
        tail = 20 * np.log10(max(np.sqrt((out[SR + SR // 4:, 0] ** 2).mean()), 1e-9) / np.sqrt((dry ** 2).mean()))
        flag = ' NaN' if bad else ' CLIP' if np.abs(out).max() > 1.0 else ''
        print('%2d %-13s %+6.1fdB x%5.2f %6.2f %+6.0fdB%s' % (e['id'], e['name'], lvl, br, diff, tail, flag))


if __name__ == '__main__':
    main()
