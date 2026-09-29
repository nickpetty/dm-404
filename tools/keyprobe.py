"""keyprobe.py R,C [R,C ...]: identify panel keys. For each key, in a fresh
boot each (parallel, eMMC snapshot):
  - screenshot while it is held (km2_R_C_held.png) and after release
    (km2_R_C_up.png), with the LED packets (both pages) it caused;
  - after entering REC, whether it brings the main screen back (EXIT).
Prints one line per key; writes build/logs/keyprobe.json.
"""
import json, os, socket, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT


def boot(port, exe):
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % port,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            return q, L.Link(socket.create_connection(('127.0.0.1', port)))
        except OSError:
            time.sleep(0.2)
    q.kill()
    raise RuntimeError('no link')


def leds(lk):
    with lk.lock:
        pk = list(lk.bmc)
        lk.bmc.clear()
    out = {}
    for p in pk:
        b = bytes.fromhex(p)
        if (b[0] & 0x0f) == 1 and b[1] in (0, 1) and b[2] >= 0x30:
            out['%d/%02x' % (b[1], b[2])] = b[3]
    return out


def probe(rc, port, exe):
    r, c = rc
    res = {}
    q, lk = boot(port, exe)
    try:
        time.sleep(22)
        leds(lk)
        lk.key(r, c, True)
        time.sleep(0.5)
        lk.shot('km2_%d_%d_held' % rc)
        lk.key(r, c, False)
        time.sleep(0.8)
        lk.shot('km2_%d_%d_up' % rc)
        res['leds'] = leds(lk)
    finally:
        q.kill()
    # EXIT test: REC, then this key; the main screen shows "P-1" on top.
    q, lk = boot(port, exe)
    try:
        time.sleep(22)
        with lk.lock:
            main = lk.img
        lk.key(0, 2, True); time.sleep(0.2); lk.key(0, 2, False); time.sleep(0.8)
        with lk.lock:
            rec = lk.img
        lk.key(r, c, True); time.sleep(0.2); lk.key(r, c, False); time.sleep(0.8)
        with lk.lock:
            after = lk.img
        res['exits_rec'] = rec != main and after[:16 * 9] == main[:16 * 9]
    finally:
        q.kill()
    return rc, res


def main():
    keys = [tuple(map(int, a.split(','))) for a in sys.argv[1:]]
    workers = 3
    src = os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    exes = []
    for w in range(workers):
        exe = os.path.join(ROOT, 'build', 'qemu', 'qemu-probe%d.exe' % w)
        with open(src, 'rb') as a, open(exe, 'wb') as b:
            b.write(a.read())
        exes.append(exe)
    free = list(range(workers))
    lock = threading.Lock()

    def job(rc):
        with lock:
            w = free.pop()
        try:
            return probe(rc, 5420 + w, exes[w])
        finally:
            with lock:
                free.append(w)

    out = {}
    with ThreadPoolExecutor(workers) as pool:
        for rc, res in pool.map(job, keys):
            out['%d,%d' % rc] = res
            print(rc, res, flush=True)
    json.dump(out, open(os.path.join(ROOT, 'build', 'logs', 'keyprobe.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
