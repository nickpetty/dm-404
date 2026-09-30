"""Make a Windows release package of dm-404 from a finished build.

    python tools/package.py [VERSION]

Builds nothing: run tools/build.sh, tools/build_fx.sh and
tools/build_frontend.sh first. Lays out build/dist/DM-404-VERSION-win64/:

    DM-404.exe                    the app
    qemu/qemu-system-arm.exe        the emulator (stripped of debug info)
    qemu/sp404fx.dll                the effects engine
    qemu/*.dll                      the MSYS2 runtime DLLs the two need
    VST3/DM-404 Link.vst3/        the DAW plugin
    README.md, LICENSE, LICENSES/   this project's
    THIRD-PARTY.txt                 what else is inside, its licences, sources
    third-party-licenses/           those licences' texts
    SOURCE.txt                      where the source of this build is

and zips it next to the folder. VERSION defaults to the frontend's project
version. Nothing of Roland's goes in: the user supplies the firmware.
"""
import os, re, shutil, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MSYS = r'C:\msys64'
UCRT_BIN = os.path.join(MSYS, 'ucrt64', 'bin')
OBJDUMP = os.path.join(UCRT_BIN, 'objdump.exe')
STRIP = os.path.join(UCRT_BIN, 'strip.exe')
QEMU_TAG = 'v11.1.2'


def version():
    if len(sys.argv) > 1:
        return sys.argv[1].lstrip('v')
    m = re.search(r'project\(DM404 VERSION ([0-9.]+)', open(os.path.join(ROOT, 'frontend', 'CMakeLists.txt')).read())
    return m.group(1)


def git(*args):
    return subprocess.check_output(['git', '-C', ROOT] + list(args), text=True).strip()


def imports(path):
    out = subprocess.check_output([OBJDUMP, '-p', path], text=True, errors='replace')
    return re.findall(r'DLL Name: (\S+)', out)


def runtime_dlls(binaries):
    """The MSYS2 DLLs the binaries need, followed through their own imports.
    Windows' own DLLs (not in the UCRT64 bin folder) are left out."""
    found, todo = {}, list(binaries)
    while todo:
        for name in imports(todo.pop()):
            src = os.path.join(UCRT_BIN, name)
            if name.lower() in found or not os.path.isfile(src):
                continue
            found[name.lower()] = src
            todo.append(src)
    return sorted(found.values())


# Where each bundled library comes from, its licence, and the licence files
# to carry (from the MSYS2 packages' share/licenses, when present).
LIBS = [
    ('libglib-2.0-0.dll libgio-2.0-0.dll libgobject-2.0-0.dll libgmodule-2.0-0.dll', 'GLib', 'LGPL-2.1-or-later', 'https://gitlab.gnome.org/GNOME/glib', 'glib2'),
    ('libpixman-1-0.dll', 'pixman', 'MIT', 'https://gitlab.freedesktop.org/pixman/pixman', 'pixman'),
    ('SDL2.dll', 'SDL2', 'Zlib', 'https://github.com/libsdl-org/SDL', 'SDL2'),
    ('zlib1.dll', 'zlib', 'Zlib', 'https://zlib.net', 'zlib'),
    ('libzstd.dll', 'Zstandard', 'BSD-3-Clause OR GPL-2.0', 'https://github.com/facebook/zstd', 'zstd'),
    ('libbz2-1.dll', 'bzip2', 'bzip2-1.0.6', 'https://sourceware.org/bzip2/', 'bzip2'),
    ('libiconv-2.dll', 'libiconv', 'LGPL-2.1-or-later', 'https://www.gnu.org/software/libiconv/', 'libiconv'),
    ('libintl-8.dll', 'gettext (libintl)', 'LGPL-2.1-or-later', 'https://www.gnu.org/software/gettext/', 'gettext-runtime'),
    ('libpcre2-8-0.dll', 'PCRE2', 'BSD-3-Clause', 'https://github.com/PCRE2Project/pcre2', 'pcre2'),
    ('libffi-8.dll', 'libffi', 'MIT', 'https://github.com/libffi/libffi', 'libffi'),
    ('libncursesw6.dll', 'ncurses', 'MIT (X11)', 'https://invisible-island.net/ncurses/', 'ncurses'),
    ('libwinpthread-1.dll', 'mingw-w64 winpthreads', 'MIT AND BSD-3-Clause', 'https://www.mingw-w64.org', 'libwinpthread'),
    ('libgcc_s_seh-1.dll libstdc++-6.dll', 'GCC runtime', 'GPL-3.0 with the GCC Runtime Library Exception', 'https://gcc.gnu.org', 'gcc-libs'),
]


def library_notes(dlls, dest):
    """THIRD-PARTY entries for the bundled DLLs, and their licence files."""
    names = {os.path.basename(d).lower() for d in dlls}
    lines, missing = [], set(names)
    lic_root = os.path.join(MSYS, 'ucrt64', 'share', 'licenses')
    for files, what, lic, url, pkg in LIBS:
        mine = [f for f in files.split() if f.lower() in names]
        if not mine:
            continue
        missing -= {f.lower() for f in mine}
        lines.append('  %-26s %s, %s\n  %-26s source: %s' % (', '.join(mine), what, lic, '', url))
        src = os.path.join(lic_root, pkg)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(dest, pkg), dirs_exist_ok=True)
    for m in sorted(missing):
        lines.append('  %-26s (see the MSYS2 package it comes from: https://packages.msys2.org)' % m)
    return '\n'.join(lines)


def main():
    ver = version()
    commit = git('rev-parse', 'HEAD')
    name = 'DM-404-%s-win64' % ver
    dist = os.path.join(ROOT, 'build', 'dist')
    out = os.path.join(dist, name)
    app = os.path.join(ROOT, 'build', 'frontend', 'DM404_artefacts', 'Release', 'DM-404.exe')
    vst3 = os.path.join(ROOT, 'build', 'frontend', 'DM404Link_artefacts', 'Release', 'VST3', 'DM-404 Link.vst3')
    qemu = os.path.join(ROOT, 'build', 'qemu', 'qemu-system-arm.exe')
    fx = os.path.join(ROOT, 'build', 'fx', 'sp404fx.dll')
    for f in (app, vst3, qemu, fx):
        if not os.path.exists(f):
            sys.exit('missing %s: build first' % f)

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'qemu'))
    shutil.copy2(app, out)
    shutil.copytree(vst3, os.path.join(out, 'VST3', os.path.basename(vst3)))

    # The emulator, without its debug info (100 MB of it), and the engine.
    q = os.path.join(out, 'qemu', 'qemu-system-arm.exe')
    subprocess.check_call([STRIP, '--strip-debug', '-o', q, qemu])
    shutil.copy2(fx, os.path.join(out, 'qemu'))
    dlls = runtime_dlls([qemu, fx])
    for d in dlls:
        shutil.copy2(d, os.path.join(out, 'qemu'))

    # This project's documents and licences.
    for f in ('README.md', 'LICENSE'):
        shutil.copy2(os.path.join(ROOT, f), out)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(out, 'LICENSES'))

    # Everything else that is inside, with its licence texts.
    tp = os.path.join(out, 'third-party-licenses')
    os.makedirs(tp)
    carried = [
        (os.path.join(ROOT, 'third_party', 'qemu', 'COPYING'), 'QEMU-COPYING.txt'),
        (os.path.join(ROOT, 'third_party', 'qemu', 'LICENSE'), 'QEMU-LICENSE.txt'),
        (os.path.join(ROOT, 'third_party', 'DaisySP', 'LICENSE'), 'DaisySP-LICENSE.txt'),
        (os.path.join(ROOT, 'third_party', 'DaisySP', 'DaisySP-LGPL', 'LICENSE'), 'DaisySP-LGPL-LICENSE.txt'),
    ]
    juce = None
    cache = os.path.join(ROOT, 'build', 'frontend', 'CMakeCache.txt')
    m = re.search(r'^JUCE_DIR:PATH=(.+)$', open(cache).read(), re.M) if os.path.isfile(cache) else None
    if m and m.group(1).strip():
        juce = m.group(1).strip()
    elif os.path.isdir(os.path.join(ROOT, 'build', 'frontend', '_deps', 'juce-src')):
        juce = os.path.join(ROOT, 'build', 'frontend', '_deps', 'juce-src')
    if juce:
        carried += [(os.path.join(juce, 'LICENSE.md'), 'JUCE-LICENSE.md'),
                    (os.path.join(juce, 'modules', 'juce_audio_processors_headless', 'format_types', 'VST3_SDK', 'LICENSE.txt'),
                     'VST3_SDK-LICENSE.txt')]
    for src, dst in carried:
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(tp, dst))
        else:
            print('note: no %s' % src)
    libs = library_notes(dlls, tp)

    open(os.path.join(out, 'THIRD-PARTY.txt'), 'w', newline='\r\n').write('''\
What is in this package besides dm-404's own code, and under which
licences. The texts are in third-party-licenses/ (and LICENSES/).

  DM-404.exe, VST3/        built with JUCE 9 (AGPLv3 / commercial; used
                             under the AGPLv3) and the Steinberg VST3 SDK (MIT),
                             and FatFs R0.15a (ChaN, BSD-style)
  qemu/qemu-system-arm.exe   QEMU %s (GPL-2.0) with dm-404's machine
                             (GPL-2.0-or-later); source: https://www.qemu.org
                             and this project's repository (SOURCE.txt)
  qemu/sp404fx.dll           dm-404's effects engine (MIT) with DaisySP (MIT)
                             and DaisySP-LGPL (LGPL-2.1): its whole source is in
                             this project's repository and
                             https://github.com/electro-smith/DaisySP, so it can
                             be rebuilt or relinked (LGPL-2.1 section 6)

  Runtime libraries from MSYS2 (UCRT64), used by the emulator, unmodified:
%s

Not included: Roland's SP-404MKII firmware (each user supplies their own),
and the Windows MIDI Services SDK (Windows provides it, or the user does).
"Roland" and "SP-404" are trademarks of Roland Corporation; dm-404 is not
affiliated with or endorsed by Roland.
''' % (QEMU_TAG, libs))

    open(os.path.join(out, 'SOURCE.txt'), 'w', newline='\r\n').write('''\
The complete source of this build (GPL-2.0 / AGPL-3.0 / LGPL-2.1):

  dm-404    https://github.com/nickpetty/dm-404
              commit %s
  QEMU        %s, https://gitlab.com/qemu-project/qemu (tools/qemu_sync.sh
              applies core/qemu/patches and overlays core/qemu)
  DaisySP     https://github.com/electro-smith/DaisySP (with its DaisySP-LGPL
              submodule)
  JUCE        9.0.2, https://github.com/juce-framework/JUCE
  MSYS2 libraries: https://packages.msys2.org (sources linked from each package)

If you cannot get any of these, ask the dm-404 maintainers for a copy
(written offer, valid for three years from this release).
''' % (commit, QEMU_TAG))

    zpath = out + '.zip'
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _, files in os.walk(out):
            for f in files:
                full = os.path.join(base, f)
                z.write(full, os.path.join(name, os.path.relpath(full, out)))
    size = os.path.getsize(zpath) / 1e6
    print('packaged %s (%d runtime DLLs), %s (%.1f MB)' % (out, len(dlls), zpath, size))


if __name__ == '__main__':
    main()
