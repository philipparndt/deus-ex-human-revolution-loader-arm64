#include "macho.h"

#include "log.h"
#include "strmap.h"
#include "thunk.h"

#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_IMAGES 64
static struct image *images[MAX_IMAGES];
static int nimages;
static struct strmap export_maps[MAX_IMAGES];

static uint8_t *read_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    fstat(fd, &st);
    uint8_t *buf = malloc(st.st_size);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, st.st_size - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    *size = got;
    return buf;
}

static int vmprot_to_gprot(vm_prot_t p) {
    return ((p & VM_PROT_READ) ? GPROT_READ : 0) | ((p & VM_PROT_WRITE) ? GPROT_WRITE : 0) |
           ((p & VM_PROT_EXECUTE) ? GPROT_EXEC : 0);
}

struct image *macho_load(const char *path) {
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        LOGE("macho: cannot read %s", path);
        return NULL;
    }
    // Select the i386 slice of a universal binary.
    uint8_t *mh_data = data;
    size_t mh_size = size;
    if (OSSwapBigToHostInt32(*(uint32_t *)data) == FAT_MAGIC) {
        const struct fat_header *fh = (const struct fat_header *)data;
        const struct fat_arch *fa = (const struct fat_arch *)(fh + 1);
        mh_data = NULL;
        for (uint32_t i = 0; i < OSSwapBigToHostInt32(fh->nfat_arch); i++) {
            if ((cpu_type_t)OSSwapBigToHostInt32(fa[i].cputype) == CPU_TYPE_I386) {
                mh_data = data + OSSwapBigToHostInt32(fa[i].offset);
                mh_size = OSSwapBigToHostInt32(fa[i].size);
            }
        }
        if (!mh_data) {
            LOGE("macho: %s has no i386 slice", path);
            return NULL;
        }
    }
    const struct mach_header *mh = (const struct mach_header *)mh_data;
    if (mh->magic != MH_MAGIC || mh->cputype != CPU_TYPE_I386) {
        LOGE("macho: %s is not an i386 Mach-O", path);
        return NULL;
    }

    struct image *img = calloc(1, sizeof *img);
    img->path = strdup(path);
    img->file = mh_data;
    img->file_size = mh_size;
    img->filetype = mh->filetype;
    img->mh_flags = mh->flags;

    // First pass: collect load commands.
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    uint32_t nsects_total = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize)
        if (((const struct load_command *)lc)->cmd == LC_SEGMENT)
            nsects_total += ((const struct segment_command *)lc)->nsects;
    img->sects = calloc(nsects_total ? nsects_total : 1, sizeof *img->sects);
    img->dylibs = calloc(mh->ncmds, sizeof(char *));
    img->dylib_weak = calloc(mh->ncmds, sizeof(bool));

    uint32_t vm_lo = UINT32_MAX, vm_hi = 0;
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *cmd = (const struct load_command *)lc;
        switch (cmd->cmd) {
        case LC_SEGMENT: {
            const struct segment_command *sc = (const struct segment_command *)cmd;
            if (img->nfile_segs == 16) fatal("macho: too many segments");
            int fidx = img->nfile_segs++;
            img->seg_by_file_index[fidx] = -1;
            if (sc->vmsize == 0 || (sc->initprot == 0 && sc->filesize == 0 && sc->vmaddr == 0)) break; // __PAGEZERO
            if (img->nsegs == 16) fatal("macho: too many segments");
            img->seg_by_file_index[fidx] = img->nsegs;
            struct image_segment *s = &img->segs[img->nsegs++];
            memcpy(s->name, sc->segname, 16);
            s->addr = sc->vmaddr;
            s->vmsize = sc->vmsize;
            s->fileoff = sc->fileoff;
            s->filesize = sc->filesize;
            s->prot = vmprot_to_gprot(sc->initprot);
            if (sc->vmaddr < vm_lo) vm_lo = sc->vmaddr;
            if (sc->vmaddr + sc->vmsize > vm_hi) vm_hi = sc->vmaddr + sc->vmsize;
            const struct section *sec = (const struct section *)(sc + 1);
            for (uint32_t j = 0; j < sc->nsects; j++) {
                struct image_section *d = &img->sects[img->nsects++];
                memcpy(d->segname, sec[j].segname, 16);
                memcpy(d->sectname, sec[j].sectname, 16);
                d->addr = sec[j].addr;
                d->size = sec[j].size;
                d->offset = sec[j].offset;
                d->flags = sec[j].flags;
                d->reserved1 = sec[j].reserved1;
                d->reserved2 = sec[j].reserved2;
                d->reloff = sec[j].reloff;
                d->nreloc = sec[j].nreloc;
            }
            break;
        }
        case LC_SYMTAB: {
            const struct symtab_command *st = (const struct symtab_command *)cmd;
            img->syms = (const struct nlist *)(mh_data + st->symoff);
            img->nsyms = st->nsyms;
            img->strtab = (const char *)(mh_data + st->stroff);
            break;
        }
        case LC_DYSYMTAB: img->dysym = (const struct dysymtab_command *)cmd; break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: img->dyld_info = (const struct dyld_info_command *)cmd; break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LAZY_LOAD_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: {
            const struct dylib_command *dc = (const struct dylib_command *)cmd;
            img->dylib_weak[img->ndylibs] = cmd->cmd == LC_LOAD_WEAK_DYLIB;
            img->dylibs[img->ndylibs++] = strdup((const char *)dc + dc->dylib.name.offset);
            break;
        }
        case LC_ID_DYLIB: {
            const struct dylib_command *dc = (const struct dylib_command *)cmd;
            img->install_name = strdup((const char *)dc + dc->dylib.name.offset);
            break;
        }
        case LC_UNIXTHREAD: {
            // flavor, count, then i386_thread_state_t; eip is the 11th register.
            const uint32_t *w = (const uint32_t *)(cmd + 1);
            if (w[0] == 1 /* x86_THREAD_STATE32 */) img->entry = w[2 + 10];
            break;
        }
        case LC_MAIN: {
            const struct entry_point_command *ep = (const struct entry_point_command *)cmd;
            img->entry = (uint32_t)ep->entryoff; // file offset, fixed up below
            img->entry_is_main = true;
            break;
        }
        default: break;
        }
    }
    if (img->nsegs == 0) fatal("macho: %s has no segments", path);

    // Map the image.
    uint32_t map_lo = vm_lo & ~(GPAGE_SIZE - 1);
    uint32_t map_hi = (vm_hi + GPAGE_SIZE - 1) & ~(GPAGE_SIZE - 1);
    bool fixed = mh->filetype == MH_EXECUTE && !(mh->flags & MH_PIE);
    if (fixed) {
        // The first host page is never available; non-PIE executables start at
        // 0x1000 but only have the header and load commands there.
        if (map_lo < GPAGE_SIZE) map_lo = GPAGE_SIZE;
        if (!gmem_map_fixed(map_lo, map_hi - map_lo, GPROT_RWX))
            fatal("macho: cannot map %s at %#x-%#x", path, map_lo, map_hi);
        img->slide = 0;
    } else {
        gaddr_t base = gmem_map_anywhere(map_hi - map_lo, GPROT_RWX, 0);
        if (!base) fatal("macho: no room for %s", path);
        img->slide = (int32_t)(base - map_lo);
    }
    img->lo = map_lo + img->slide;
    img->hi = map_hi + img->slide;

    for (int i = 0; i < img->nsegs; i++) {
        struct image_segment *s = &img->segs[i];
        s->addr += img->slide;
        uint32_t dst = s->addr, off = s->fileoff, len = s->filesize;
        if (dst < img->lo) { // clipped first page (executable header)
            uint32_t skip = img->lo - dst;
            if (skip >= len) continue;
            dst += skip;
            off += skip;
            len -= skip;
        }
        memcpy(g2h(dst), mh_data + off, len);
    }
    for (int i = 0; i < img->nsects; i++) img->sects[i].addr += img->slide;

    // Header: dylibs have it mapped at the start of __TEXT; executables keep a copy.
    if (img->segs[0].fileoff == 0 && img->segs[0].addr >= img->lo)
        img->header = img->segs[0].addr;
    else
        img->header = gheap_memdup(mh, sizeof *mh + mh->sizeofcmds);

    if (img->entry_is_main) {
        // entryoff is a file offset into __TEXT.
        for (int i = 0; i < img->nsegs; i++)
            if (img->entry >= img->segs[i].fileoff && img->entry < img->segs[i].fileoff + img->segs[i].filesize)
                img->entry = img->segs[i].addr + (img->entry - img->segs[i].fileoff);
    } else if (img->entry) {
        img->entry += img->slide;
    }

    LOGI("macho: loaded %s at %#x-%#x (slide %#x, %d segments, %d dylibs)", path, img->lo, img->hi,
         img->slide, img->nsegs, img->ndylibs);
    return img;
}

void macho_register(struct image *img) {
    if (nimages == MAX_IMAGES) fatal("too many images");
    strmap_init(&export_maps[nimages]);
    for (uint32_t i = 0; i < img->nsyms; i++) {
        const struct nlist *n = &img->syms[i];
        if ((n->n_type & N_STAB) || !(n->n_type & N_EXT)) continue;
        int type = n->n_type & N_TYPE;
        if (type != N_SECT && type != N_ABS) continue;
        uint32_t value = n->n_value + (type == N_SECT ? img->slide : 0);
        uint64_t packed = value | ((uint64_t)((n->n_desc & N_WEAK_DEF) ? 1 : 0) << 32);
        strmap_put(&export_maps[nimages], img->strtab + n->n_un.n_strx, packed, false);
    }
    images[nimages++] = img;
}

gaddr_t macho_export(const struct image *img, const char *name, bool *is_weak) {
    for (int i = 0; i < nimages; i++) {
        if (images[i] != img) continue;
        uint64_t v;
        if (!strmap_get(&export_maps[i], name, &v)) return 0;
        if (is_weak) *is_weak = (v >> 32) & 1;
        return (gaddr_t)v;
    }
    return 0;
}

const struct image_section *macho_section(const struct image *img, const char *seg, const char *sect) {
    for (int i = 0; i < img->nsects; i++)
        if (strncmp(img->sects[i].segname, seg, 16) == 0 && strncmp(img->sects[i].sectname, sect, 16) == 0)
            return &img->sects[i];
    return NULL;
}

const char *macho_describe_addr(gaddr_t addr, char *buf, size_t len) {
    for (int i = 0; i < nimages; i++) {
        const struct image *img = images[i];
        if (addr < img->lo || addr >= img->hi) continue;
        const char *base = strrchr(img->path, '/');
        // Nearest preceding exported symbol, if the image has any.
        const char *best = NULL;
        uint32_t best_addr = 0;
        for (uint32_t j = 0; j < img->nsyms; j++) {
            const struct nlist *n = &img->syms[j];
            if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT) continue;
            uint32_t a = n->n_value + img->slide;
            if (a <= addr && a >= best_addr) {
                best_addr = a;
                best = img->strtab + n->n_un.n_strx;
            }
        }
        if (best && addr - best_addr < 0x10000)
            snprintf(buf, len, "%s+%#x (%s+%#x)", base ? base + 1 : img->path, addr - img->lo + (img->lo - img->slide),
                     best, addr - best_addr);
        else
            snprintf(buf, len, "%s+%#x", base ? base + 1 : img->path, addr - img->slide);
        return buf;
    }
    snprintf(buf, len, "%#x", addr);
    return buf;
}

// ---------------------------------------------------------------------------
// Linking

static uint64_t read_uleb(const uint8_t **p, const uint8_t *end) {
    uint64_t r = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return r;
}

static int64_t read_sleb(const uint8_t **p, const uint8_t *end) {
    int64_t r = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        r |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
    return r;
}

// Address of a segment by its load-command index (dyld opcode numbering).
static gaddr_t seg_addr(const struct image *img, int file_index) {
    if (file_index < 0 || file_index >= img->nfile_segs || img->seg_by_file_index[file_index] < 0)
        fatal("macho: opcode references unmapped segment %d in %s", file_index, img->path);
    return img->segs[img->seg_by_file_index[file_index]].addr;
}

static void do_rebase_ops(struct image *img) {
    const struct dyld_info_command *di = img->dyld_info;
    const uint8_t *p = img->file + di->rebase_off, *end = p + di->rebase_size;
    uint32_t addr = 0;
    int type = REBASE_TYPE_POINTER;
    uint32_t count = 0;
    while (p < end) {
        uint8_t op = *p & REBASE_OPCODE_MASK, imm = *p & REBASE_IMMEDIATE_MASK;
        p++;
        switch (op) {
        case REBASE_OPCODE_DONE: p = end; break;
        case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            addr = seg_addr(img, imm) + (uint32_t)read_uleb(&p, end);
            break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: addr += (uint32_t)read_uleb(&p, end); break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: addr += imm * 4; break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
            uint64_t times = 1, skip = 0;
            if (op == REBASE_OPCODE_DO_REBASE_IMM_TIMES) times = imm;
            else if (op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES) times = read_uleb(&p, end);
            else if (op == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB) skip = read_uleb(&p, end);
            else {
                times = read_uleb(&p, end);
                skip = read_uleb(&p, end);
            }
            for (uint64_t t = 0; t < times; t++) {
                if (type == REBASE_TYPE_POINTER || type == REBASE_TYPE_TEXT_ABSOLUTE32)
                    gwrite32(addr, gread32(addr) + img->slide);
                count++;
                addr += 4 + (uint32_t)skip;
            }
            break;
        }
        default: fatal("macho: bad rebase opcode %#x in %s", op, img->path);
        }
    }
    LOGD("macho: %u rebases in %s", count, img->path);
}

static const char *ordinal_lib(const struct image *img, int ordinal) {
    if (ordinal == BIND_SPECIAL_DYLIB_SELF) return "<self>";
    if (ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE) return "<main>";
    if (ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) return "<flat>";
    if (ordinal >= 1 && ordinal <= img->ndylibs) return img->dylibs[ordinal - 1];
    return "<?>";
}

static uint32_t unresolved_count;

static gaddr_t resolve_or_stub(struct image *img, symbol_resolver resolve, const char *name, const char *lib,
                               bool weak) {
    gaddr_t a = resolve(name, lib, weak);
    if (a) return a;
    if (weak) return 0;
    unresolved_count++;
    LOGD("macho: unresolved %s (from %s) in %s", name, lib, img->path);
    return hostsym_missing_stub(name);
}

static void bind_write(gaddr_t addr, int type, gaddr_t value) {
    switch (type) {
    case BIND_TYPE_POINTER:
    case BIND_TYPE_TEXT_ABSOLUTE32: gwrite32(addr, value); break;
    case BIND_TYPE_TEXT_PCREL32: gwrite32(addr, value - (addr + 4)); break;
    default: fatal("macho: bad bind type %d", type);
    }
}

static void do_bind_ops(struct image *img, symbol_resolver resolve, uint32_t off, uint32_t size, bool lazy) {
    const uint8_t *p = img->file + off, *end = p + size;
    uint32_t addr = 0;
    int type = BIND_TYPE_POINTER, ordinal = 0, flags = 0;
    int64_t addend = 0;
    const char *sym = NULL;
    uint32_t count = 0;
    while (p < end) {
        uint8_t op = *p & BIND_OPCODE_MASK, imm = *p & BIND_IMMEDIATE_MASK;
        p++;
        switch (op) {
        case BIND_OPCODE_DONE:
            if (!lazy) p = end;
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = imm; break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ordinal = (int)read_uleb(&p, end); break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = imm ? (int)(int8_t)(BIND_OPCODE_MASK | imm) : 0; break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            flags = imm;
            p += strlen(sym) + 1;
            break;
        case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: addr = seg_addr(img, imm) + (uint32_t)read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: addr += (uint32_t)read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
            uint64_t times = 1, skip = 0;
            if (op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
                times = read_uleb(&p, end);
                skip = read_uleb(&p, end);
            }
            bool weak_import = (flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) ||
                               (ordinal >= 1 && ordinal <= img->ndylibs && img->dylib_weak[ordinal - 1]);
            gaddr_t target = resolve_or_stub(img, resolve, sym, ordinal_lib(img, ordinal), weak_import);
            for (uint64_t t = 0; t < times; t++) {
                bind_write(addr, type, target ? target + (gaddr_t)addend : 0);
                count++;
                addr += 4 + (uint32_t)skip;
            }
            if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) addr += (uint32_t)read_uleb(&p, end);
            else if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) addr += imm * 4;
            break;
        }
        default: fatal("macho: bad bind opcode %#x in %s", op, img->path);
        }
    }
    LOGD("macho: %u %sbinds in %s", count, lazy ? "lazy " : "", img->path);
}

// Weak binding: point coalesced weak definitions at the global winner.
static void do_weak_bind_ops(struct image *img, symbol_resolver resolve) {
    const struct dyld_info_command *di = img->dyld_info;
    const uint8_t *p = img->file + di->weak_bind_off, *end = p + di->weak_bind_size;
    uint32_t addr = 0;
    int type = BIND_TYPE_POINTER;
    int64_t addend = 0;
    const char *sym = NULL;
    gaddr_t target = 0;
    while (p < end) {
        uint8_t op = *p & BIND_OPCODE_MASK, imm = *p & BIND_IMMEDIATE_MASK;
        p++;
        switch (op) {
        case BIND_OPCODE_DONE: p = end; break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            sym = (const char *)p;
            p += strlen(sym) + 1;
            // A strong definition in this image needs no fixups; otherwise use the first definition.
            target = (imm & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) ? 0 : resolve(sym, "<weak>", true);
            break;
        case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
        case BIND_OPCODE_SET_ADDEND_SLEB: addend = read_sleb(&p, end); break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: addr = seg_addr(img, imm) + (uint32_t)read_uleb(&p, end); break;
        case BIND_OPCODE_ADD_ADDR_ULEB: addr += (uint32_t)read_uleb(&p, end); break;
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
            uint64_t times = 1, skip = 0;
            if (op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
                times = read_uleb(&p, end);
                skip = read_uleb(&p, end);
            }
            for (uint64_t t = 0; t < times; t++) {
                if (target) bind_write(addr, type, target + (gaddr_t)addend);
                addr += 4 + (uint32_t)skip;
            }
            if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) addr += (uint32_t)read_uleb(&p, end);
            else if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) addr += imm * 4;
            break;
        }
        default: fatal("macho: bad weak bind opcode %#x", op);
        }
    }
}

// Classic (pre-10.6) linking: relocation entries and indirect symbol tables.
static gaddr_t nlist_resolve(struct image *img, symbol_resolver resolve, uint32_t symidx) {
    const struct nlist *n = &img->syms[symidx];
    const char *name = img->strtab + n->n_un.n_strx;
    if ((n->n_type & N_TYPE) == N_SECT) return n->n_value + img->slide; // defined locally
    int ordinal = GET_LIBRARY_ORDINAL(n->n_desc);
    const char *lib = ordinal == SELF_LIBRARY_ORDINAL ? "<self>"
                      : ordinal == EXECUTABLE_ORDINAL ? "<main>"
                      : ordinal == DYNAMIC_LOOKUP_ORDINAL ? "<flat>"
                      : ordinal <= img->ndylibs ? img->dylibs[ordinal - 1] : "<?>";
    bool weak = (n->n_desc & N_WEAK_REF) || (ordinal >= 1 && ordinal <= img->ndylibs && img->dylib_weak[ordinal - 1]);
    return resolve_or_stub(img, resolve, name, lib, weak);
}

static void do_classic_link(struct image *img, symbol_resolver resolve) {
    const struct dysymtab_command *ds = img->dysym;
    if (!ds) return;
    // Relocation addresses are relative to the first segment (no MH_SPLIT_SEGS).
    uint32_t reloc_base = img->segs[0].addr;
    if (img->mh_flags & MH_SPLIT_SEGS) {
        for (int i = 0; i < img->nsegs; i++)
            if (img->segs[i].prot & GPROT_WRITE) {
                reloc_base = img->segs[i].addr;
                break;
            }
    }

    const struct relocation_info *rel = (const struct relocation_info *)(img->file + ds->locreloff);
    for (uint32_t i = 0; i < ds->nlocrel && img->slide; i++) {
        uint32_t raw = *(const uint32_t *)&rel[i];
        uint32_t address;
        int type, length;
        if (raw & R_SCATTERED) {
            const struct scattered_relocation_info *s = (const struct scattered_relocation_info *)&rel[i];
            address = s->r_address;
            type = s->r_type;
            length = s->r_length;
        } else {
            address = (uint32_t)rel[i].r_address;
            type = rel[i].r_type;
            length = rel[i].r_length;
        }
        if (type != GENERIC_RELOC_VANILLA || length != 2) {
            if (type == GENERIC_RELOC_PAIR) continue;
            LOGE("macho: unsupported local reloc type %d len %d in %s", type, length, img->path);
            continue;
        }
        gaddr_t a = reloc_base + address;
        gwrite32(a, gread32(a) + img->slide);
    }

    rel = (const struct relocation_info *)(img->file + ds->extreloff);
    for (uint32_t i = 0; i < ds->nextrel; i++) {
        if (!rel[i].r_extern || rel[i].r_type != GENERIC_RELOC_VANILLA || rel[i].r_length != 2) {
            LOGE("macho: unsupported external reloc in %s", img->path);
            continue;
        }
        gaddr_t a = reloc_base + (uint32_t)rel[i].r_address;
        gaddr_t target = nlist_resolve(img, resolve, rel[i].r_symbolnum);
        if (rel[i].r_pcrel)
            gwrite32(a, gread32(a) + target - img->slide);
        else
            gwrite32(a, gread32(a) + target);
    }

    const uint32_t *indirect = (const uint32_t *)(img->file + ds->indirectsymoff);
    for (int i = 0; i < img->nsects; i++) {
        struct image_section *s = &img->sects[i];
        int type = s->flags & SECTION_TYPE;
        uint32_t stride;
        if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) stride = 4;
        else if (type == S_SYMBOL_STUBS) stride = s->reserved2;
        else continue;
        for (uint32_t j = 0; j < s->size / stride; j++) {
            uint32_t symidx = indirect[s->reserved1 + j];
            gaddr_t slot = s->addr + j * stride;
            if (symidx & INDIRECT_SYMBOL_ABS) continue;
            gaddr_t target;
            if (symidx & INDIRECT_SYMBOL_LOCAL) {
                if (type == S_SYMBOL_STUBS) continue;
                gwrite32(slot, gread32(slot) + img->slide);
                continue;
            }
            target = nlist_resolve(img, resolve, symidx);
            if (type == S_SYMBOL_STUBS) {
                // i386 self-modifying jump table entry: jmp rel32
                if (stride < 5) fatal("macho: unexpected stub size %u", stride);
                gwrite8(slot, 0xE9);
                gwrite32(slot + 1, target - (slot + 5));
            } else {
                gwrite32(slot, target);
            }
        }
    }
}

void macho_link(struct image *img, symbol_resolver resolve) {
    unresolved_count = 0;
    if (img->dyld_info) {
        if (img->slide && img->dyld_info->rebase_size) do_rebase_ops(img);
        do_bind_ops(img, resolve, img->dyld_info->bind_off, img->dyld_info->bind_size, false);
        do_bind_ops(img, resolve, img->dyld_info->lazy_bind_off, img->dyld_info->lazy_bind_size, true);
        if (img->dyld_info->weak_bind_size) do_weak_bind_ops(img, resolve);
    } else {
        do_classic_link(img, resolve);
    }
    LOGI("macho: linked %s (%u unresolved symbols)", img->path, unresolved_count);
}
