#include "thunk.h"

#include "strmap.h"

#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <stdlib.h>

#define MAX_THUNKS 16384

static struct {
    const char *name;
    thunk_fn fn;
    gaddr_t stub;
} thunks[MAX_THUNKS];
static uint32_t nthunks;
static struct strmap symbols; // name -> guest address
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

static void missing_called(struct guest_regs *r) {
    fatal("guest called unimplemented function %s (return address %#x)", thunks[r->thunk_id].name,
          gread32(r->esp));
}

static gaddr_t add_thunk(const char *name, thunk_fn fn) {
    pthread_mutex_lock(&thunk_lock);
    if (nthunks >= MAX_THUNKS) fatal("too many thunks");
    uint32_t id = nthunks++;
    thunks[id].name = name;
    thunks[id].fn = fn;
    thunks[id].stub = cpu_make_stub(id);
    gaddr_t stub = thunks[id].stub;
    pthread_mutex_unlock(&thunk_lock);
    return stub;
}

void thunks_init(void) {
    strmap_init(&symbols);
    unsigned long size = 0;
    const struct hostsym_def *defs =
        (const struct hostsym_def *)getsectiondata(&_mh_execute_header, "__DATA", "__dxsyms", &size);
    size_t n = defs ? size / sizeof *defs : 0;
    size_t nfn = 0, ndata = 0;
    for (size_t i = 0; i < n; i++) {
        gaddr_t addr;
        if (defs[i].fn) {
            addr = add_thunk(defs[i].name, defs[i].fn);
            nfn++;
        } else {
            addr = gheap_calloc(1, defs[i].data_size ? defs[i].data_size : 4);
            ndata++;
        }
        if (!strmap_put(&symbols, defs[i].name, addr, false)) LOGE("duplicate host symbol %s", defs[i].name);
    }
    // Data initializers run after all symbols exist (they may reference each other).
    for (size_t i = 0; i < n; i++) {
        if (defs[i].fn || !defs[i].data_init) continue;
        uint64_t addr;
        strmap_get(&symbols, defs[i].name, &addr);
        defs[i].data_init((gaddr_t)addr);
    }
    LOGI("thunks: %zu functions, %zu data symbols", nfn, ndata);
}

gaddr_t hostsym_lookup(const char *name) {
    uint64_t v;
    return strmap_get(&symbols, name, &v) ? (gaddr_t)v : 0;
}

gaddr_t hostsym_missing_stub(const char *name) {
    uint64_t v;
    if (strmap_get(&symbols, name, &v)) return (gaddr_t)v;
    gaddr_t stub = add_thunk(strdup(name), missing_called);
    strmap_put(&symbols, name, stub, false);
    return stub;
}

gaddr_t thunk_register_dynamic(const char *name, thunk_fn fn) { return add_thunk(name, fn); }

const char *thunk_name(uint32_t id) { return id < nthunks ? thunks[id].name : "?"; }

void host_dispatch(struct guest_regs *r) {
    uint32_t id = r->thunk_id;
    if (id >= nthunks) fatal("bad thunk id %u", id);
    LOGT("-> %s(%#x, %#x, %#x, %#x) ret=%#x", thunks[id].name, gread32(r->esp + 4), gread32(r->esp + 8),
         gread32(r->esp + 12), gread32(r->esp + 16), gread32(r->esp));
    thunks[id].fn(r);
}
