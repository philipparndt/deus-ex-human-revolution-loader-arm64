#include "hobj.h"

#include "log.h"

#include <pthread.h>
#include <stdlib.h>

#define CHUNK_SLOTS 65536u // 1 MB of slots per mapping step

static void **slot_ptrs;
static uint32_t nslots, mapped_slots;
static pthread_mutex_t hobj_lock = PTHREAD_MUTEX_INITIALIZER;

// host pointer -> slot index + 1 (open addressing)
static struct { uintptr_t key; uint32_t idx1; } *ptr_map;
static size_t map_cap, map_count;

static size_t hash_ptr(uintptr_t p) { return (size_t)((p >> 4) * 0x9E3779B97F4A7C15ULL); }

static void map_insert(uintptr_t key, uint32_t idx1) {
    if ((map_count + 1) * 2 > map_cap) {
        size_t old_cap = map_cap;
        __typeof__(ptr_map) old = ptr_map;
        map_cap = map_cap ? map_cap * 2 : 65536;
        ptr_map = calloc(map_cap, sizeof *ptr_map);
        map_count = 0;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].key && old[i].idx1) map_insert(old[i].key, old[i].idx1);
        free(old);
    }
    size_t j = hash_ptr(key) & (map_cap - 1);
    while (ptr_map[j].key && ptr_map[j].key != key) j = (j + 1) & (map_cap - 1);
    if (!ptr_map[j].key) map_count++;
    ptr_map[j].key = key;
    ptr_map[j].idx1 = idx1;
}

static uint32_t map_find(uintptr_t key) {
    if (!map_cap) return 0;
    size_t j = hash_ptr(key) & (map_cap - 1);
    while (ptr_map[j].key) {
        if (ptr_map[j].key == key) return ptr_map[j].idx1;
        j = (j + 1) & (map_cap - 1);
    }
    return 0;
}

void hobj_init(void) {
    slot_ptrs = calloc(HOBJ_REGION_SIZE / HOBJ_SLOT_SIZE, sizeof(void *));
    nslots = 1; // slot 0 unused so that handles are never the region base
}

gaddr_t hobj_wrap(const void *host) {
    if (!host) return 0;
    pthread_mutex_lock(&hobj_lock);
    uint32_t idx1 = map_find((uintptr_t)host);
    uint32_t idx;
    if (idx1) {
        idx = idx1 - 1;
    } else {
        idx = nslots++;
        if (idx >= HOBJ_REGION_SIZE / HOBJ_SLOT_SIZE) fatal("hobj: handle region exhausted");
        if (idx >= mapped_slots) {
            if (!gmem_map_fixed(HOBJ_REGION_BASE + mapped_slots * HOBJ_SLOT_SIZE, CHUNK_SLOTS * HOBJ_SLOT_SIZE,
                                GPROT_RW))
                fatal("hobj: cannot map handle region");
            mapped_slots += CHUNK_SLOTS;
        }
        slot_ptrs[idx] = (void *)host;
        map_insert((uintptr_t)host, idx + 1);
        gaddr_t g = HOBJ_REGION_BASE + idx * HOBJ_SLOT_SIZE;
        gwrite32(g + 4, idx);
    }
    pthread_mutex_unlock(&hobj_lock);
    return HOBJ_REGION_BASE + idx * HOBJ_SLOT_SIZE;
}

void *hobj_unwrap(gaddr_t g) {
    if (!g || !hobj_is_handle(g)) return NULL;
    uint32_t idx = (g - HOBJ_REGION_BASE) / HOBJ_SLOT_SIZE;
    return idx < nslots ? slot_ptrs[idx] : NULL;
}

void hobj_set_isa(gaddr_t g, gaddr_t isa) {
    if (hobj_is_handle(g)) gwrite32(g & ~(HOBJ_SLOT_SIZE - 1), isa);
}

void hobj_forget(gaddr_t g) {
    if (!hobj_is_handle(g)) return;
    pthread_mutex_lock(&hobj_lock);
    uint32_t idx = (g - HOBJ_REGION_BASE) / HOBJ_SLOT_SIZE;
    if (idx < nslots && slot_ptrs[idx]) {
        map_insert((uintptr_t)slot_ptrs[idx], 0);
        slot_ptrs[idx] = NULL;
    }
    pthread_mutex_unlock(&hobj_lock);
}
