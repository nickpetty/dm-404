"""Dump micro T-Kernel's task table from a running emulator.

    python tools/tasks.py [SECONDS]

Boots the emulator (via mon.py), saves the TCB array and prints each task:
id, name, state, priority, entry point, and the wait fields. Layout found
from the PendSV dispatcher (ITCM 0xdd3c) and the TCBs themselves:
TCBs are 0x90 bytes apart; +0x08 tskid, +0x14 entry, +0x20 priorities
(initial, base, current) and state in the top byte, +0x28 wait spec,
+0x2c wait object id, +0x84 saved SP, +0x88 an 8-character name.
"""
import os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TCB_BASE = 0x202bb000          # dump window; the table lies inside
WINDOW = 0x6000
STRIDE = 0x90
STATES = {0: 'NONEXIST', 1: 'READY', 2: 'WAIT', 4: 'SUSPEND', 6: 'WAIT+SUS',
          8: 'DORMANT'}


def main():
    secs = sys.argv[1] if len(sys.argv) > 1 else '25'
    out = os.path.join(ROOT, 'build', 'logs', 'tcb.bin')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mon.py'), '-t', secs,
                    'pmemsave 0x%x 0x%x %s' % (TCB_BASE, WINDOW, out.replace('\\', '/')),
                    'xp/1wx 0x202bbbfc'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = open(out, 'rb').read()

    def w(a):
        return struct.unpack_from('<I', d, a - TCB_BASE)[0]

    # Find the task with id 1 by looking for the id sequence at the stride.
    first = None
    for a in range(TCB_BASE, TCB_BASE + WINDOW - STRIDE * 4, 4):
        if w(a + 8) == 1 and w(a + 8 + STRIDE) == 2 and w(a + 8 + 2 * STRIDE) == 3:
            first = a
            break
    if first is None:
        sys.exit('TCB table not found')
    print('TCB table at %08x' % first)
    a = first
    while a + STRIDE <= TCB_BASE + WINDOW and 0 < w(a + 8) < 256:
        pri = w(a + 0x20)
        state = pri >> 24
        name = d[a + 0x88 - TCB_BASE:a + 0x90 - TCB_BASE].split(b'\0')[0].decode('latin1')
        if state:
            print('%3d %-8s %-9s pri %3d entry %08x wspec %08x wid %-6d w30 %08x sp %08x'
                  % (w(a + 8), name, STATES.get(state, hex(state)), (pri >> 16) & 0xff,
                     w(a + 0x14), w(a + 0x28), w(a + 0x2c), w(a + 0x30), w(a + 0x84)))
        a += STRIDE


if __name__ == '__main__':
    main()
