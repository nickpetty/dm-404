"""ppm2png.py IN.ppm OUT.png: convert a QEMU screendump; prints lit pixels."""
import struct, sys, zlib

d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)


open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' +
                              chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
print('%dx%d, %d lit pixels' % (w, h, sum(1 for i in range(0, len(px), 3) if px[i])))
