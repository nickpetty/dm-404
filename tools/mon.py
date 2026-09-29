"""Boot the emulator, wait, then run QEMU monitor commands and print them.

    python tools/mon.py [-t SECONDS] [-e ENV=VAL] "info registers" "x/8wx 0x80000000" ...

The emulator runs headless with the same machine options as run.sh; its
own log goes to build/logs/mon.log. Useful for seeing where the CPU sits
(PC, which exception it is in) when the firmware goes quiet.
"""
import argparse, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def monitor(port, cmds):
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.settimeout(2)

    def drain():
        out = b''
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
                if out.endswith(b'(qemu) '):
                    break
        except socket.timeout:
            pass
        return out.decode(errors='replace')

    drain()
    for c in cmds:
        s.sendall(c.encode() + b'\n')
        out = drain()
        # The monitor echoes the command; drop that and the prompt.
        lines = [l for l in out.replace('\r', '').split('\n')
                 if not l.startswith('(qemu)') and l.strip() != c.strip()]
        print('>>> ' + c)
        print('\n'.join(l for l in lines if l.strip() and not l.startswith('\x1b')))
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-t', type=float, default=8)
    ap.add_argument('-p', type=int, default=4444)
    ap.add_argument('-d', default='guest_errors')
    ap.add_argument('cmds', nargs='*', default=['info registers'])
    a = ap.parse_args()
    exe = os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    args = [exe, '-M', 'sp404mk2,flash=' + os.path.join(ROOT, 'build', 'flash.bin'),
            '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
            '-nographic', '-serial', 'none',
            *(['-drive', 'if=sd,index=0,format=raw,file=' + os.path.join(ROOT, 'build', 'sd.img')] if os.environ.get('SP404_SD') else []),
            '-drive', 'if=sd,index=1,format=raw,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
            '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % a.p,
            '-d', a.d, '-D', os.path.join(ROOT, 'build', 'logs', 'mon.log')]
    p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.STDOUT)
    try:
        time.sleep(a.t)
        if p.poll() is not None:
            sys.exit('emulator exited with %d' % p.returncode)
        monitor(a.p, a.cmds)
    finally:
        p.kill()


if __name__ == '__main__':
    main()
