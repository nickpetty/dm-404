"""keyleds.py: for each matrix key, boot fresh, press it and record which
LEDs change (the firmware's "01 00 idx value" packets to the BMC) and
whether the screen changes. Runs several emulators in parallel.

    python tools/keyleds.py [WORKERS]

Writes build/logs/keyleds.json: {"r,c": {"leds": {idx: value}, "screen": bool}}.
"""
import json, os, socket, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT
KEYS = [(r, c) for r in range(8) for c in range(4)] + [(0, 4)]


def run_key(rc, port, exe):
    r, c = rc
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % port,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                s = socket.create_connection(('127.0.0.1', port))
                break
            except OSError:
                time.sleep(0.2)
        lk = L.Link(s)
        time.sleep(22)

        def drain():
            with lk.lock:
                pk = list(lk.bmc)
                lk.bmc.clear()
                img = lk.img
            return pk, img

        _, before = drain()
        lk.key(r, c, True)
        time.sleep(0.3)
        lk.key(r, c, False)
        time.sleep(1.2)
        pk, after = drain()
        leds = {}
        for p in pk:
            b = bytes.fromhex(p)
            if (b[0] & 0x0f) == 1 and b[1] == 0 and b[2] >= 0x30:
                leds[b[2]] = b[3]
        s.close()
        return rc, {'leds': leds, 'screen': before != after}
    finally:
        q.kill()


def main():
    workers = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    src = os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    exes = []
    for w in range(workers):
        exe = os.path.join(ROOT, 'build', 'qemu', 'qemu-keyleds%d.exe' % w)
        with open(src, 'rb') as a, open(exe, 'wb') as b:
            b.write(a.read())
        exes.append(exe)
    slots = list(range(workers))
    lock = threading.Lock()

    def job(rc):
        with lock:
            w = slots.pop()
        try:
            return run_key(rc, 5410 + w, exes[w])
        finally:
            with lock:
                slots.append(w)

    out = {}
    with ThreadPoolExecutor(workers) as pool:
        for rc, res in pool.map(job, KEYS):
            out['%d,%d' % rc] = res
            print(rc, res, flush=True)
    json.dump(out, open(os.path.join(ROOT, 'build', 'logs', 'keyleds.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
