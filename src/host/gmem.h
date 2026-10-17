// Guest (32-bit) address space management.
//
// The guest sees a flat 4 GB address space. On the host it lives at
// guest_base .. guest_base + 4 GB. With the Rosetta backend guest_base is 0
// (guest addresses are host addresses); natively on arm64 the kernel does not
// allow mappings below 4 GB, so guest_base is a 4 GB-aligned reservation.
//
// Host code must never dereference a guest address directly: always go
// through g2h()/h2g().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t gaddr_t;

extern uint8_t *guest_base;

static inline void *g2h(gaddr_t a) { return a ? (void *)(guest_base + a) : NULL; }
static inline gaddr_t h2g(const void *p) {
    return p ? (gaddr_t)((uintptr_t)p - (uintptr_t)guest_base) : 0;
}
// True if a host pointer lies inside the guest address space.
static inline bool host_ptr_in_guest(const void *p) {
    return (uintptr_t)p - (uintptr_t)guest_base < 0x100000000ULL;
}

// Guest memory accessors (unaligned-safe, little endian).
static inline uint32_t gread32(gaddr_t a) { uint32_t v; __builtin_memcpy(&v, guest_base + a, 4); return v; }
static inline uint16_t gread16(gaddr_t a) { uint16_t v; __builtin_memcpy(&v, guest_base + a, 2); return v; }
static inline uint8_t gread8(gaddr_t a) { return guest_base[a]; }
static inline void gwrite32(gaddr_t a, uint32_t v) { __builtin_memcpy(guest_base + a, &v, 4); }
static inline void gwrite16(gaddr_t a, uint16_t v) { __builtin_memcpy(guest_base + a, &v, 2); }
static inline void gwrite8(gaddr_t a, uint8_t v) { guest_base[a] = v; }

enum { GPROT_NONE = 0, GPROT_READ = 1, GPROT_WRITE = 2, GPROT_EXEC = 4 };
#define GPROT_RW (GPROT_READ | GPROT_WRITE)
#define GPROT_RWX (GPROT_READ | GPROT_WRITE | GPROT_EXEC)

#define GPAGE_SIZE 0x4000u // host page size (16 KB on Apple Silicon, also under Rosetta)
#define GPAGE_SHIFT 14

// Configure where guest memory lives. direct_low = true: guest_base = 0 and the
// low 4 GB of the host process are used (Rosetta backend).
void gmem_init(bool direct_low, bool guest_exec);

// Map zero-filled memory at an exact guest address. Fails (returns false) if any
// page is not available.
bool gmem_map_fixed(gaddr_t addr, uint32_t size, int prot);
// Map zero-filled memory anywhere; returns 0 on failure. align must be a power of
// two >= GPAGE_SIZE (or 0 for page alignment).
gaddr_t gmem_map_anywhere(uint32_t size, int prot, uint32_t align);
// Keep a range out of map_anywhere and the heap; gmem_map_reserved maps parts of it.
void gmem_reserve(gaddr_t addr, uint32_t size);
bool gmem_map_reserved(gaddr_t addr, uint32_t size, int prot);
uint32_t gmem_used_pages(void); // guest address space in use, in GPAGE_SIZE pages
void gmem_unmap(gaddr_t addr, uint32_t size);
bool gmem_protect(gaddr_t addr, uint32_t size, int prot);
bool gmem_is_mapped(gaddr_t addr, uint32_t size);

// Guest heap (malloc family semantics, 16-byte aligned, thread safe).
gaddr_t gheap_alloc(uint32_t size);
gaddr_t gheap_calloc(uint32_t n, uint32_t size);
gaddr_t gheap_realloc(gaddr_t p, uint32_t size);
gaddr_t gheap_memalign(uint32_t align, uint32_t size);
void gheap_free(gaddr_t p);
uint32_t gheap_size(gaddr_t p); // usable size, 0 if not a heap pointer

// Convenience: copy host data/strings into the guest heap.
gaddr_t gheap_strdup(const char *s);
gaddr_t gheap_memdup(const void *p, uint32_t size);
