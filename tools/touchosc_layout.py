"""Make the TouchOSC layout for DM-404's OSC control.

    python tools/touchosc_layout.py [OUT]

Writes frontend/Resources/DM-404.tosc (or OUT): the panel's keys, pads and
knobs where the app has them, each on the address
frontend/Source/OscControl.h reads, with the LEDs and knobs following what
the app sends back. Positions are the app's (PanelComponent.cpp, pixel
centres in a 570 x 915 photo of the panel); keep them in step.

A .tosc file is zlib-compressed XML ("lexml" version 5). The node shapes
here copy files saved by TouchOSC's own editor: the property sets per
control type, the value lists and the OSC message blocks.
"""
import os, sys, uuid, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCALE = 1.2                      # photo pixels to layout points
W, H = round(570 * SCALE), round(915 * SCALE)

BG = (0.137, 0.153, 0.212)       # the default background's navy
KEY = (0.85, 0.36, 0.20)         # the panel's orange
RED = (1.0, 0.16, 0.23)
PAD = (0.42, 0.43, 0.46)
WHITE = (1, 1, 1)


def ident(name):
    out = ''.join(ch if ch.isalnum() else '_' for ch in name.lower())
    while '__' in out:
        out = out.replace('__', '_')
    return out.strip('_')


def cdata(s):
    return '<![CDATA[%s]]>' % s


def num(v):
    v = round(float(v), 4)
    return str(int(v)) if v == int(v) else repr(v)


def prop(t, key, value):
    if t == 'c':
        r, g, b, a = (list(value) + [1])[:4]
        v = '<r>%s</r><g>%s</g><b>%s</b><a>%s</a>' % (num(r), num(g), num(b), num(a))
    elif t == 'r':
        v = '<x>%s</x><y>%s</y><w>%s</w><h>%s</h>' % tuple(num(x) for x in value)
    elif t == 's':
        v = cdata(value)
    elif t == 'b':
        v = '1' if value else '0'
    else:
        v = num(value)
    return "<property type='%s'><key>%s</key><value>%s</value></property>" % (t, cdata(key), v)


def value(key, default, locked_default_current=0):
    return ('<value><key>%s</key><locked>0</locked><lockedDefaultCurrent>%d</lockedDefaultCurrent>'
            '<default>%s</default><defaultPull>0</defaultPull></value>' % (cdata(key), locked_default_current, cdata(default)))


def partial(t, conv, v, lo=0, hi=1):
    return ('<partial><type>%s</type><conversion>%s</conversion><value>%s</value>'
            '<scaleMin>%s</scaleMin><scaleMax>%s</scaleMax></partial>' % (t, conv, cdata(v), num(lo), num(hi)))


def osc(path, var='x', send=True, receive=True):
    return ('<osc><enabled>1</enabled><send>%d</send><receive>%d</receive><feedback>0</feedback>'
            '<noDuplicates>0</noDuplicates><connections>1111111111</connections>'
            '<triggers><trigger><var>%s</var><condition>ANY</condition></trigger></triggers>'
            '<path>%s</path><arguments>%s</arguments></osc>'
            % (send, receive, cdata(var), partial('CONSTANT', 'STRING', path), partial('VALUE', 'FLOAT', var)))


_ids = iter(range(1 << 30))


def node(kind, props, values, messages='', children=''):
    # IDs from the node's place, so the file only changes when the layout does.
    s = "<node ID='%s' type='%s'><properties>%s</properties><values>%s</values>" % (
        uuid.uuid5(uuid.NAMESPACE_URL, 'dm-404/touchosc/%d' % next(_ids)), kind, ''.join(sorted(props, key=lambda p: p.split('CDATA[')[1])), ''.join(values))
    if messages:
        s += '<messages>%s</messages>' % messages
    if children:
        s += '<children>%s</children>' % children
    return s + '</node>'


def common(name, frame, colour, interactive, outline=True, background=True, corner=1, shape=1):
    return [prop('b', 'background', background), prop('c', 'color', colour), prop('f', 'cornerRadius', corner),
            prop('r', 'frame', frame), prop('b', 'grabFocus', interactive), prop('b', 'interactive', interactive),
            prop('b', 'locked', False), prop('s', 'name', name), prop('i', 'orientation', 0),
            prop('b', 'outline', outline), prop('i', 'outlineStyle', 1), prop('i', 'pointerPriority', 0),
            prop('i', 'shape', shape), prop('b', 'visible', True)]


def button(name, frame, colour, path=None, interactive=True, outline=True, background=True, script=None,
           receive_only=False):
    props = common(name, frame, colour, interactive, outline, background) + [
        prop('i', 'buttonType', 0), prop('b', 'press', True), prop('b', 'release', True),
        prop('b', 'valuePosition', False)]
    if script:
        props.append(prop('s', 'script', script))
    msg = osc(path, send=not receive_only) if path else ''
    return node('BUTTON', props, [value('x', '0'), value('touch', 'false')], msg)


def label(name, frame, text, size=12, colour=WHITE):
    props = common(name, frame, (0, 0, 0, 0), False, outline=False, background=False) + [
        prop('i', 'font', 0), prop('i', 'textAlignH', 2), prop('i', 'textAlignV', 2), prop('b', 'textClip', True),
        prop('c', 'textColor', colour), prop('i', 'textLength', 0), prop('i', 'textSize', size)]
    return node('LABEL', props, [value('text', text, 1), value('touch', 'false')])


def box(name, frame, colour, corner=1):
    return node('BOX', common(name, frame, colour, False, outline=False, corner=corner), [value('touch', 'false')])


def radial(name, frame, path):
    props = common(name, frame, KEY, True, shape=2) + [
        prop('b', 'centered', False), prop('b', 'grid', True), prop('c', 'gridColor', (0, 0, 0, 0.25)),
        prop('i', 'gridSteps', 20), prop('b', 'inverted', False), prop('i', 'response', 0),
        prop('i', 'responseFactor', 100)]
    return node('RADIAL', props, [value('x', '0.5'), value('touch', 'false')], osc(path))


def encoder(name, frame, path):
    props = common(name, frame, KEY, True, shape=2) + [
        prop('b', 'cursor', True), prop('i', 'cursorDisplay', 0), prop('b', 'grid', True),
        prop('c', 'gridColor', (0, 0, 0, 0.25)), prop('i', 'gridSteps', 24), prop('i', 'response', 0),
        prop('i', 'responseFactor', 100)]
    return node('ENCODER', props, [value('x', '0'), value('y', '0'), value('touch', 'false')],
                osc(path, receive=False))


def at(cx, cy, w, h):
    """A frame from a pixel centre and size in the photo."""
    return (round((cx - w / 2) * SCALE), round((cy - h / 2) * SCALE), round(w * SCALE), round(h * SCALE))


# The pad's LED takes level, r, g, b; the level lights it (x), the script
# colours it.
PAD_SCRIPT = """function onReceiveOSC(message, connections)
  local a = message[2]
  if a and #a >= 4 then
    self.color = Color(a[2].value, a[3].value, a[4].value, 0.85)
  end
end
"""

SUBS = {
    'PATTERN SELECT': 'UNDO', 'START/END': 'CHOP', 'PITCH/SPEED': 'ENVELOPE', 'LOOP': 'PING-PONG',
    'ROLL': 'ROLL SET', 'EXIT': 'PATTERN STOP', 'REMAIN': 'CURRENT PAD', 'BUS FX': 'MUTE BUS', 'HOLD': 'PAUSE',
    'EXT SOURCE': 'INPUT SETTING', 'SUB PAD': 'PROJECT',
    '1': 'FIXED VELOCITY', '2': '16 VELOCITY', '3': 'CUE', '4': 'CHROMATIC', '5': 'EXCHANGE', '6': 'INIT PARAM',
    '7': 'PAD LINK', '8': 'MUTE GROUPS', '9': 'METRONOME', '10': 'COUNT-IN', '11': 'TAP TEMPO', '12': 'GAIN',
    '13': 'UTILITY', '14': 'IMPORT/EXPORT', '15': 'PAD SETTING', '16': 'EFX SETTING'}


def key(name, cx, cy, w, h, big=False):
    """A key: the button, its LED over it, its name and SHIFT function."""
    f = at(cx, cy, w, h)
    red = name in ('REC', 'RESAMPLE', 'DEL')
    out = button(ident(name), f, (0.3, 0.31, 0.34), '/dm404/key/' + ident(name))
    out += button(ident(name) + '_led', f, RED if red else KEY, '/dm404/led/' + ident(name), interactive=False,
                  outline=False, background=False, receive_only=True)
    words = name.split(' ') if (' ' in name and not big) else [name]
    size = 15 if big else 10
    if len(words) == 1:
        out += label(ident(name) + '_name', f, name, size)
    else:
        top = (f[0], f[1], f[2], f[3] / 2)
        bottom = (f[0], f[1] + f[3] / 2, f[2], f[3] / 2)
        out += label(ident(name) + '_name1', top, words[0], size)
        out += label(ident(name) + '_name2', bottom, ' '.join(words[1:]), size)
    if name in SUBS:
        out += label(ident(name) + '_sub', (f[0] - 10, f[1] + f[3] + 1, f[2] + 20, 14), SUBS[name], 9)
    return out


def layout():
    kids = []
    # The round display window: the app shows the screen.
    kids.append(box('display', at(285, 268, 228, 228), (0, 0, 0), corner=114 * SCALE))
    kids.append(label('display_text', at(285, 268, 200, 30), 'DM-404', 18, (0.6, 0.62, 0.68)))
    # Knobs.
    for name, cx in (('VOLUME', 100), ('CTRL 1', 222), ('CTRL 2', 345), ('CTRL 3', 470)):
        kids.append(radial(ident(name), at(cx, 106, 62, 62), '/dm404/knob/' + ident(name)))
        kids.append(label(ident(name) + '_title', at(cx, 64, 90, 16), name, 10))
    # Effect keys around the display.
    for name, cx, cy in (('FILTER+DRIVE', 123, 207), ('RESONATOR', 117, 260), ('DELAY', 123, 315),
                         ('ISOLATOR', 447, 207), ('DJFX LOOPER', 453, 260), ('MFX', 447, 315)):
        kids.append(key(name, cx, cy, 90, 40, big=True))
    # Pattern sequencer and sample edit; VALUE.
    for name, cx in zip(('PATTERN SELECT', 'PATTERN EDIT', 'RECORD SETTING', 'START/END', 'PITCH/SPEED', 'MARK'),
                        (85, 145, 205, 285, 345, 405)):
        kids.append(key(name, cx, 400, 50, 30))
    kids.append(encoder('value', at(483, 393, 48, 48), '/dm404/value'))
    kids.append(button('value_push', at(483, 352, 60, 18), (0.3, 0.31, 0.34), '/dm404/value/push'))
    kids.append(label('value_push_name', at(483, 352, 60, 18), 'PUSH', 9))
    # Sampling and sample mode.
    for name, cx in zip(('DEL', 'REC', 'RESAMPLE'), (85, 145, 205)):
        kids.append(key(name, cx, 454, 44, 30))
    left, w, gap = 242.0, 46.0, (267.0 - 5 * 46.0) / 4
    for i, name in enumerate(('BPM SYNC', 'GATE', 'LOOP', 'REVERSE', 'ROLL')):
        kids.append(key(name, left + w / 2 + i * (w + gap), 454, w, 30))
    # EXIT, COPY, REMAIN, the banks and SHIFT.
    for i, (name, cx) in enumerate(zip(('EXIT', 'COPY', 'REMAIN', 'A/F', 'B/G', 'C/H', 'D/I', 'E/J', 'SHIFT'),
                                       (85, 145, 205, 264, 309, 355, 400, 445, 491))):
        kids.append(key(name, cx, 507, 50 if i < 3 else 36, 30))
    # The pads and the column beside them.
    pad_x, pad_y = (97, 191, 285, 378), (585, 666, 747, 828)
    for r, name in enumerate(('BUS FX', 'HOLD', 'EXT SOURCE', 'SUB PAD')):
        kids.append(key(name, 472, pad_y[r] - 4, 72, 62, big=True))
    for r in range(4):
        for c in range(4):
            n = str(r * 4 + c + 1)
            f = at(pad_x[c], pad_y[r] - 4, 78, 62)
            kids.append(button('pad' + n, f, PAD, '/dm404/pad/' + n))
            inner = (f[0] + 6, f[1] + 6, f[2] - 12, f[3] - 12)
            kids.append(button('pad' + n + '_led', inner, KEY, '/dm404/led/pad/' + n, interactive=False,
                               outline=False, background=False, script=PAD_SCRIPT, receive_only=True))
            kids.append(label('pad' + n + '_number', (f[0] + f[2] - 34, f[1] + 2, 30, 20), n, 14))
            kids.append(label('pad' + n + '_sub', (f[0], f[1] + f[3] + 2, f[2], 14), SUBS[n], 9))
    root = common('DM-404', (0, 0, W, H), BG, False, outline=False)
    root = [p for p in root if 'CDATA[name]' not in p]
    return ("<?xml version='1.0' encoding='UTF-8'?><lexml version='5'>"
            + node('GROUP', root, [value('touch', 'false')], children=''.join(kids)) + '</lexml>')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'frontend', 'Resources', 'DM-404.tosc')
    xml = layout().encode('utf-8')
    open(out, 'wb').write(zlib.compress(xml, 9))
    print('wrote %s (%d bytes, %d controls)' % (out, os.path.getsize(out), xml.count(b"<node ") - 1))


if __name__ == '__main__':
    main()
