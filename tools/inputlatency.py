"""How late the unit's input comes out of it with EXT SOURCE on.

    python tools/inputlatency.py [CLICKS]

Streams input to the emulator from the start (as the app does, so it piles
up while the unit boots), turns EXT SOURCE on, then puts a click into the
input once a second and times, in the unit's own output frames, when each
comes back out: the emulator's input-to-output delay (the app's own audio
buffers come on top).
"""
import array, os, socket, struct, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT
PORT = 5492


def main():
    clicks = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    q = subprocess.Popen([os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe'),
                          '-M', 'sp404mk2,flash=%s,link=link' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % PORT,
                          '-drive', 'if=sd,index=0',
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-serial', 'none', '-monitor', 'none'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                s = socket.create_connection(('127.0.0.1', PORT))
                break
            except OSError:
                time.sleep(0.2)
        lk = L.Link(s)
        state = {'click': False, 'stop': False, 'sent_at': []}

        def feed():
            # 10 ms of input every 10 ms, in real time, from the start.
            t0 = time.perf_counter()
            k = 0
            while not state['stop']:
                chunk = bytearray(480 * 4)
                if state['click']:
                    state['click'] = False
                    struct.pack_into('<hh', chunk, 0, 20000, 20000)
                    with lk.lock:
                        state['sent_at'].append(len(lk.pcm) // 4)
                lk.send(0x85, bytes(chunk[:256 * 4]))
                lk.send(0x85, bytes(chunk[256 * 4:]))
                k += 1
                while time.perf_counter() < t0 + k * 0.01:
                    time.sleep(0.001)

        th = threading.Thread(target=feed, daemon=True)
        th.start()
        time.sleep(18)
        lk.key(0, 0, True)          # EXT SOURCE
        time.sleep(0.15)
        lk.key(0, 0, False)
        time.sleep(1.5)
        with lk.lock:
            start = len(lk.pcm) // 4
        for _ in range(clicks):
            state['click'] = True
            time.sleep(1.0)
        state['stop'] = True
        time.sleep(0.5)
        with lk.lock:
            pcm = array.array('h', bytes(lk.pcm))
        onsets = []
        quiet = True
        for f in range(start, len(pcm) // 2):
            v = abs(pcm[f * 2])
            if v > 300 and quiet:
                onsets.append(f)
                quiet = False
            elif v < 50:
                quiet = quiet or (onsets and f - onsets[-1] > 4800)
        for sent in state['sent_at']:
            later = [o for o in onsets if o >= sent]
            if later:
                print('click in at output frame %d, out at %d: %.1f ms' % (sent, later[0], (later[0] - sent) / 48.0))
            else:
                print('click in at output frame %d: not heard' % sent)
    finally:
        q.kill()


if __name__ == '__main__':
    main()
