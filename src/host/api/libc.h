// Private helpers shared by the libc/POSIX thunk files (math, file, time, net,
// sys, locale, wchar, misc).
#pragma once
#include "common.h"

// Define an additional symbol name for an existing thunk in the same file.
#define THUNK_ALIAS(id, symname, target) \
    THUNK_AS(id, symname) { thunk_impl_##target(r); }

// Intern a host string in guest memory: the same contents always yield the same
// guest address, and the string is never freed. For "returns a pointer to a
// static string" APIs whose results the guest may keep (setlocale, tm_zone, ...).
gaddr_t gintern(const char *s);

// 64-bit guest reads/writes (little endian, 4-byte alignment is fine).
static inline uint64_t gread64(gaddr_t a) { return gread32(a) | ((uint64_t)gread32(a + 4) << 32); }
static inline void gwrite64(gaddr_t a, uint64_t v) {
    gwrite32(a, (uint32_t)v);
    gwrite32(a + 4, (uint32_t)(v >> 32));
}

// i386 struct tm conversions that keep tm_zone valid (interned) and pass the
// guest's tm_zone back to the host (for strftime %Z).
struct tm;
void libc_put_tm(gaddr_t g, const struct tm *tm);
void libc_get_tm(gaddr_t g, struct tm *tm);

// file.c: memory mappings created through mmap (used by vm_deallocate).
// Returns true if [addr, addr+size) overlapped a tracked mapping and was unmapped.
bool libc_munmap_tracked(gaddr_t addr, uint32_t size);
