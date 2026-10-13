// Memory scanner for autoplay (DXH_AUTOPLAY): finds game state such as the
// player's position the way a person does with a cheat tool, but driven by the
// bot, which knows what it just did. "scan start" takes every plausible float
// in writable guest memory as a candidate; "scan same" (after standing still)
// and "scan changed" (after walking) narrow the set; "scan back" keeps values
// that returned to where they were at "scan mark" (walked forward, then back).
//
// Guest pages can be unmapped at any time, so memory is read with
// mach_vm_read_overwrite, which fails instead of faulting.
#include "gmem.h"
#include "log.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (1u << 20)
#define LIST_MAX (4u << 20) // switch from bitmap to list below this many candidates

struct region {
    gaddr_t g;
    uint32_t size;
    size_t off; // into snap and bitmap (in floats)
};
struct cand {
    gaddr_t g;
    float prev, mark, far; // last value, value at "scan mark", largest distance from mark since
};

static struct region *regions;
static int nregions;
static float *snap;     // bitmap phase: last value of every float
static uint64_t *bits;  // bitmap phase: candidate bits
static size_t nfloats;
static struct cand *list; // list phase
static size_t ncand;
static bool list_phase, active;
static float *chunk;

static bool plausible(float v) { return isfinite(v) && fabsf(v) > 1e-3f && fabsf(v) < 1e7f; }

static bool read_guest(gaddr_t g, void *dst, uint32_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(guest_base + g), size,
                                  (mach_vm_address_t)dst, &got) == KERN_SUCCESS &&
           got == size;
}

static void free_all(void) {
    free(regions), free(snap), free(bits), free(list);
    regions = NULL, snap = NULL, bits = NULL, list = NULL;
    nregions = 0, nfloats = ncand = 0, list_phase = active = false;
}

static void find_regions(void) {
    mach_vm_address_t a = (mach_vm_address_t)guest_base, end = a + 0x100000000ULL;
    int cap = 0;
    while (a < end) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n,
                           &obj) != KERN_SUCCESS || a >= end)
            break;
        if (a + size > end) size = end - a;
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) &&
            !(info.protection & VM_PROT_EXECUTE)) {
            for (mach_vm_size_t o = 0; o < size; o += 0x10000000u) { // keep sizes in uint32_t
                if (nregions == cap) regions = realloc(regions, sizeof *regions * (cap = cap ? cap * 2 : 1024));
                uint64_t s = size - o < 0x10000000u ? size - o : 0x10000000u;
                regions[nregions++] = (struct region){(gaddr_t)(a + o - (uintptr_t)guest_base), (uint32_t)s, nfloats};
                nfloats += s / 4;
            }
        }
        a += size;
    }
}

static void to_list(void) {
    list = malloc(sizeof *list * (ncand ? ncand : 1));
    size_t n = 0;
    for (int r = 0; r < nregions; r++)
        for (size_t i = 0; i < regions[r].size / 4; i++) {
            size_t k = regions[r].off + i;
            if (bits[k / 64] >> (k % 64) & 1)
                list[n++] = (struct cand){regions[r].g + (gaddr_t)i * 4, snap[k], snap[k], 0};
        }
    ncand = n;
    free(snap), free(bits);
    snap = NULL, bits = NULL;
    list_phase = true;
}

void memscan_start(void) {
    free_all();
    if (!chunk) chunk = malloc(CHUNK);
    find_regions();
    snap = malloc(nfloats * 4);
    bits = calloc((nfloats + 63) / 64, 8);
    for (int r = 0; r < nregions; r++) {
        float *s = snap + regions[r].off;
        if (!read_guest(regions[r].g, s, regions[r].size)) continue;
        for (size_t i = 0; i < regions[r].size / 4; i++)
            if (plausible(s[i])) bits[(regions[r].off + i) / 64] |= 1ull << ((regions[r].off + i) % 64), ncand++;
    }
    active = true;
    LOGI("memscan: %d regions, %zu MB, %zu candidates", nregions, nfloats * 4 >> 20, ncand);
    if (ncand <= LIST_MAX) to_list();
}

enum { KEEP_SAME, KEEP_CHANGED, KEEP_BACK };

static bool keep(int mode, struct cand *c, float v) {
    if (!plausible(v)) return false;
    float d = fabsf(v - c->prev), m = fabsf(v - c->mark);
    switch (mode) {
    case KEEP_SAME: return d <= 1e-3f * fmaxf(1, fabsf(v));
    case KEEP_CHANGED: return d >= 0.25f;
    default: // an axis the last move did not change still counts as back
        return c->far < 0.25f ? d <= 1e-3f * fmaxf(1, fabsf(v)) : m <= 0.3f * c->far;
    }
}

static void filter(int mode) {
    if (!active) { LOGE("memscan: no scan (scan start first)"); return; }
    if (!list_phase) {
        if (mode == KEEP_BACK) { LOGE("memscan: scan back needs a smaller candidate set"); return; }
        size_t n = 0;
        for (int r = 0; r < nregions; r++) {
            for (uint32_t o = 0; o < regions[r].size; o += CHUNK) {
                uint32_t len = regions[r].size - o < CHUNK ? regions[r].size - o : CHUNK;
                size_t base = regions[r].off + o / 4;
                bool ok = read_guest(regions[r].g + o, chunk, len);
                for (size_t i = 0; i < len / 4; i++) {
                    size_t k = base + i;
                    uint64_t *w = &bits[k / 64], bit = 1ull << (k % 64);
                    if (!(*w & bit)) {
                        if (!(*w >> (k % 64))) i += 63 - k % 64; // rest of this word is empty
                        continue;
                    }
                    struct cand c = {0, snap[k], snap[k], 0};
                    if (ok && keep(mode, &c, chunk[i])) snap[k] = chunk[i], n++;
                    else *w &= ~bit;
                }
            }
        }
        ncand = n;
        if (ncand <= LIST_MAX) to_list();
    } else {
        size_t n = 0;
        for (size_t i = 0; i < ncand; i++) {
            float v;
            if (!read_guest(list[i].g, &v, 4)) continue;
            float m = fabsf(v - list[i].mark);
            if (m > list[i].far) list[i].far = m;
            if (keep(mode, &list[i], v)) list[i].prev = v, list[n++] = list[i];
        }
        ncand = n;
    }
    LOGI("memscan: %s -> %zu candidates", mode == KEEP_SAME ? "same" : mode == KEEP_CHANGED ? "changed" : "back", ncand);
}

// Takes the current values as the reference for the next "same" (after a move
// that some coordinates follow and others don't); "mark" also starts a new
// "back" excursion.
static void refresh(bool and_mark) {
    if (!list_phase) { LOGE("memscan: scan mark/update needs a smaller candidate set"); return; }
    for (size_t i = 0; i < ncand; i++) {
        float v;
        if (read_guest(list[i].g, &v, 4)) list[i].prev = v;
        float m = fabsf(list[i].prev - list[i].mark);
        if (m > list[i].far) list[i].far = m;
        if (and_mark) list[i].mark = list[i].prev, list[i].far = 0;
    }
    LOGI("memscan: %s (%zu candidates)", and_mark ? "mark" : "update", ncand);
}

// Logs up to max candidates, grouped into runs of neighbouring floats (a
// position is usually x, y, z next to each other), and writes all to path.
static void report(int max, const char *path) {
    if (!list_phase) { LOGE("memscan: %zu candidates, too many to report", ncand); return; }
    FILE *f = path ? fopen(path, "w") : NULL;
    int shown = 0;
    for (size_t i = 0; i < ncand; i++) {
        float v = 0;
        read_guest(list[i].g, &v, 4);
        bool run = (i && list[i - 1].g + 4 == list[i].g) || (i + 1 < ncand && list[i + 1].g == list[i].g + 4);
        float nb[5] = {0}; // neighbours: a coordinate's siblings (y, z) are usually next to it
        read_guest(list[i].g - 8, nb, sizeof nb);
        if (f) fprintf(f, "%#010x %s now %.3f mark %.3f moved %.3f  [%.2f %.2f (%.2f) %.2f %.2f]\n", list[i].g, run ? "run" : "   ", v,
                       list[i].mark, list[i].far, nb[0], nb[1], nb[2], nb[3], nb[4]);
        if (shown < max) {
            LOGI("memscan: %#010x %s now %.3f mark %.3f moved %.3f  [%.2f %.2f (%.2f) %.2f %.2f]", list[i].g, run ? "run" : "   ", v,
                 list[i].mark, list[i].far, nb[0], nb[1], nb[2], nb[3], nb[4]);
            shown++;
        }
    }
    if (f) fclose(f);
    LOGI("memscan: %zu candidates%s%s", ncand, path ? ", all in " : "", path ? path : "");
}

// One script line after "scan": start | same | changed | back | mark | update | report N [FILE]
void memscan_command(const char *what, const char *arg, const char *file) {
    if (!strcmp(what, "start")) memscan_start();
    else if (!strcmp(what, "same")) filter(KEEP_SAME);
    else if (!strcmp(what, "changed")) filter(KEEP_CHANGED);
    else if (!strcmp(what, "back")) filter(KEEP_BACK);
    else if (!strcmp(what, "mark")) refresh(true);
    else if (!strcmp(what, "update")) refresh(false);
    else if (!strcmp(what, "report")) report(*arg ? atoi(arg) : 50, *file ? file : NULL);
    else LOGE("memscan: unknown scan command '%s'", what);
}
