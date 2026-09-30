"""The computer's MIDI ports as classic (WinMM) Windows apps see them, and
optionally a round trip through one of them.

    python tools/midiports.py                     list the ports
    python tools/midiports.py listen NAME SECS    print what arrives from NAME
    python tools/midiports.py note NAME NOTE      send note-on/off (channel 1) to NAME
"""
import ctypes, sys, time
from ctypes import wintypes

w = ctypes.windll.winmm


class INCAPS(ctypes.Structure):
    _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                ('szPname', wintypes.WCHAR * 32), ('dwSupport', wintypes.DWORD)]


class OUTCAPS(ctypes.Structure):
    _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                ('szPname', wintypes.WCHAR * 32), ('wTechnology', wintypes.WORD), ('wVoices', wintypes.WORD),
                ('wNotes', wintypes.WORD), ('wChannelMask', wintypes.WORD), ('dwSupport', wintypes.DWORD)]


def ins():
    out = []
    for i in range(w.midiInGetNumDevs()):
        c = INCAPS()
        w.midiInGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c))
        out.append(c.szPname)
    return out


def outs():
    out = []
    for i in range(w.midiOutGetNumDevs()):
        c = OUTCAPS()
        w.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c))
        out.append(c.szPname)
    return out


MidiInProc = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t)


def main():
    args = sys.argv[1:]
    if not args:
        print('MIDI in :', ins())
        print('MIDI out:', outs())
    elif args[0] == 'listen':
        idx = ins().index(args[1])
        got = []

        def proc(h, msg, inst, p1, p2):
            if msg == 0x3C3:                       # MIM_DATA
                got.append((time.time(), p1 & 0xffffff))
        cb = MidiInProc(proc)
        h = wintypes.HANDLE()
        r = w.midiInOpen(ctypes.byref(h), idx, cb, 0, 0x30000)   # CALLBACK_FUNCTION
        if r:
            print('midiInOpen failed', r)
            return
        w.midiInStart(h)
        time.sleep(float(args[2]))
        w.midiInStop(h)
        w.midiInClose(h)
        for t, m in got:
            if (m & 0xff) != 0xf8:
                print('%.3f  %02x %02x %02x' % (t, m & 0xff, (m >> 8) & 0xff, (m >> 16) & 0xff))
        print('%d messages' % len(got))
    elif args[0] == 'note':
        idx = outs().index(args[1])
        note = int(args[2])
        h = wintypes.HANDLE()
        r = w.midiOutOpen(ctypes.byref(h), idx, 0, 0, 0)
        if r:
            print('midiOutOpen failed', r)
            return
        w.midiOutShortMsg(h, 0x90 | (note << 8) | (110 << 16))
        time.sleep(0.2)
        w.midiOutShortMsg(h, 0x80 | (note << 8))
        time.sleep(0.2)
        w.midiOutClose(h)
        print('sent note', note)


if __name__ == '__main__':
    main()
