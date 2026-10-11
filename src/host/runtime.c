#include "runtime.h"

#include "log.h"

#include "thunk.h"

#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct image **g_preload_src;
static int g_npreload_src;

static uint32_t g_argc;
static gaddr_t g_argv, g_envp, g_apple, g_vars;
static gaddr_t g_stack_lo, g_stack_hi;
static struct image *g_images[64];
static int g_nimages;

void runtime_set_process_args(int argc, gaddr_t argv, gaddr_t envp, gaddr_t apple) {
    g_argc = (uint32_t)argc;
    g_argv = argv;
    g_envp = envp;
    g_apple = apple;
}

void runtime_set_main_stack(gaddr_t lo, gaddr_t hi) {
    g_stack_lo = lo;
    g_stack_hi = hi;
}

uint32_t runtime_argc(void) { return g_argc; }
gaddr_t runtime_argv(void) { return g_argv; }
gaddr_t runtime_envp(void) { return g_envp; }
gaddr_t runtime_apple(void) { return g_apple; }
gaddr_t runtime_program_vars(void) { return g_vars; }

// Guest runtime libraries live in <dxhost dir>/guest (copied there by the build).
static gaddr_t preload_resolve(const char *name, const char *from, bool weak) {
    (void)from;
    (void)weak;
    gaddr_t a;
    for (int i = 0; i < g_npreload_src; i++)
        if ((a = macho_export(g_preload_src[i], name, NULL))) return a;
    return hostsym_lookup(name);
}

void runtime_preload_guest_libs(struct image **list, int *count) {
    if (getenv("DXH_NO_GUEST_LIBS")) return;
    char exe[PATH_MAX], dir[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) fatal("cannot determine dxhost path");
    char *real = realpath(exe, NULL);
    snprintf(dir, sizeof dir, "%s", dirname(real));
    free(real);
    char stdcxx[PATH_MAX], unwind[PATH_MAX];
    snprintf(stdcxx, sizeof stdcxx, "%s/guest/libstdc++-i386.a", dir);
    snprintf(unwind, sizeof unwind, "%s/guest/libunwind-i386.a", dir);
    if (access(stdcxx, R_OK) != 0) {
        LOGE("guest libstdc++ not found at %s; C++ guest code will fail", stdcxx);
        return;
    }
    char fast[PATH_MAX];
    snprintf(fast, sizeof fast, "%s/guest/libfast-i386.o", dir);
    g_preload_src = list;
    g_npreload_src = *count;
    // libfast first: its libc fast paths are exported in place of host thunks
    // (DXH_NO_GUEST_FAST=1 disables them for comparison)
    const char *paths[3];
    int npaths = 0;
    if (access(fast, R_OK) == 0 && !getenv("DXH_NO_GUEST_FAST")) paths[npaths++] = fast;
    paths[npaths++] = stdcxx;
    paths[npaths++] = unwind;
    struct image *img = macho_load_objects("/usr/lib/libstdc++.6.dylib", paths, npaths, preload_resolve);
    if (!img) fatal("failed to link guest libstdc++");
    list[(*count)++] = img;
    macho_register(img);
}

void runtime_images_loaded(struct image **list, int count) {
    g_nimages = count;
    memcpy(g_images, list, sizeof(struct image *) * count);
    // struct ProgramVars { mh; int *NXArgcPtr; char ***NXArgvPtr; char ***environPtr; char **__prognamePtr; }
    gaddr_t storage = gheap_calloc(1, 4 * 4);
    gwrite32(storage, g_argc);
    gwrite32(storage + 4, g_argv);
    gwrite32(storage + 8, g_envp);
    gwrite32(storage + 12, g_argc ? gread32(g_argv) : 0);
    g_vars = gheap_calloc(1, 5 * 4);
    gwrite32(g_vars, list[0]->header);
    gwrite32(g_vars + 4, storage);
    gwrite32(g_vars + 8, storage + 4);
    gwrite32(g_vars + 12, storage + 8);
    gwrite32(g_vars + 16, storage + 12);
}

int runtime_image_count(void) { return g_nimages; }
struct image *runtime_image(int i) { return i >= 0 && i < g_nimages ? g_images[i] : NULL; }

gaddr_t runtime_main_stack_lo(void) { return g_stack_lo; }
gaddr_t runtime_main_stack_hi(void) { return g_stack_hi; }
