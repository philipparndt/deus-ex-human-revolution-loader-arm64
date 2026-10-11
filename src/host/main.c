// dxhost: runs a 32-bit (i386) macOS game binary on a 64-bit host.
//
//   dxhost "/path/to/Deus Ex Human Revolution - Directors Cut.app" [args...]
#include "cpu.h"
#include "crash.h"
#include "gmem.h"
#include "hobj.h"
#include "log.h"
#include "macho.h"
#include "runtime.h"
#include "thunk.h"

void objc_bridge_register_images(void);
void objc_install_main_bundle(void);

#include <libgen.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

#define MAIN_STACK_SIZE (16u << 20)

static struct image *loaded[32];
static int nloaded;

static gaddr_t resolve_symbol(const char *name, const char *from_lib, bool weak_import) {
    (void)from_lib;
    (void)weak_import;
    // Guest images in load order (main executable first), then host thunks.
    gaddr_t a;
    for (int i = 0; i < nloaded; i++)
        if ((a = macho_export(loaded[i], name, NULL))) return a;
    return hostsym_lookup(name);
}

static char *find_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) fatal("cannot find %s", path);
    if (!S_ISDIR(st.st_mode)) return realpath(path, NULL);
    // App bundle: Contents/MacOS/<name without .app>
    char buf[PATH_MAX], tmp[PATH_MAX];
    strlcpy(tmp, path, sizeof tmp);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;
    char *name = basename(tmp);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    snprintf(buf, sizeof buf, "%s/Contents/MacOS/%s", path, name);
    return realpath(buf, NULL);
}

static void load_dependencies(struct image *img, const char *exe_dir) {
    for (int i = 0; i < img->ndylibs; i++) {
        const char *dep = img->dylibs[i];
        char path[PATH_MAX];
        if (strncmp(dep, "@executable_path/", 17) == 0)
            snprintf(path, sizeof path, "%s/%s", exe_dir, dep + 17);
        else if (strncmp(dep, "@loader_path/", 13) == 0) {
            char dir[PATH_MAX];
            strlcpy(dir, img->path, sizeof dir);
            snprintf(path, sizeof path, "%s/%s", dirname(dir), dep + 13);
        } else
            continue; // system library: provided by the host layer
        bool already = false;
        for (int j = 0; j < nloaded; j++)
            if (loaded[j]->install_name && strcmp(loaded[j]->install_name, dep) == 0) already = true;
        if (already) continue;
        struct image *d = macho_load(path);
        if (!d) {
            LOGE("dependency %s of %s could not be loaded", dep, img->path);
            continue;
        }
        if (!d->install_name) d->install_name = strdup(dep);
        loaded[nloaded++] = d;
        macho_register(d);
        load_dependencies(d, exe_dir);
    }
}

// Build the initial i386 process stack: argc, argv[], 0, envp[], 0, apple[], 0.
static gaddr_t build_stack(gaddr_t stack_hi, int argc, char **argv, const char *exe_path) {
    int envc = 0;
    for (char **e = environ; *e; e++)
        if (strncmp(*e, "DYLD_", 5) != 0) envc++;
    char apple0[PATH_MAX + 32];
    snprintf(apple0, sizeof apple0, "executable_path=%s", exe_path);

    gaddr_t sp = stack_hi;
    gaddr_t *gargv = calloc(argc + 1, sizeof(gaddr_t));
    gaddr_t *genv = calloc(envc + 1, sizeof(gaddr_t));
#define PUSH_STR(s)                                                                                \
    ({                                                                                             \
        size_t l_ = strlen(s) + 1;                                                                 \
        sp -= (uint32_t)l_;                                                                        \
        memcpy(g2h(sp), (s), l_);                                                                  \
        sp;                                                                                        \
    })
    gaddr_t gapple = PUSH_STR(apple0);
    int k = 0;
    for (char **e = environ; *e; e++)
        if (strncmp(*e, "DYLD_", 5) != 0) genv[k++] = PUSH_STR(*e);
    for (int i = argc - 1; i >= 0; i--) gargv[i] = PUSH_STR(i == 0 ? exe_path : argv[i]);
#undef PUSH_STR

    uint32_t nwords = 1 + (argc + 1) + (envc + 1) + 2;
    sp = (sp - nwords * 4) & ~15u;
    gaddr_t p = sp;
    gwrite32(p, argc), p += 4;
    for (int i = 0; i < argc; i++) gwrite32(p, gargv[i]), p += 4;
    gwrite32(p, 0), p += 4;
    for (int i = 0; i < envc; i++) gwrite32(p, genv[i]), p += 4;
    gwrite32(p, 0), p += 4;
    gwrite32(p, gapple), p += 4;
    gwrite32(p, 0);

    runtime_set_process_args(argc, sp + 4, sp + 4 + 4 * (argc + 1), sp + 4 + 4 * (argc + 1) + 4 * (envc + 1));
    free(gargv);
    free(genv);
    return sp;
}

static void run_initializers(struct image *img) {
    for (int i = 0; i < img->nsects; i++) {
        const struct image_section *s = &img->sects[i];
        if ((s->flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS) continue;
        uint32_t n = s->size / 4;
        LOGI("running %u initializers of %s", n, img->path);
        for (uint32_t j = 0; j < n; j++) {
            gaddr_t fn = gread32(s->addr + 4 * j);
            uint32_t args[5] = {runtime_argc(), runtime_argv(), runtime_envp(), runtime_apple(),
                                runtime_program_vars()};
            char desc[256];
            LOGD("  initializer %u/%u at %#x %s", j + 1, n, fn, macho_describe_addr(fn, desc, sizeof desc));
            cpu_call(fn, 5, args);
        }
    }
}

// Test mode: link i386 objects/archives and call their main().
static int run_test_objects(int n, char **paths) {
    gmem_init(CPU_DIRECT_LOW, CPU_EXECUTES_GUEST_PAGES);
    cpu_init();
    hobj_init();
    thunks_init();
    runtime_preload_guest_libs(loaded, &nloaded);
    struct image *img = macho_load_objects("test", (const char *const *)paths, n, resolve_symbol);
    if (!img) fatal("no objects loaded");
    // The test image goes first so that it acts as the main executable.
    memmove(loaded + 1, loaded, sizeof loaded[0] * (size_t)nloaded);
    loaded[0] = img;
    nloaded++;
    macho_register(img);
    runtime_images_loaded(loaded, nloaded);
    objc_bridge_register_images();
    gaddr_t stack_lo = gmem_map_anywhere(MAIN_STACK_SIZE, GPROT_RW, 0);
    char *targv[] = {paths[0], NULL};
    gaddr_t sp = build_stack(stack_lo + MAIN_STACK_SIZE, 1, targv, paths[0]);
    runtime_set_main_stack(stack_lo, sp);
    cpu_set_thread_stack(stack_lo, sp);
    const char *describe = getenv("DXH_DESCRIBE"); // debug: "addr,addr,..."
    if (describe) {
        char *list = strdup(describe), *tok, *save = NULL;
        for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char buf[256];
            gaddr_t a = (gaddr_t)strtoul(tok, NULL, 0);
            LOGE("%#x = %s", a, macho_describe_addr(a, buf, sizeof buf));
        }
    }
    for (int i = nloaded - 1; i >= 0; i--) run_initializers(loaded[i]);
    gaddr_t main_fn = macho_export(img, "_main", NULL);
    if (!main_fn) fatal("test objects define no _main");
    uint32_t args[3] = {runtime_argc(), runtime_argv(), runtime_envp()};
    int rc = (int)cpu_call(main_fn, 3, args);
    runtime_exit(rc);
}

int main(int argc, char **argv) {
    log_init();
    if (!getenv("DXH_NO_CRASH_HANDLER")) crash_install();
    if (argc < 2) {
        fprintf(stderr, "usage: %s <Game.app | i386 executable> [args...]\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "--test") == 0) return run_test_objects(argc - 2, argv + 2);

    char *exe_path = find_executable(argv[1]);
    if (!exe_path) fatal("cannot resolve executable for %s", argv[1]);
    char exe_dir_buf[PATH_MAX];
    strlcpy(exe_dir_buf, exe_path, sizeof exe_dir_buf);
    const char *exe_dir = dirname(exe_dir_buf);

    // Feral's own launcher tells the game where its data lives (the folder
    // containing the .app, e.g. DeusExHRDCData next to it).
    if (!getenv("FERAL_RUNTIME_DATA_FOLDER")) {
        char app_dir[PATH_MAX];
        strlcpy(app_dir, exe_path, sizeof app_dir);
        char *contents = strstr(app_dir, ".app/Contents/MacOS/");
        if (contents) {
            *strstr(app_dir, ".app/") = 0;
            char *slash = strrchr(app_dir, '/');
            if (slash) *slash = 0;
            setenv("FERAL_RUNTIME_DATA_FOLDER", app_dir, 0);
            LOGI("FERAL_RUNTIME_DATA_FOLDER=%s", app_dir);
        }
    }

    gmem_init(CPU_DIRECT_LOW, CPU_EXECUTES_GUEST_PAGES);
    cpu_init();
    hobj_init();
    thunks_init();

    struct image *exe = macho_load(exe_path);
    if (!exe) return 1;
    if (exe->filetype != MH_EXECUTE) fatal("%s is not an executable", exe_path);
    loaded[nloaded++] = exe;
    macho_register(exe);
    load_dependencies(exe, exe_dir);
    runtime_preload_guest_libs(loaded, &nloaded);

    for (int i = 0; i < nloaded; i++) macho_link(loaded[i], resolve_symbol);
    runtime_images_loaded(loaded, nloaded);
    objc_bridge_register_images();
    objc_install_main_bundle();

    gaddr_t stack_lo = gmem_map_anywhere(MAIN_STACK_SIZE, GPROT_RW, 0);
    if (!stack_lo) fatal("no memory for the main stack");
    gaddr_t sp = build_stack(stack_lo + MAIN_STACK_SIZE, argc - 1, argv + 1, exe_path);
    // Initializers run below the argument block on the same stack.
    runtime_set_main_stack(stack_lo, sp);
    cpu_set_thread_stack(stack_lo, sp);

    // Initializers: dependencies first (reverse load order), main executable last.
    for (int i = nloaded - 1; i >= 0; i--) run_initializers(loaded[i]);

    // On macOS dyld runs initializers deep inside its own frames, so main()
    // later gets untouched, zero-filled stack memory. Some games (HR) rely on
    // that by accident (uninitialised members of large stack objects), so
    // hand main() a clean stack like the kernel would.
    memset(g2h(stack_lo), 0, sp - stack_lo);

    LOGI("starting %s at %#x", exe_path, exe->entry);
    cpu_start(exe->entry, sp);
}
