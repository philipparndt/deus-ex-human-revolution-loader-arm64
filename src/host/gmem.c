#include "gmem.h"

#include "log.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

uint8_t *guest_base;

#define NPAGES (0x100000000ULL >> GPAGE_SHIFT)

enum { PS_UNAVAIL = 0, PS_FREE, PS_MAPPED, PS_SLAB, PS_LARGE };

static uint8_t page_state[NPAGES];
static uint32_t large_pages[NPAGES]; // page count, stored at the first page of a large block
static uint8_t slab_class[NPAGES];   // size class of a slab page
static bool exec_allowed;
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t search_cursor = 0x10000000u >> GPAGE_SHIFT;

static int host_prot(int prot) {
    int p = 0;
    if (prot & GPROT_READ) p |= PROT_READ;
    if (prot & GPROT_WRITE) p |= PROT_WRITE;
    if ((prot & GPROT_EXEC) && exec_allowed) p |= PROT_EXEC;
    return p;
}

static void reserve_range(uint64_t start, uint64_t end) {
    // Round inwards to whole pages.
    start = (start + GPAGE_SIZE - 1) & ~(uint64_t)(GPAGE_SIZE - 1);
    end &= ~(uint64_t)(GPAGE_SIZE - 1);
    if (end <= start) return;
    void *p = mmap(guest_base + start, end - start, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        LOGD("gmem: could not reserve %#llx-%#llx", start, end);
        return;
    }
    for (uint64_t a = start; a < end; a += GPAGE_SIZE) page_state[a >> GPAGE_SHIFT] = PS_FREE;
}

void gmem_init(bool direct_low, bool guest_exec) {
    exec_allowed = guest_exec;
    memset(page_state, PS_UNAVAIL, sizeof page_state);
    if (direct_low) {
        guest_base = NULL;
        // The host binary carries a zero-fill __GUEST segment that the kernel
        // maps at exec time, before dyld/malloc start allocating low memory.
        const struct segment_command_64 *seg = getsegbyname("__GUEST");
        if (seg) reserve_range(seg->vmaddr, seg->vmaddr + seg->vmsize);
        else LOGE("gmem: host binary has no __GUEST reserve segment");
        // Walk the existing host mappings below 4 GB and reserve every other gap.
        mach_vm_address_t addr = 0, prev_end = GPAGE_SIZE;
        for (;;) {
            mach_vm_size_t size;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj;
            if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS)
                break;
            if (addr >= 0x100000000ULL) break;
            LOGD("gmem: host mapping %#llx-%#llx", addr, addr + size);
            if (addr > prev_end) reserve_range(prev_end, addr);
            prev_end = addr + size;
            addr += size;
        }
        if (prev_end < 0x100000000ULL - GPAGE_SIZE) reserve_range(prev_end, 0x100000000ULL - GPAGE_SIZE);
    } else {
        // Reserve 8 GB and keep the 4 GB-aligned window inside it.
        size_t span = 0x200000000ULL;
        uint8_t *p = mmap(NULL, span, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) fatal("gmem: cannot reserve guest address space");
        uint8_t *base = (uint8_t *)(((uintptr_t)p + 0xFFFFFFFFULL) & ~0xFFFFFFFFULL);
        if (base > p) munmap(p, base - p);
        uint8_t *end = base + 0x100000000ULL;
        if (end < p + span) munmap(end, (p + span) - end);
        guest_base = base;
        for (uint64_t i = 1; i < NPAGES - 1; i++) page_state[i] = PS_FREE;
    }
    LOGI("gmem: guest base %p", (void *)guest_base);
}

static bool range_has_state(uint32_t first, uint32_t n, int state) {
    for (uint32_t i = 0; i < n; i++)
        if (page_state[first + i] != state) return false;
    return true;
}

static bool do_map(uint32_t first, uint32_t n, int prot, int state) {
    void *want = guest_base + ((uint64_t)first << GPAGE_SHIFT);
    void *p = mmap(want, (size_t)n << GPAGE_SHIFT, host_prot(prot), MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (p != want) {
        LOGE("gmem: mmap at %p failed", want);
        return false;
    }
    memset(page_state + first, state, n);
    return true;
}

bool gmem_map_fixed(gaddr_t addr, uint32_t size, int prot) {
    uint64_t start = addr & ~(GPAGE_SIZE - 1);
    uint64_t end = ((uint64_t)addr + size + GPAGE_SIZE - 1) & ~(uint64_t)(GPAGE_SIZE - 1);
    uint32_t first = (uint32_t)(start >> GPAGE_SHIFT), n = (uint32_t)((end - start) >> GPAGE_SHIFT);
    pthread_mutex_lock(&mem_lock);
    bool ok = range_has_state(first, n, PS_FREE) && do_map(first, n, prot, PS_MAPPED);
    pthread_mutex_unlock(&mem_lock);
    return ok;
}

static uint32_t find_free(uint32_t n, uint32_t align_pages) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lo = pass == 0 ? search_cursor : 1, hi = pass == 0 ? NPAGES : search_cursor;
        uint32_t i = (lo + align_pages - 1) & ~(align_pages - 1);
        while (i + n <= hi) {
            uint32_t j = 0;
            while (j < n && page_state[i + j] == PS_FREE) j++;
            if (j == n) {
                search_cursor = i + n;
                return i;
            }
            i = (i + j + 1 + align_pages - 1) & ~(align_pages - 1);
        }
    }
    return 0;
}

static gaddr_t map_anywhere_state(uint32_t size, int prot, uint32_t align, int state) {
    uint32_t n = (uint32_t)(((uint64_t)size + GPAGE_SIZE - 1) >> GPAGE_SHIFT);
    uint32_t align_pages = align > GPAGE_SIZE ? align >> GPAGE_SHIFT : 1;
    if (n == 0) n = 1;
    pthread_mutex_lock(&mem_lock);
    uint32_t first = find_free(n, align_pages);
    gaddr_t r = 0;
    if (first && do_map(first, n, prot, state)) {
        r = first << GPAGE_SHIFT;
        if (state == PS_LARGE) large_pages[first] = n;
    }
    pthread_mutex_unlock(&mem_lock);
    return r;
}

gaddr_t gmem_map_anywhere(uint32_t size, int prot, uint32_t align) {
    return map_anywhere_state(size, prot, align, PS_MAPPED);
}

static void unmap_pages(uint32_t first, uint32_t n) {
    mmap(guest_base + ((uint64_t)first << GPAGE_SHIFT), (size_t)n << GPAGE_SHIFT, PROT_NONE,
         MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    memset(page_state + first, PS_FREE, n);
}

void gmem_unmap(gaddr_t addr, uint32_t size) {
    uint64_t start = addr & ~(GPAGE_SIZE - 1);
    uint64_t end = ((uint64_t)addr + size + GPAGE_SIZE - 1) & ~(uint64_t)(GPAGE_SIZE - 1);
    pthread_mutex_lock(&mem_lock);
    for (uint64_t a = start; a < end; a += GPAGE_SIZE)
        if (page_state[a >> GPAGE_SHIFT] == PS_MAPPED) unmap_pages((uint32_t)(a >> GPAGE_SHIFT), 1);
    pthread_mutex_unlock(&mem_lock);
}

bool gmem_protect(gaddr_t addr, uint32_t size, int prot) {
    uint64_t start = addr & ~(GPAGE_SIZE - 1);
    uint64_t end = ((uint64_t)addr + size + GPAGE_SIZE - 1) & ~(uint64_t)(GPAGE_SIZE - 1);
    return mprotect(guest_base + start, end - start, host_prot(prot)) == 0;
}

bool gmem_is_mapped(gaddr_t addr, uint32_t size) {
    uint64_t end = (uint64_t)addr + size;
    for (uint64_t a = addr & ~(GPAGE_SIZE - 1); a < end; a += GPAGE_SIZE) {
        int s = page_state[a >> GPAGE_SHIFT];
        if (s == PS_FREE || s == PS_UNAVAIL) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Guest heap: size-class slabs for small blocks, page runs for large blocks.

#define SLAB_SIZE 0x10000u
#define MAX_SMALL 8192u

static const uint32_t class_sizes[] = {
    16,   32,   48,   64,   80,   96,   112,  128,  144,  160,  176,  192,  208,  224,  240,  256,
    320,  384,  448,  512,  640,  768,  896,  1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192,
};
#define NCLASSES (sizeof class_sizes / sizeof class_sizes[0])

static gaddr_t free_list[NCLASSES];
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static int size_class(uint32_t size) {
    if (size <= 256) return size == 0 ? 0 : (int)((size + 15) / 16) - 1;
    for (int c = 16; c < (int)NCLASSES; c++)
        if (size <= class_sizes[c]) return c;
    return -1;
}

static bool refill(int c) {
    gaddr_t slab = map_anywhere_state(SLAB_SIZE, GPROT_RW, SLAB_SIZE, PS_SLAB);
    if (!slab) return false;
    for (uint32_t i = 0; i < SLAB_SIZE / GPAGE_SIZE; i++) slab_class[(slab >> GPAGE_SHIFT) + i] = (uint8_t)c;
    uint32_t sz = class_sizes[c];
    gaddr_t head = free_list[c];
    for (uint32_t off = (SLAB_SIZE / sz) * sz; off >= sz; off -= sz) {
        gaddr_t blk = slab + off - sz;
        gwrite32(blk, head);
        head = blk;
    }
    free_list[c] = head;
    return true;
}

gaddr_t gheap_alloc(uint32_t size) {
    int c = size_class(size);
    if (c < 0) return map_anywhere_state(size, GPROT_RW, 0, PS_LARGE);
    pthread_mutex_lock(&heap_lock);
    if (!free_list[c] && !refill(c)) {
        pthread_mutex_unlock(&heap_lock);
        return 0;
    }
    gaddr_t p = free_list[c];
    free_list[c] = gread32(p);
    pthread_mutex_unlock(&heap_lock);
    // Recycled blocks are zeroed: freshly allocated memory on macOS is
    // almost always zero, and some game code (accidentally) depends on it.
    memset(g2h(p), 0, class_sizes[c]);
    return p;
}

uint32_t gheap_size(gaddr_t p) {
    if (!p) return 0;
    uint32_t page = p >> GPAGE_SHIFT;
    switch (page_state[page]) {
    case PS_SLAB: return class_sizes[slab_class[page]];
    case PS_LARGE: return large_pages[page] ? large_pages[page] << GPAGE_SHIFT : 0;
    default: return 0;
    }
}

void gheap_free(gaddr_t p) {
    if (!p) return;
    uint32_t page = p >> GPAGE_SHIFT;
    int state = page_state[page];
    if (state == PS_SLAB) {
        int c = slab_class[page];
        pthread_mutex_lock(&heap_lock);
        gwrite32(p, free_list[c]);
        free_list[c] = p;
        pthread_mutex_unlock(&heap_lock);
    } else if (state == PS_LARGE && large_pages[page] && (p & (GPAGE_SIZE - 1)) == 0) {
        pthread_mutex_lock(&mem_lock);
        uint32_t n = large_pages[page];
        large_pages[page] = 0;
        unmap_pages(page, n);
        pthread_mutex_unlock(&mem_lock);
    } else {
        LOGE("gheap_free: invalid pointer %#x", p);
    }
}

gaddr_t gheap_calloc(uint32_t n, uint32_t size) {
    uint64_t total = (uint64_t)n * size;
    if (total > 0xFFFFFFFFu) return 0;
    gaddr_t p = gheap_alloc((uint32_t)total);
    if (p && size_class((uint32_t)total) >= 0) memset(g2h(p), 0, total); // large blocks are fresh zero pages
    return p;
}

gaddr_t gheap_realloc(gaddr_t p, uint32_t size) {
    if (!p) return gheap_alloc(size);
    if (size == 0) {
        gheap_free(p);
        return gheap_alloc(0);
    }
    uint32_t old = gheap_size(p);
    if (size <= old && (old <= MAX_SMALL || size > old / 2)) return p;
    gaddr_t n = gheap_alloc(size);
    if (!n) return 0;
    memcpy(g2h(n), g2h(p), old < size ? old : size);
    gheap_free(p);
    return n;
}

gaddr_t gheap_memalign(uint32_t align, uint32_t size) {
    if (align <= 16) return gheap_alloc(size);
    if (align <= 256 && size_class(size) >= 0) {
        // Power-of-two class sizes <= 8192 that are multiples of align are naturally aligned.
        uint32_t want = size < align ? align : size;
        want = (want + align - 1) & ~(align - 1);
        while (want <= MAX_SMALL) {
            int c = size_class(want);
            if (class_sizes[c] % align == 0) return gheap_alloc(class_sizes[c]);
            want += align;
        }
    }
    return map_anywhere_state(size ? size : 1, GPROT_RW, align, PS_LARGE);
}

gaddr_t gheap_strdup(const char *s) {
    if (!s) return 0;
    return gheap_memdup(s, (uint32_t)strlen(s) + 1);
}

gaddr_t gheap_memdup(const void *src, uint32_t size) {
    gaddr_t p = gheap_alloc(size);
    if (p) memcpy(g2h(p), src, size);
    return p;
}
