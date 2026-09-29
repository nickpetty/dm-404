"""Make a FAT32 disk image for the emulated eMMC (B:) or SD card (A:).

    python tools/mkdisk.py OUT.img SIZE_MB [--label LABEL] [--dirs D1,D2,...]
        [--add HOSTPATH=IMAGEPATH ...]

The real internal drive is exFAT ("SP404mkII Internal Drive"); the
firmware's FatFs mounts FAT32 as well, which is all this makes. The volume
starts at sector 0 (no partition table).
"""
import argparse, os, sys

import fs.path
from pyfatfs import PyFat
from pyfatfs.PyFatFS import PyFatFS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('size_mb', type=int)
    ap.add_argument('--label', default='SP-404MKII')
    ap.add_argument('--dirs', default='')
    ap.add_argument('--add', action='append', default=[])
    a = ap.parse_args()

    with open(a.out, 'wb') as f:
        f.truncate(a.size_mb * 1024 * 1024)
    pf = PyFat.PyFat()
    pf.mkfs(a.out, PyFat.PyFat.FAT_TYPE_FAT32, size=a.size_mb * 1024 * 1024,
            label=a.label[:11].upper())
    pf.close()

    vol = PyFatFS(a.out, read_only=False)
    for d in filter(None, a.dirs.split(',')):
        vol.makedirs(d, recreate=True)
    for spec in a.add:
        host, img = spec.split('=', 1)
        vol.makedirs(fs.path.dirname(img) or '/', recreate=True)
        with open(host, 'rb') as src:
            vol.writebytes(img, src.read())
    for path in vol.walk.dirs('/'):
        print(path)
    vol.close()


if __name__ == '__main__':
    main()
