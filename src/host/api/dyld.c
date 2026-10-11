// dyld / dlfcn: image queries and dynamic loading of further guest images.
#include "common.h"
#include "../macho.h"
#include "../runtime.h"

#include <limits.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define RTLD_NEXT_G 0xFFFFFFFFu
#define RTLD_DEFAULT_G 0xFFFFFFFEu
#define RTLD_SELF_G 0xFFFFFFFDu
#define RTLD_MAIN_ONLY_G 0xFFFFFFFBu

static pthread_mutex_t dl_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static gaddr_t add_image_cbs[32], remove_image_cbs[32];
static int nadd_cbs, nremove_cbs;
static _Thread_local char dl_error[512];
static struct image *dl_images[64];
static int ndl_images;

// Pseudo handle for system libraries that are implemented by host thunks.
static gaddr_t host_lib_handle;

static gaddr_t image_name_guest(struct image *img) {
    static struct { struct image *img; gaddr_t name; } cache[64];
    for (int i = 0; i < 64; i++) {
        if (cache[i].img == img) return cache[i].name;
        if (!cache[i].img) {
            cache[i].img = img;
            cache[i].name = gheap_strdup(img->path);
            return cache[i].name;
        }
    }
    return gheap_strdup(img->path);
}

static struct image *image_for_addr(gaddr_t a) {
    for (int i = 0; i < runtime_image_count(); i++) {
        struct image *img = runtime_image(i);
        if (a >= img->lo && a < img->hi) return img;
    }
    for (int i = 0; i < ndl_images; i++)
        if (a >= dl_images[i]->lo && a < dl_images[i]->hi) return dl_images[i];
    if (a >= 0x1000 && a < 0x4000 && runtime_image_count()) return runtime_image(0); // exe header page
    return NULL;
}

THUNK(_dyld_register_func_for_add_image) {
    pthread_mutex_lock(&dl_lock);
    if (nadd_cbs < 32) add_image_cbs[nadd_cbs++] = ARG(0);
    pthread_mutex_unlock(&dl_lock);
    for (int i = 0; i < runtime_image_count(); i++) {
        struct image *img = runtime_image(i);
        gcall2(ARG(0), img->header, (uint32_t)img->slide);
    }
    for (int i = 0; i < ndl_images; i++) gcall2(ARG(0), dl_images[i]->header, (uint32_t)dl_images[i]->slide);
}
THUNK(_dyld_register_func_for_remove_image) {
    pthread_mutex_lock(&dl_lock);
    if (nremove_cbs < 32) remove_image_cbs[nremove_cbs++] = ARG(0);
    pthread_mutex_unlock(&dl_lock);
}

static struct image *image_by_index(uint32_t i) {
    int n = runtime_image_count();
    if ((int)i < n) return runtime_image((int)i);
    if ((int)i - n < ndl_images) return dl_images[i - n];
    return NULL;
}
THUNK(_dyld_image_count) { RET(runtime_image_count() + ndl_images); }
THUNK(_dyld_get_image_header) {
    struct image *img = image_by_index(ARG(0));
    RET(img ? img->header : 0);
}
THUNK(_dyld_get_image_vmaddr_slide) {
    struct image *img = image_by_index(ARG(0));
    RET(img ? (uint32_t)img->slide : 0);
}
THUNK(_dyld_get_image_name) {
    struct image *img = image_by_index(ARG(0));
    RET(img ? image_name_guest(img) : 0);
}

// struct dyld_unwind_sections { mh; dwarf_section; dwarf_section_length; compact_unwind_section; length; }
THUNK(_dyld_find_unwind_sections) {
    struct image *img = image_for_addr(ARG(0));
    if (!img) RET(0);
    gaddr_t out = ARG(1);
    const struct image_section *eh = macho_section(img, "__TEXT", "__eh_frame");
    const struct image_section *cu = macho_section(img, "__TEXT", "__unwind_info");
    gwrite32(out, img->header);
    gwrite32(out + 4, eh ? eh->addr : 0);
    gwrite32(out + 8, eh ? eh->size : 0);
    gwrite32(out + 12, cu ? cu->addr : 0);
    gwrite32(out + 16, cu ? cu->size : 0);
    RET(1);
}

// i386 Dl_info { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; }
THUNK(dladdr) {
    struct image *img = image_for_addr(ARG(0));
    gaddr_t info = ARG(1);
    if (!img) RET(0);
    gwrite32(info, image_name_guest(img));
    gwrite32(info + 4, img->header);
    gwrite32(info + 8, 0);
    gwrite32(info + 12, 0);
    // Nearest preceding symbol
    const char *best = NULL;
    uint32_t best_addr = 0;
    for (uint32_t i = 0; i < img->nsyms; i++) {
        const struct nlist *n = &img->syms[i];
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT) continue;
        uint32_t a = n->n_value + (img->filetype == MH_DYLIB && img->file ? img->slide : 0);
        if (a <= ARG(0) && a >= best_addr) {
            best_addr = a;
            best = img->strtab + n->n_un.n_strx;
        }
    }
    if (best) {
        gwrite32(info + 8, guest_static_string(3, best[0] == '_' ? best + 1 : best));
        gwrite32(info + 12, best_addr);
    }
    RET(1);
}

THUNK(_NSGetExecutablePath) {
    struct image *exe = runtime_image(0);
    const char *path = exe ? exe->path : "";
    uint32_t need = (uint32_t)strlen(path) + 1;
    if (gread32(ARG(1)) < need) {
        gwrite32(ARG(1), need);
        RET(-1);
    }
    memcpy(ARGP(0), path, need);
    RET(0);
}

// const NXArchInfo *NXGetLocalArchInfo(void) -> { name, cputype, cpusubtype, byteorder, description }
THUNK(NXGetLocalArchInfo) {
    static gaddr_t info;
    if (!info) {
        info = gheap_calloc(1, 20);
        gwrite32(info, gheap_strdup("i386"));
        gwrite32(info + 4, 7);
        gwrite32(info + 8, 3);
        gwrite32(info + 12, 1); // NX_LittleEndian
        gwrite32(info + 16, gheap_strdup("Intel 80x86"));
    }
    RET(info);
}

// --- dlopen ---------------------------------------------------------------------------

static gaddr_t resolve_global(const char *name, const char *from, bool weak) {
    (void)from;
    (void)weak;
    gaddr_t a;
    for (int i = 0; i < runtime_image_count(); i++)
        if ((a = macho_export(runtime_image(i), name, NULL))) return a;
    for (int i = 0; i < ndl_images; i++)
        if ((a = macho_export(dl_images[i], name, NULL))) return a;
    return hostsym_lookup(name);
}

static bool is_i386_macho(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[4096];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n < 8) return false;
    uint32_t magic;
    memcpy(&magic, hdr, 4);
    if (magic == MH_MAGIC) return true;
    if (OSSwapBigToHostInt32(magic) == 0xCAFEBABE) {
        uint32_t narch = OSSwapBigToHostInt32(*(uint32_t *)(hdr + 4));
        for (uint32_t i = 0; i < narch && 8 + 20 * (i + 1) <= n; i++)
            if (OSSwapBigToHostInt32(*(uint32_t *)(hdr + 8 + 20 * i)) == 7) return true;
    }
    return false;
}

static char *resolve_dl_path(const char *path) {
    char buf[PATH_MAX];
    struct image *exe = runtime_image(0);
    char exe_dir[PATH_MAX] = ".";
    if (exe) {
        strlcpy(exe_dir, exe->path, sizeof exe_dir);
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = 0;
    }
    if (strncmp(path, "@executable_path/", 17) == 0) snprintf(buf, sizeof buf, "%s/%s", exe_dir, path + 17);
    else if (strncmp(path, "@loader_path/", 13) == 0) snprintf(buf, sizeof buf, "%s/%s", exe_dir, path + 13);
    else if (path[0] != '/') {
        // Relative names: look next to the executable and in Resources/Frameworks.
        const char *dirs[] = {"", "/../Resources", "/../Frameworks"};
        for (int i = 0; i < 3; i++) {
            snprintf(buf, sizeof buf, "%s%s/%s", exe_dir, dirs[i], path);
            if (access(buf, R_OK) == 0) return realpath(buf, NULL);
        }
        return NULL;
    } else
        strlcpy(buf, path, sizeof buf);
    return realpath(buf, NULL);
}

static void run_image_initializers(struct image *img) {
    for (int i = 0; i < img->nsects; i++) {
        const struct image_section *s = &img->sects[i];
        if ((s->flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS) continue;
        for (uint32_t j = 0; j < s->size / 4; j++) {
            uint32_t args[5] = {runtime_argc(), runtime_argv(), runtime_envp(), runtime_apple(),
                                runtime_program_vars()};
            cpu_call(gread32(s->addr + 4 * j), 5, args);
        }
    }
}

THUNK(dlopen) {
    if (!ARG(0)) RET(RTLD_DEFAULT_G);
    const char *path = ARGS(0);
    pthread_mutex_lock(&dl_lock);
    for (int i = 0; i < ndl_images; i++) {
        if (strcmp(dl_images[i]->install_name ? dl_images[i]->install_name : "", path) == 0) {
            pthread_mutex_unlock(&dl_lock);
            RET(hobj_wrap(dl_images[i]));
        }
    }
    char *real = resolve_dl_path(path);
    if (real && is_i386_macho(real)) {
        for (int i = 0; i < ndl_images; i++)
            if (strcmp(dl_images[i]->path, real) == 0) {
                pthread_mutex_unlock(&dl_lock);
                free(real);
                RET(hobj_wrap(dl_images[i]));
            }
        struct image *img = macho_load(real);
        free(real);
        if (!img) {
            pthread_mutex_unlock(&dl_lock);
            snprintf(dl_error, sizeof dl_error, "dlopen(%s): cannot load image", path);
            RET(0);
        }
        if (!img->install_name) img->install_name = strdup(path);
        macho_register(img);
        dl_images[ndl_images++] = img;
        macho_link(img, resolve_global);
        pthread_mutex_unlock(&dl_lock);
        LOGI("dlopen: loaded guest image %s", path);
        run_image_initializers(img);
        for (int i = 0; i < nadd_cbs; i++) gcall2(add_image_cbs[i], img->header, (uint32_t)img->slide);
        RET(hobj_wrap(img));
    }
    pthread_mutex_unlock(&dl_lock);
    free(real);
    // System libraries/frameworks: provided by host thunks.
    if (strncmp(path, "/System/", 8) == 0 || strncmp(path, "/usr/lib/", 9) == 0) {
        LOGD("dlopen(%s): system library, served by host thunks", path);
        if (!host_lib_handle) host_lib_handle = gheap_calloc(1, 16);
        RET(host_lib_handle);
    }
    LOGI("dlopen(%s) failed: not found", path);
    snprintf(dl_error, sizeof dl_error, "dlopen(%s): image not found", path);
    RET(0);
}

THUNK(dlsym) {
    char name[512];
    snprintf(name, sizeof name, "_%s", ARGS(1));
    gaddr_t h = ARG(0), a = 0;
    if (h == host_lib_handle && h) {
        a = hostsym_lookup(name);
    } else if (h == RTLD_DEFAULT_G || h == RTLD_NEXT_G || h == RTLD_SELF_G || h == RTLD_MAIN_ONLY_G) {
        a = resolve_global(name, NULL, false);
    } else {
        struct image *img = hobj_unwrap(h);
        if (img) a = macho_export(img, name, NULL);
    }
    if (!a) {
        snprintf(dl_error, sizeof dl_error, "dlsym(%s): symbol not found", ARGS(1));
        LOGD("%s", dl_error);
    }
    RET(a);
}

THUNK(dlclose) { RET(0); }
THUNK(dlerror) {
    if (!dl_error[0]) RET(0);
    gaddr_t s = guest_static_string(4, dl_error);
    dl_error[0] = 0;
    RET(s);
}
