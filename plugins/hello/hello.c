// hello.c — Phase 10d.0: first-party "Hello" plugin (proof-of-bundling).
//
// Validates the Phase 10a plugin runtime end-to-end: pure-C plugin that
// implements the 5 ABI exports, builds via the in-tree CMake target,
// installs to /usr/lib/<exec_prefix>/plugins/hello/hello.so via
// `make install`, and loads on next launch.
//
// The "Hello" command does the simplest possible thing: it tags the
// plugin as alive by mutating a static counter readable via
// messageProc. A future iteration can call back into the host for an
// actual UI affordance once we ship the host-side scintilla-send
// helper (filed as v4.1).

#include <stdio.h>
#include <string.h>

#include "../linuxplugin.h"

static LinuxNppData g_npp;
static int          g_helloCount = 0;

static void cmdHello(void)
{
    ++g_helloCount;
    fprintf(stderr, "[hello plugin] Hello! (count=%d)\n", g_helloCount);
}

static void cmdAbout(void)
{
    fprintf(stderr,
        "[hello plugin] First-party scaffolding plugin (Phase 10d.0).\n"
        "[hello plugin] Validates the Linux Plugin ABI + dlopen loader.\n");
}

static LinuxFuncItem g_funcs[2] = {
    { "&Hello",  cmdHello, 0, 0, 0 },
    { "&About",  cmdAbout, 0, 0, 0 },
};

const char* getName(void) { return "Hello"; }

LinuxFuncItem* getFuncsArray(int* count)
{
    *count = sizeof(g_funcs) / sizeof(g_funcs[0]);
    return g_funcs;
}

void setInfo(LinuxNppData data)
{
    g_npp = data;
}

void beNotified(void* notification)
{
    /* No-op — we don't react to editor events in this MVP. */
    (void)notification;
}

intptr_t messageProc(unsigned int msg, uintptr_t wParam, intptr_t lParam)
{
    /* Test hook: msg = 0xFFFFFF00 returns the hello-counter. */
    if (msg == 0xFFFFFF00) return (intptr_t)g_helloCount;
    (void)wParam; (void)lParam;
    return 0;
}
