"""A USB/IP client against the emulated unit's USB port: list it, import it,
and read what it presents (descriptors, strings), as a computer would.

    python tools/usbip_probe.py [SECONDS_TO_BOOT] [--config]

Boots the emulator with its USB/IP server (-M ...,usbip=usbip on a TCP
chardev), lists the devices (OP_REQ_DEVLIST), imports busid 1-1, then asks
for the device, configuration and string descriptors with control URBs.
--config also sets configuration 1 (what a driver does next).
The emulator's log (SP404_TRACE=usb) goes to build/logs/usbip.log.
"""
import os, socket, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link as L

ROOT = L.ROOT
PORT, USBIP = 5490, 5491


def recv_all(s, n):
    b = b''
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise EOFError('closed after %d of %d bytes' % (len(b), n))
        b += d
    return b


def device(rec):
    busid = rec[256:288].split(b'\0')[0].decode()
    busnum, devnum, speed, vid, pid, bcd, cls, sub, proto, conf, nconf, nintf = \
        struct.unpack('>IIIHHHBBBBBB', rec[288:312])
    return ('busid %s speed %d  %04x:%04x bcd %04x class %02x/%02x/%02x config %d of %d, %d interfaces'
            % (busid, speed, vid, pid, bcd, cls, sub, proto, conf, nconf, nintf)), nintf


class Client:
    def __init__(self):
        self.s = socket.create_connection(('127.0.0.1', USBIP), timeout=10)
        self.seq = 0

    def devlist(self):
        self.s.sendall(struct.pack('>HHI', 0x0111, 0x8005, 0))
        ver, code, status, n = struct.unpack('>HHII', recv_all(self.s, 12))
        out = []
        for _ in range(n):
            text, nintf = device(recv_all(self.s, 312))
            intf = [recv_all(self.s, 4)[:3].hex() for _ in range(nintf)]
            out.append(text + '  interfaces ' + ' '.join(intf))
        return out

    def import_(self, busid='1-1'):
        self.s.sendall(struct.pack('>HHI', 0x0111, 0x8003, 0) + busid.encode().ljust(32, b'\0'))
        ver, code, status = struct.unpack('>HHI', recv_all(self.s, 8))
        if status:
            return None
        return device(recv_all(self.s, 312))[0]

    def submit(self, ep, direction_in, length, setup=b'\0' * 8, data=b''):
        self.seq += 1
        h = struct.pack('>IIIII', 1, self.seq, 0x10002, 1 if direction_in else 0, ep)
        h += struct.pack('>IiiiI', 0, length, 0, 0, 0) + setup
        self.s.sendall(h + (b'' if direction_in else data))
        r = recv_all(self.s, 48)
        cmd, seq, _, _, _, status, actual = struct.unpack('>IIIIIii', r[:28])
        body = recv_all(self.s, actual) if direction_in and actual > 0 else b''
        return status, body

    def control_in(self, req_type, req, value, index, length):
        return self.submit(0, True, length, struct.pack('<BBHHH', req_type, req, value, index, length))

    def control_out(self, req_type, req, value, index, data=b''):
        return self.submit(0, False, len(data), struct.pack('<BBHHH', req_type, req, value, index, len(data)), data)


def string(c, idx):
    if not idx:
        return ''
    st, d = c.control_in(0x80, 6, 0x0300 | idx, 0x0409, 255)
    return d[2:].decode('utf-16-le', 'replace') if st == 0 and len(d) > 2 else '(status %d)' % st


def walk_config(d):
    i = 0
    while i + 2 <= len(d) and d[i]:
        n, t = d[i], d[i + 1]
        b = d[i:i + n]
        if t == 4:
            print('    interface %d alt %d: class %02x/%02x/%02x, %d endpoints' % (b[2], b[3], b[5], b[6], b[7], b[4]))
        elif t == 5:
            kinds = ['control', 'iso', 'bulk', 'interrupt']
            print('      endpoint %02x %s max %d' % (b[2], kinds[b[3] & 3], struct.unpack_from('<H', b, 4)[0]))
        elif t == 0x24:
            pass
        else:
            print('    descriptor type %02x, %d bytes' % (t, n))
        i += n


def main():
    boot = float(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else 18
    log = os.path.join(ROOT, 'build', 'logs', 'usbip.log')
    env = dict(os.environ, SP404_TRACE=os.environ.get('SP404_TRACE', 'usb'))
    q = subprocess.Popen([os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe'),
                          '-M', 'sp404mk2,flash=%s,link=link,usbip=usbip' % os.path.join(ROOT, 'build', 'flash.bin'),
                          '-bios', os.path.join(ROOT, 'firmware', 'SP404MKII_APP1.bin'),
                          '-chardev', 'socket,id=link,host=127.0.0.1,port=%d,server=on,wait=on' % PORT,
                          '-chardev', 'socket,id=usbip,host=127.0.0.1,port=%d,server=on,wait=off' % USBIP,
                          '-drive', 'if=sd,index=0',
                          '-drive', 'if=sd,index=1,format=raw,snapshot=on,file=' + os.path.join(ROOT, 'build', 'emmc.img'),
                          '-nographic', '-serial', 'none', '-monitor', 'none', '-D', log],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    try:
        for _ in range(50):
            try:
                s = socket.create_connection(('127.0.0.1', PORT))
                break
            except OSError:
                time.sleep(0.2)
        lk = L.Link(s)
        time.sleep(boot)
        print('devices:', Client().devlist())
        c = Client()
        print('import:', c.import_())
        st, d = c.control_in(0x80, 6, 0x0100, 0, 18)
        print('device descriptor (status %d): %s' % (st, d.hex(' ')))
        if st == 0 and len(d) >= 18:
            print('  manufacturer %r, product %r, serial %r' % (string(c, d[14]), string(c, d[15]), string(c, d[16])))
        st, d = c.control_in(0x80, 6, 0x0200, 0, 9)
        if st == 0 and len(d) >= 4:
            total = struct.unpack_from('<H', d, 2)[0]
            st, d = c.control_in(0x80, 6, 0x0200, 0, total)
            print('configuration (status %d, %d bytes):' % (st, len(d)))
            walk_config(d)
        if '--config' in sys.argv:
            print('SET_CONFIGURATION 1:', c.control_out(0x00, 9, 1, 0)[0])
            time.sleep(1)
        lk.shot('usbip')
    finally:
        q.kill()


if __name__ == '__main__':
    main()
