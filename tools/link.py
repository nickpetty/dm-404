"""Scriptable frontend-link client: boot the emulator, talk to it, look.

    python tools/link.py [-t SECONDS] [ACTION ...]

ACTION is one of
    wait:S              let S seconds pass
    mark:NAME           print the audio position (for finding events in a wav)
    wav:NAME            save all audio so far as build/logs/NAME.wav
    key:R,C             tap matrix key row R, column C (press, 150 ms, release)
    hold:R,C / up:R,C   press / release
    knob:ADC,CH,MUX,V   set an analog input
    bmc:AABBCCDD        inject a 4-byte packet from the BMC
    enc:N               turn the VALUE encoder N detents (negative: back)
    tone:S              play a 440 Hz sine into the unit's input for S seconds
    shot:NAME           save the screen as build/logs/NAME.png
    sweep               tap every matrix key in turn, saving a shot after each

The emulator runs with the eMMC image and the link on TCP 5404. Prints a
summary of what arrived: display frames, audio frames, BMC packets.
"""
import os, socket, struct, subprocess, sys, threading, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get('LINK_PORT', '5404'))


class Link:
    def __init__(self, sock):
        self.s = sock
        self.img = bytes(1024)
        self.frames = 0
        self.audio = 0
        self.peak = 0
        self.bmc = []
        self.pcm = bytearray()          # all audio, 48 kHz stereo s16le
        self.lock = threading.Lock()
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b''
        while True:
            try:
                d = self.s.recv(65536)
            except OSError:
                return
            if not d:
                return
            buf += d
            while len(buf) >= 4:
                t, _, n = struct.unpack_from('<BBH', buf)
                if len(buf) < 4 + n:
                    break
                p = buf[4:4 + n]
                buf = buf[4 + n:]
                with self.lock:
                    if t == 1:
                        self.img = p
                        self.frames += 1
                    elif t == 2:
                        self.audio += n // 4
                        self.pcm += p
                        for i in range(0, n, 2):
                            v = abs(struct.unpack_from('<h', p, i)[0])
                            self.peak = max(self.peak, v)
                    elif t == 3:
                        self.bmc.append(p.hex())

    def send(self, t, payload):
        self.s.sendall(struct.pack('<BBH', t, 0, len(payload)) + payload)

    def key(self, r, c, down):
        self.send(0x81, bytes([r, c, 1 if down else 0]))

    def shot(self, name):
        with self.lock:
            img = self.img
        w, h, sc = 128, 64, 4
        rows = []
        for y in range(h * sc):
            row = bytearray([0])
            for x in range(w * sc):
                on = img[(y // sc) * 16 + (x // sc) // 8] & (0x80 >> ((x // sc) & 7))
                row += b'\xe8\xf0\xff' if on else b'\x00\x00\x00'
            rows.append(bytes(row))

        def chunk(t, b):
            return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
        path = os.path.join(ROOT, 'build', 'logs', name + '.png')
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' +
                               chunk(b'IHDR', struct.pack('>IIBBBBB', w * sc, h * sc, 8, 2, 0, 0, 0)) +
                               chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
        lit = sum(bin(b).count('1') for b in img)
        print('shot %s: %d lit pixels' % (name, lit))
        return lit


def main():
    args = sys.argv[1:]
    boot = 20.0
    if args[:1] == ['-t']:
        boot = float(args[1])
        args = args[2:]
    exe = os.environ.get('LINK_QEMU_EXE') or os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    q = subprocess.Popen([exe, '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % PORT,
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-monitor', 'none', '-serial', 'none']
                         + os.environ.get('LINK_QEMU_ARGS', '').split(),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                s = socket.create_connection(('127.0.0.1', PORT))
                break
            except OSError:
                time.sleep(0.2)
        link = Link(s)
        time.sleep(boot)
        for a in args:
            kind, _, v = a.partition(':')
            if kind == 'wait':
                time.sleep(float(v))
            elif kind in ('key', 'hold', 'up'):
                r, c = map(int, v.split(','))
                if kind != 'up':
                    link.key(r, c, True)
                if kind == 'key':
                    time.sleep(0.15)
                if kind != 'hold':
                    link.key(r, c, False)
            elif kind == 'knob':
                adc, ch, mux, val = map(int, v.split(','))
                link.send(0x82, struct.pack('<BBBH', adc, ch, mux, val))
            elif kind == 'tone':
                # A 440 Hz sine at -6 dB into the unit's input, in real time.
                import math
                end = time.time() + float(v)
                n = 0
                while time.time() < end:
                    frames = bytearray()
                    for i in range(480):
                        x = int(16000 * math.sin(2 * math.pi * 440 * (n + i) / 48000))
                        frames += struct.pack('<hh', x, x)
                    n += 480
                    link.send(0x85, bytes(frames[:256 * 4]))
                    link.send(0x85, bytes(frames[256 * 4:]))
                    time.sleep(0.01)
            elif kind == 'enc':
                link.send(0x84, struct.pack('<b', int(v)))
            elif kind == 'bmc':
                link.send(0x83, bytes.fromhex(v))
            elif kind == 'shot':
                link.shot(v)
            elif kind == 'mark':
                with link.lock:
                    print('mark %s at %.3f s' % (v, len(link.pcm) / 4 / 48000))
            elif kind == 'wav':
                with link.lock:
                    pcm = bytes(link.pcm)
                hdr = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +                     struct.pack('<IHHIIHH', 16, 1, 2, 48000, 48000 * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm))
                open(os.path.join(ROOT, 'build', 'logs', v + '.wav'), 'wb').write(hdr + pcm)
            elif kind == 'sweep':
                for r in range(8):
                    for c in range(7):
                        link.key(r, c, True)
                        time.sleep(0.2)
                        link.key(r, c, False)
                        time.sleep(0.8)
                        if link.shot('key_%d_%d' % (r, c)):
                            pass
        time.sleep(0.5)
        with link.lock:
            print('display frames %d, audio frames %d (peak %d), bmc packets %d'
                  % (link.frames, link.audio, link.peak, len(link.bmc)))
            sys_msgs = sorted({p for p in link.bmc if p[1] in '01' and p[0] == '0'} )
            print('system/other packets seen:', ' '.join(sys_msgs[:20]))
    finally:
        q.kill()


if __name__ == '__main__':
    main()
