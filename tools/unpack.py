"""Unpack SP404MKII_APP1.bin into the regions its startup code creates.

APP1 is flashed at 0x60080000 and begins with Arm Compiler startup code. The
__scatterload table (found through the two words at file 0x80) lists every
region the startup copies, decompresses or zeroes, with its handler. The RW
compression is armlink's LZ77 variant; the decoder is from tallfree
(github.com/Mudb0y/tallfree, MIT).

Writes build/regions/<name>.bin and build/regions/regions.txt.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FLASH = 0x60080000


def decompress(src, outlen):
    o = bytearray(); i = 0
    while len(o) < outlen:
        tok = src[i]; i += 1
        lit = tok & 7
        if lit == 0: lit = src[i]; i += 1
        cpy = tok >> 4
        if cpy == 0: cpy = src[i]; i += 1
        n = lit - 1
        if n > 0:
            o += src[i:i + n]; i += n
        if tok & 8:
            off = src[i]; i += 1
            start = len(o) - off
            for k in range(cpy + 2): o.append(o[start + k])
        else:
            o += b'\x00' * cpy
    return bytes(o[:outlen])


def main():
    fw = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin')
    out = os.path.join(ROOT, 'build', 'regions')
    os.makedirs(out, exist_ok=True)
    D = open(fw, 'rb').read()
    # The words at 0x80/0x84 give the table's bounds, relative to 0x80.
    rel0, rel1 = struct.unpack_from('<2I', D, 0x80)
    rows = [struct.unpack_from('<4I', D, off) for off in range(0x80 + rel0, 0x80 + rel1, 16)]
    # Rows are (load, exec, size, handler). The first row is the vector copy;
    # of the other two handlers, armlink places __decompress before zeroinit.
    copy_fn = rows[0][3]
    others = sorted({r[3] for r in rows} - {copy_fn})
    assert len(others) == 2, others
    kinds = {copy_fn: 'copy', others[0]: 'decompress', others[1]: 'zero'}
    with open(os.path.join(out, 'regions.txt'), 'w') as f:
        for load, dst, size, fn in rows:
            kind, foff = kinds[fn], load - FLASH
            name = ''
            if kind != 'zero':
                data = D[foff:foff + size] if kind == 'copy' else decompress(D[foff:], size)
                name = 'r_%08x_%s.bin' % (dst, kind)
                open(os.path.join(out, name), 'wb').write(data)
            line = '%-10s file 0x%06x -> 0x%08x size 0x%08x  %s' % (kind, foff, dst, size, name)
            print(line); f.write(line.rstrip() + '\n')


if __name__ == '__main__':
    main()
