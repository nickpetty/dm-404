"""Time the boot: when each new screen appears, and the emulator's CPU use.

    python tools/boottime.py [SECONDS] [QEMU ARGS...]

Starts the emulator with the link (eMMC snapshot), and prints the time of
every screen change and, once a second, the emulator process's CPU time,
so waits (idle CPU) can be told apart from work (a core busy).
"""
import hashlib, os, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT


def cpu_seconds(pid):
    # Windows: kernel + user time of the process via wmic-free ctypes.
    import ctypes
    from ctypes import wintypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
    if not h:
        return 0.0
    c, e, k, u = (wintypes.FILETIME() for _ in range(4))
    ctypes.windll.kernel32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    ctypes.windll.kernel32.CloseHandle(h)
    t = lambda f: (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7
    return t(k) + t(u)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    extra = sys.argv[2:]
    port = 5476
    exe = os.path.join(ROOT, 'build', 'qemu', 'qemu-boottime.exe')
    # BOOT_QEMU=path times another build (copied next to the usual one).
    src = os.environ.get('BOOT_QEMU') or os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    open(exe, 'wb').write(open(src, 'rb').read())
    t0 = time.time()
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % port,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' +
                          os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none'] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                lk = L.Link(socket.create_connection(('127.0.0.1', port)))
                break
            except OSError:
                time.sleep(0.05)
        last, last_cpu, n = None, 0.0, 0
        next_tick = 1.0
        while time.time() - t0 < secs:
            time.sleep(0.05)
            with lk.lock:
                img = bytes(lk.img)
            h = hashlib.md5(img).hexdigest()
            now = time.time() - t0
            if h != last:
                n += 1
                lit = sum(bin(b).count('1') for b in img)
                print('%6.2fs screen %d (%d lit)' % (now, n, lit))
                if n <= 12:
                    lk.shot('boot%02d' % n)
                last = h
            if now >= next_tick:
                c = cpu_seconds(q.pid)
                print('%6.2fs   cpu %.0f%%' % (now, (c - last_cpu) * 100))
                last_cpu = c
                next_tick += 1.0
    finally:
        q.kill()


if __name__ == '__main__':
    main()
