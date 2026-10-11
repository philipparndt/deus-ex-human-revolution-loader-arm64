// Fixes for bugs in the game's own code, applied to the loaded image before it
// runs. Each patch checks the original bytes at its site and is skipped when
// they differ (other game build), so it can never corrupt foreign code.
// Patched code jumps into a "cave": a guest page holding the replacement.
#include "patches.h"

#include "gmem.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

static gaddr_t cave_base, cave_next;

static gaddr_t cave_alloc(uint32_t n) {
    if (!cave_base) {
        cave_base = cave_next = gmem_map_anywhere(4096, GPROT_RWX, 0);
        if (!cave_base) return 0;
    }
    if (cave_next + n > cave_base + 4096) return 0;
    gaddr_t a = cave_next;
    cave_next += (n + 15) & ~15u;
    return a;
}

static void put_jmp(uint8_t *p, gaddr_t at, gaddr_t to) { // e9 rel32
    int32_t rel = (int32_t)(to - (at + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
}

static void put_jcc(uint8_t *p, uint8_t cc, gaddr_t at, gaddr_t to) { // 0f 8x rel32
    int32_t rel = (int32_t)(to - (at + 6));
    p[0] = 0x0F;
    p[1] = cc;
    memcpy(p + 2, &rel, 4);
}

// Write into the image's code (mapped read-only/executable).
static void code_write(gaddr_t at, const void *src, uint32_t n) {
    gaddr_t page = at & ~4095u, end = (at + n + 4095) & ~4095u;
    gmem_protect(page, end - page, GPROT_RWX);
    memcpy(g2h(at), src, n);
    gmem_protect(page, end - page, GPROT_READ | GPROT_EXEC);
}

static bool site_matches(gaddr_t at, const uint8_t *orig, uint32_t n) {
    return memcmp(g2h(at), orig, n) == 0;
}

// Deus Ex: Human Revolution - Director's Cut (Feral, 2014): the movie player's
// "next frame due" test (0xb4cc00) ignores the playback state, and its catch-up
// loop (0x6802b8: while (due) skip_frame()) only advances while playing. If the
// game stalls for longer than the rest of a movie (backgrounded app, loading
// spike), skip_frame reaches the end, sets the state to stopped and leaves the
// last frame's time, so the loop spins forever with the audio repeating. The
// same happens when a movie is paused with a frame due. Fix: not due unless
// playing (state 1 at movie+0x44).
static void patch_hr_movie_due(int32_t slide) {
    static const uint8_t orig[] = {0x66, 0x0F, 0x57, 0xC0,        // xorpd   xmm0, xmm0
                                   0x66, 0x0F, 0x2E, 0x46, 0x10}; // ucomisd xmm0, [esi+0x10]
    gaddr_t site = 0xb4cc0d + (uint32_t)slide;
    gaddr_t ret_not_due = 0xb4cce1 + (uint32_t)slide;
    gaddr_t resume = site + sizeof orig; // the original "je not_due" on the start time
    if (!site_matches(site, orig, sizeof orig)) return;
    // Sanity check of the surroundings: je rel32 to ret_not_due right after the site.
    const uint8_t *je = g2h(resume);
    int32_t rel;
    memcpy(&rel, je + 2, 4);
    if (je[0] != 0x0F || je[1] != 0x84 || resume + 6 + (uint32_t)rel != ret_not_due) return;

    uint8_t cave[4 + 6 + sizeof orig + 5];
    gaddr_t c = cave_alloc(sizeof cave);
    if (!c) return;
    static const uint8_t cmp_state[] = {0x83, 0x7E, 0x44, 0x01}; // cmp dword [esi+0x44], 1
    memcpy(cave, cmp_state, 4);
    put_jcc(cave + 4, 0x85, c + 4, ret_not_due); // jne not_due
    memcpy(cave + 10, orig, sizeof orig);         // the replaced instructions
    put_jmp(cave + 10 + sizeof orig, c + 10 + sizeof orig, resume);
    memcpy(g2h(c), cave, sizeof cave);

    uint8_t jmp[sizeof orig];
    memset(jmp, 0x90, sizeof jmp);
    put_jmp(jmp, site, c);
    code_write(site, jmp, sizeof jmp);
    LOGI("patch: HR movie frame-due test checks the playback state (%#x -> %#x)", site, c);
}

void patches_apply(struct image *exe) {
    if (getenv("DXH_NO_PATCHES")) return;
    patch_hr_movie_due(exe->slide);
}
