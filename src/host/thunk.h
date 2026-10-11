// Host implementations of symbols imported by guest code.
//
//   THUNK(strlen) { RET(strlen(ARGS(0))); }
//   THUNK_AS(fopen_unix2003, "_fopen$UNIX2003") { ... }
//   GDATA(__stderrp, 4, init_fn)   // guest-visible variable
//
// Definitions are collected through a linker section, so adding a thunk
// anywhere in the host sources is enough to register it.
#pragma once
#include "cpu.h"
#include "log.h"

#include <string.h>

typedef void (*thunk_fn)(struct guest_regs *r);

struct hostsym_def {
    const char *name;       // Mach-O symbol name, with leading underscore
    thunk_fn fn;            // function thunk, or NULL for data
    uint32_t data_size;     // data symbols: bytes to allocate in guest memory
    void (*data_init)(gaddr_t addr);
};

#define HOSTSYM_SECTION __attribute__((used, section("__DATA,__dxsyms")))

#define THUNK_AS(id, symname)                                                                  \
    static void thunk_impl_##id(struct guest_regs *r);                                       \
    HOSTSYM_SECTION static const struct hostsym_def hostsym_##id = {symname, thunk_impl_##id, 0, 0}; \
    static void thunk_impl_##id(__attribute__((unused)) struct guest_regs *r)

#define THUNK(sym) THUNK_AS(sym, "_" #sym)

#define GDATA_AS(id, symname, size, init)                                                      \
    HOSTSYM_SECTION static const struct hostsym_def hostsym_data_##id = {symname, NULL, size, init}

#define GDATA(sym, size, init) GDATA_AS(sym, "_" #sym, size, init)

// Argument access (cdecl: arguments start above the return address).
#define ARG(i) gread32(r->esp + 4 + 4 * (i))
#define ARGI(i) ((int32_t)ARG(i))
#define ARG64(i) ((uint64_t)ARG(i) | ((uint64_t)ARG((i) + 1) << 32))
#define ARGP(i) g2h(ARG(i))
#define ARGS(i) ((const char *)g2h(ARG(i)))
static inline double arg_double(struct guest_regs *r, int i) {
    uint64_t v = ARG64(i);
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline float arg_float(struct guest_regs *r, int i) {
    uint32_t v = ARG(i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
#define ARGD(i) arg_double(r, i)
#define ARGF(i) arg_float(r, i)

// RET* set the guest return value and return from the thunk.
#define RET(v) do { r->ret = (uint32_t)(v); return; } while (0)
#define RET64(v) do { r->ret = (uint64_t)(v); return; } while (0)
#define RETD(v) do { r->fret = (double)(v); r->fp_ret = 1; return; } while (0)

void thunks_init(void);
// Look up a host-provided symbol; returns 0 if none.
gaddr_t hostsym_lookup(const char *name);
// A stub for a symbol nobody provides: aborts with the name when called.
gaddr_t hostsym_missing_stub(const char *name);
const char *thunk_name(uint32_t id);

// Register a host function under a synthetic name at runtime (callbacks etc.).
gaddr_t thunk_register_dynamic(const char *name, thunk_fn fn);
