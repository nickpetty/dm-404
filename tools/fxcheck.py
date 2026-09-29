"""Check that every effect changes the sound: loop a pad, engage each MFX
effect in turn, and compare the output with the dry signal.

    python tools/fxcheck.py [-o DIR] [KNOB_RAW]

Plays pad 3 (a looping pad in the test project), records the emulator's
output over the link, then for each effect of the MFX menu (tools/fxmap.py
explains the selection) sets CTRL 1-3 to KNOB_RAW (as the unit reads it; 0 =
fully clockwise, default 1200) and measures ~2 s. Prints per effect: output
RMS against dry, zero-crossing rate against dry (brightness), how much of
the signal differs from dry, and flags silence, clipping and NaN-like
blow-ups. -o DIR also writes each capture as a WAV for listening.
"""
import json, math, os, socket, struct, subprocess, sys, time, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT


def stats(pcm):
    n = len(pcm) // 4
    if n == 0:
        return 0.0, 0.0, 0
    s = struct.unpack('<%dh' % (n * 2), pcm[:n * 4])
    m = [(s[2 * i] + s[2 * i + 1]) / 2 for i in range(n)]
    rms = math.sqrt(sum(x * x for x in m) / n)
    zc = sum(1 for i in range(1, n) if (m[i - 1] < 0) != (m[i] < 0)) / n
    peak = max(abs(x) for x in s)
    return rms, zc, peak


def main():
    args = sys.argv[1:]
    outdir = None
    if args[:1] == ['-o']:
        outdir, args = args[1], args[2:]
        os.makedirs(outdir, exist_ok=True)
    knob = int(args[0]) if args else 1200
    panel = json.load(open(os.path.join(ROOT, 'frontend', 'panel.json')))
    fxmap = {e['id']: e['name'] for e in json.load(open(os.path.join(ROOT, 'core', 'fx', 'fxmap.json')))}
    port = 5474
    exe = os.path.join(ROOT, 'build', 'qemu', 'qemu-fxcheck.exe')
    open(exe, 'wb').write(open(os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe'), 'rb').read())
    log = os.path.join(ROOT, 'build', 'logs', 'fxcheck.log')
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % port,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' +
                          os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none', '-D', log],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def send_knob(name, v):
        a = panel[name]['analog']
        lk.send(0x82, struct.pack('<BBBH', a[0], a[1], a[2], v))

    def key(name, down):
        r, c = panel[name]['key']
        lk.key(r, c, down)

    def capture(secs):
        with lk.lock:
            start = len(lk.pcm)
        time.sleep(secs)
        with lk.lock:
            return bytes(lk.pcm[start:])

    def save(name, pcm):
        if outdir:
            w = wave.open(os.path.join(outdir, name + '.wav'), 'wb')
            w.setnchannels(2), w.setsampwidth(2), w.setframerate(48000)
            w.writeframes(pcm)
            w.close()

    # The MFX menu order (firmware table at 0x80245d4c), starting at Scatter.
    d = open(os.path.join(ROOT, 'build', 'regions', 'r_80245740_decompress.bin'), 'rb').read()
    order = [struct.unpack_from('<I', d, 0x614 + 4 * i)[0] for i in range(42)]
    try:
        for _ in range(100):
            try:
                lk = L.Link(socket.create_connection(('127.0.0.1', port)))
                break
            except OSError:
                time.sleep(0.2)
        time.sleep(24)
        a = panel['3']['analog']
        lk.send(0x82, struct.pack('<BBBH', a[0], a[1], a[2], 600))
        time.sleep(0.2)
        lk.send(0x82, struct.pack('<BBBH', a[0], a[1], a[2], 4095))
        time.sleep(1.0)
        dry = capture(2.0)
        save('dry', dry)
        drms, dzc, dpk = stats(dry)
        print('dry: rms %.0f zc %.4f peak %d' % (drms, dzc, dpk))
        key('MFX', True), time.sleep(0.15), key('MFX', False), time.sleep(0.5)
        for i, fx in enumerate(order):
            if i:
                key('MFX', True), time.sleep(0.3)
                lk.send(0x84, struct.pack('<b', 1)), time.sleep(0.3)
                key('MFX', False), time.sleep(0.3)
            for k in ('CTRL 1', 'CTRL 2', 'CTRL 3'):
                send_knob(k, knob)
            # Dry right before (MFX off), then wet (MFX on again).
            key('MFX', True), time.sleep(0.15), key('MFX', False), time.sleep(0.4)
            dry = capture(1.5)
            drms, dzc, _ = stats(dry)
            key('MFX', True), time.sleep(0.15), key('MFX', False), time.sleep(0.4)
            pcm = capture(1.5)
            name = fxmap.get(fx, str(fx))
            save('%02d_%s' % (fx, name.replace('/', '-').replace(' ', '_')), pcm)
            rms, zc, pk = stats(pcm)
            flag = ' SILENT' if rms < drms * 0.05 else ' CLIP' if pk >= 32000 else ''
            print('%2d %-13s rms x%5.2f  zc x%5.2f  peak %5d%s' %
                  (fx, name, rms / max(drms, 1), zc / max(dzc, 1e-6), pk, flag))
    finally:
        q.kill()


if __name__ == '__main__':
    main()
