#include "runtime.h"

#include "log.h"

#include <mach-o/loader.h>
#include <string.h>

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

void runtime_preload_guest_libs(struct image **list, int *count) {
    (void)list;
    (void)count;
    // TODO: guest libstdc++ / libunwind once built.
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
