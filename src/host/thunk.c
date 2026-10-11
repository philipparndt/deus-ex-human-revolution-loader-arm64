#include "thunk.h"
#include "mainq.h"

#include "strmap.h"

#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_THUNKS 65536

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
    // .objc_class_name_* are link-time markers of the fragile ObjC ABI; their
    // values are never used at run time.
    if (strncmp(name, ".objc_class_name_", 17) == 0) {
        static gaddr_t marker;
        if (!marker) marker = gheap_calloc(1, 16);
        return marker;
    }
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

// DXH_THUNK_STATS=<seconds>: periodically log the most frequent host calls.
static _Atomic uint64_t thunk_calls[MAX_THUNKS];
static int thunk_stats = -1;

static int cmp_calls(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a >> 16, y = *(const uint64_t *)b >> 16;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void *thunk_stats_thread(void *arg) {
    static uint64_t last[MAX_THUNKS];
    for (;;) {
        sleep(thunk_stats);
        uint32_t n = nthunks;
        uint64_t *v = malloc(sizeof *v * n), total = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t c = thunk_calls[i], d = c - last[i];
            last[i] = c;
            total += d;
            v[i] = (d << 16) | i;
        }
        qsort(v, n, sizeof *v, cmp_calls);
        LOGI("thunk stats: %.0f calls/s", (double)total / thunk_stats);
        for (uint32_t i = 0; i < n && i < 25 && (v[i] >> 16); i++)
            LOGI("  %10.0f/s  %s", (double)(v[i] >> 16) / thunk_stats, thunks[v[i] & 0xffff].name);
        free(v);
    }
    return arg;
}

static void thunk_stats_init(void) {
    const char *e = getenv("DXH_THUNK_STATS");
    int secs = e ? atoi(e) : 0;
    if (secs > 0) {
        pthread_t t;
        thunk_stats = secs;
        pthread_create(&t, NULL, thunk_stats_thread, NULL);
    } else {
        thunk_stats = 0;
    }
}

void host_dispatch(struct guest_regs *r) {
    uint32_t id = r->thunk_id;
    if (thunk_stats) {
        if (thunk_stats < 0) {
            static pthread_once_t once = PTHREAD_ONCE_INIT;
            pthread_once(&once, thunk_stats_init);
        }
        if (id < MAX_THUNKS) atomic_fetch_add_explicit(&thunk_calls[id], 1, memory_order_relaxed);
    }
    if (atomic_load_explicit(&mainq_pending, memory_order_relaxed) && pthread_main_np()) mainq_drain();
    if (id >= nthunks) fatal("bad thunk id %u", id);
    LOGT("-> %s(%#x, %#x, %#x, %#x) ret=%#x", thunks[id].name, gread32(r->esp + 4), gread32(r->esp + 8),
         gread32(r->esp + 12), gread32(r->esp + 16), gread32(r->esp));
    thunks[id].fn(r);
}
