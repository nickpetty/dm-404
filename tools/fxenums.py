"""List the firmware's display-string tables (runs of string pointers).

    python tools/fxenums.py [WORD ...]

Prints every run of two or more pointers to short strings found in the
unpacked firmware regions, or only the runs containing one of WORDs. These
are the value names of enumerated parameters (filter types, note lengths,
reverb types ...).
"""
import glob, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = os.path.join(ROOT, 'build', 'regions')
code = open(os.path.join(R, 'r_80000000_copy.bin'), 'rb').read()


def s(a):
    o = a - 0x80000000
    if not 0 <= o < len(code):
        return None
    e = code.find(b'\0', o)
    t = code[o:e]
    if 0 < len(t) < 24 and all(32 <= c < 127 for c in t):
        return t.decode()
    return None


words = sys.argv[1:]
for f in sorted(glob.glob(os.path.join(R, '*.bin'))):
    d = open(f, 'rb').read()
    o = 0
    while o + 4 <= len(d):
        if s(struct.unpack_from('<I', d, o)[0]) is None:
            o += 4
            continue
        e = o
        run = []
        while e + 4 <= len(d):
            t = s(struct.unpack_from('<I', d, e)[0])
            if t is None:
                break
            run.append(t)
            e += 4
        if len(run) >= 2 and (not words or any(w in run for w in words)):
            print('%s %x (%d): %s' % (os.path.basename(f), o, len(run), ' | '.join(run)))
        o = e
