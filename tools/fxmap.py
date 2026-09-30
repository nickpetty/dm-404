"""Map the effect parameters the firmware sends to the BMC sound chip.

    python tools/fxmap.py [-o FILE] [ACTION ...]

Boots the emulator (eMMC snapshot), then performs each ACTION and prints the
DT1 SysEx writes it caused, decoded as "addr(4) <- data". ACTIONs:

    key:NAME          tap a panel control by its frontend/panel.json name
    hold:NAME / up:NAME
    knob:NAME,V       set an analog control (0-4095 as the unit reads it,
                      i.e. before inversion: 0 = fully clockwise)
    enc:N             turn VALUE N detents
    mfx:N             choose the MFX effect N entries on in the menu (hold
                      MFX, turn VALUE, release)
    wait:S            let S seconds pass
    shot:NAME         OLED screenshot to build/logs/NAME.png
    label:TEXT        print TEXT (to annotate the output)

FXMAP_LEDS=1 also prints LED writes on pages other than 0/1 (blink, pulse);
FXMAP_LEDS=all prints every LED write.
FXMAP_LOG=FILE keeps the emulator's log (with SP404_TRACE=audio: per-slot
levels once a second, and the peaks of what is sampled, RX words 0/1).
FXMAP_INPUT=HZ[,LEVEL] feeds the unit's input a sine (LEVEL of 32767, default
8000) from the start, as the app streams its audio input. -o FILE also writes the boot-time dump (all writes before the first action).
Every write is "F0 41 10 00 00 00 00 08 12 a a a a d.. sum F7".
"""
import json, math, os, socket, struct, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT
NB = {4: 3, 5: 1, 6: 2, 7: 3}
HDR = bytes.fromhex('f04110000000000812')


def sysex(packets):
    out, cur = [], []
    for p in packets:
        b = bytes.fromhex(p)
        cin = b[0] & 0xf
        if cin in NB:
            cur += b[1:1 + NB[cin]]
            if cin != 4:
                out.append(bytes(cur))
                cur = []
    return out


def decode(m):
    if m[:9] != HDR or m[-1] != 0xf7:
        return 'raw ' + m.hex(' ')
    body = m[9:-2]
    return '%s <- %s' % (body[:4].hex(' '), body[4:].hex(' '))


def feed(lk, hz, level):
    """10 ms of a sine into the unit's input every 10 ms, in real time."""
    t0, k, ph = time.perf_counter(), 0, 0.0
    while True:
        pcm = bytearray()
        for _ in range(480):
            v = int(level * math.sin(ph))
            pcm += struct.pack('<hh', v, v)
            ph += 2 * math.pi * hz / 48000
        lk.send(0x85, bytes(pcm[:1024]))
        lk.send(0x85, bytes(pcm[1024:]))
        k += 1
        while time.perf_counter() < t0 + k * 0.01:
            time.sleep(0.001)


def main():
    args = sys.argv[1:]
    dump = None
    if args[:1] == ['-o']:
        dump, args = args[1], args[2:]
    panel = json.load(open(os.path.join(ROOT, 'frontend', 'panel.json')))
    port = int(os.environ.get('LINK_PORT', '5472'))
    exe = os.environ.get('LINK_QEMU_EXE') or os.path.join(ROOT, 'build', 'qemu', 'qemu-fxmap.exe')
    if not os.environ.get('LINK_QEMU_EXE'):
        open(exe, 'wb').write(open(os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe'), 'rb').read())
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % port,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' +
                          os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none'] +
                         (['-D', os.environ['FXMAP_LOG']] if os.environ.get('FXMAP_LOG') else []),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                lk = L.Link(socket.create_connection(('127.0.0.1', port)))
                break
            except OSError:
                time.sleep(0.2)
        if os.environ.get('FXMAP_INPUT'):
            hz, _, lv = os.environ['FXMAP_INPUT'].partition(',')
            threading.Thread(target=feed, args=(lk, float(hz), int(lv or 8000)), daemon=True).start()
        time.sleep(24)

        def take():
            time.sleep(0.6)
            with lk.lock:
                pk = list(lk.bmc)
                lk.bmc.clear()
            if os.environ.get('FXMAP_LEDS'):
                # LED writes "01 page idx value", except the plain pages 0/1.
                every = os.environ.get('FXMAP_LEDS') == 'all'
                leds = ['p%d:%s=%s' % (int(p[2:4], 16), p[4:6], p[6:8]) for p in pk
                        if p[1] == '1' and int(p[2:4], 16) < 0x10 and (every or p[2:4] not in ('00', '01'))]
                if leds:
                    print('   leds: ' + ' '.join(leds[:40]))
            return sysex(pk)

        boot = take()
        if dump:
            with open(dump, 'w', newline='\n') as f:
                f.write(''.join(decode(m) + '\n' for m in boot))
        print('boot: %d writes' % len(boot))

        def control(name):
            if name not in panel:
                sys.exit('no control %r in panel.json' % name)
            return panel[name]

        def press(name, down):
            b = control(name)
            if 'key' in b:
                lk.key(b['key'][0], b['key'][1], down)
            elif 'bmcDown' in b:
                lk.send(0x83, bytes.fromhex(b['bmcDown'] if down else b['bmcUp']))
            elif 'analog' in b:
                a = b['analog']
                lk.send(0x82, struct.pack('<BBBH', a[0], a[1], a[2], 600 if down else 4095))

        for act in args:
            kind, _, v = act.partition(':')
            if kind == 'key':
                press(v, True); time.sleep(0.15); press(v, False)
            elif kind == 'hold':
                press(v, True)
            elif kind == 'up':
                press(v, False)
            elif kind == 'knob':
                name, val = v.rsplit(',', 1)
                name = name.replace('_', ' ')
                a = control(name)['analog']
                lk.send(0x82, struct.pack('<BBBH', a[0], a[1], a[2], int(val)))
            elif kind == 'enc':
                lk.send(0x84, struct.pack('<b', int(v)))
            elif kind == 'mfx':
                press('MFX', True); time.sleep(0.3)
                lk.send(0x84, struct.pack('<b', int(v))); time.sleep(0.3)
                press('MFX', False)
            elif kind == 'wait':
                time.sleep(float(v))
                continue
            elif kind == 'shot':
                lk.shot(v)
                continue
            elif kind == 'label':
                print('## ' + v)
                continue
            msgs = take()
            print('%-28s %s' % (act, '; '.join(decode(m) for m in msgs) if msgs else '-'))
    finally:
        q.kill()


if __name__ == '__main__':
    main()
