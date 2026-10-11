// Host object handles.
//
// Host objects (CF/ObjC objects, dispatch queues, ...) live in 64-bit host
// memory and cannot be passed to guest code directly. Each one gets a 16-byte
// slot in a dedicated guest region; the slot address is the guest-visible
// handle. The same host pointer always maps to the same handle.
//
// Slot layout: [0] isa (guest address, set by the ObjC bridge so that guest
// code reading obj->isa sees something sane), [4] slot index, [8..15] spare.
#pragma once
#include "gmem.h"

#define HOBJ_REGION_BASE 0xE0000000u
#define HOBJ_REGION_SIZE 0x10000000u
#define HOBJ_SLOT_SIZE 16u

static inline bool hobj_is_handle(gaddr_t g) {
    return g - HOBJ_REGION_BASE < HOBJ_REGION_SIZE;
}

void hobj_init(void);
// Wrap a host pointer (NULL -> 0).
gaddr_t hobj_wrap(const void *host);
// Unwrap a handle (0 -> NULL). Returns NULL for non-handles.
void *hobj_unwrap(gaddr_t g);
// Set the guest-visible isa word of a handle slot.
void hobj_set_isa(gaddr_t g, gaddr_t isa);
// Forget a handle (host object destroyed). The slot is not reused.
void hobj_forget(gaddr_t g);
