/*
 * The BMC's sound engine: effects and the final mix, from core/fx
 * (sp404fx.dll), loaded at run time.
 *
 * The firmware drives the BMC's effects with DT1 SysEx over the BMC UART
 * (USB-MIDI framed: CIN 4 starts or continues a message, 5-7 end it) and
 * sends it dry buses on SAI1 TX line 3. This module hands both to the
 * engine. It is looked for next to the emulator's executable; SP404_FX
 * names another file, or "off" to keep the mix dry.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/sp404/sp404.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#define SP404FX_VERSION 4      /* as core/fx/sp404fx.h */

static void *fx_open(const char *path)
{
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

static void *fx_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress(lib, name);
#else
    return dlsym(lib, name);
#endif
}

static char *fx_default_path(void)
{
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));

    if (n == 0 || n >= sizeof(exe)) {
        return g_strdup("sp404fx.dll");
    }
    g_autofree char *dir = g_path_get_dirname(exe);
    return g_build_filename(dir, "sp404fx.dll", NULL);
#else
    return g_strdup("libsp404fx.so");
#endif
}

void sp404_fx_init(SP404Fx *e)
{
    const char *env = getenv("SP404_FX");
    g_autofree char *path = NULL;
    void *lib;
    int (*version)(void);
    void *(*make)(int);

    memset(e, 0, sizeof(*e));
    if (env && !strcmp(env, "off")) {
        return;
    }
    path = env ? g_strdup(env) : fx_default_path();
    lib = fx_open(path);
    if (!lib) {
        qemu_log("sp404-fx: no effects engine at %s: the mix stays dry\n", path);
        return;
    }
    version = fx_sym(lib, "sp404fx_version");
    make = fx_sym(lib, "sp404fx_new");
    e->dt1 = fx_sym(lib, "sp404fx_dt1");
    e->process = fx_sym(lib, "sp404fx_process");
    e->process_buses = fx_sym(lib, "sp404fx_process_buses");
    e->slot = fx_sym(lib, "sp404fx_slot");
    if (!version || !make || !e->dt1 || !e->process || !e->process_buses ||
        version() != SP404FX_VERSION) {
        qemu_log("sp404-fx: %s is not a version %d engine\n", path,
                 SP404FX_VERSION);
        e->dt1 = NULL;
        e->process = NULL;
        return;
    }
    e->engine = make(48000);
    qemu_log("sp404-fx: effects engine %s\n", path);
}

/* A USB-MIDI packet from the firmware to the BMC. */
void sp404_fx_midi(SP404Fx *e, const uint8_t *p)
{
    static const uint8_t len[8] = { [4] = 3, [5] = 1, [6] = 2, [7] = 3 };
    unsigned cin = p[0] & 0xf;
    const uint8_t *m;
    unsigned n;

    if (!e->engine || cin < 4 || cin > 7) {
        return;
    }
    for (unsigned i = 0; i < len[cin]; i++) {
        if (p[1 + i] == 0xf0) {
            e->sysex_len = 0;
        }
        if (e->sysex_len < sizeof(e->sysex)) {
            e->sysex[e->sysex_len++] = p[1 + i];
        }
    }
    if (cin == 4) {
        return;
    }
    /* F0 41 10 00 00 00 00 08 12 a a a a d.. sum F7 */
    m = e->sysex;
    n = e->sysex_len;
    e->sysex_len = 0;
    if (n < 16 || m[0] != 0xf0 || m[1] != 0x41 || m[8] != 0x12 ||
        m[n - 1] != 0xf7) {
        return;
    }
    SP404_TRACE("fx", "DT1 %02x %02x %02x %02x <- %u bytes", m[9], m[10],
                m[11], m[12], n - 15);
    e->dt1(e->engine, m + 9, m + 13, n - 15);
}

bool sp404_fx_active(SP404Fx *e)
{
    return e->engine != NULL;
}

void sp404_fx_process(SP404Fx *e, const float *stems, const float *in,
                      float *out, float *buses)
{
    e->process_buses(e->engine, stems, in, out, buses, 1);
}
