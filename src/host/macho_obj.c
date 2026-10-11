// Runtime linker for i386 Mach-O relocatable objects (.o) and static archives (.a).
//
// Apple's ld can no longer link i386, so guest-side runtime code (libstdc++,
// libunwind, tests) is compiled to objects and linked here into one image.
// Same-named sections of all objects are laid out contiguously (like ld does),
// which keeps __eh_frame and __mod_init_func usable as single sections.
#include "log.h"
#include "macho.h"
#include "strmap.h"
#include "thunk.h"

#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct obj_sect {
    const struct section *hdr;
    gaddr_t new_addr;
    int group;
};

struct obj {
    const char *name;
    const uint8_t *data;
    const struct mach_header *mh;
    struct obj_sect *sects;
    uint32_t nsects;
    const struct nlist *syms;
    uint32_t nsyms;
    const char *strtab;
    const struct dysymtab_command *dysym;
    gaddr_t *sym_addr; // resolved address per symbol (0 = unresolved yet)
};

struct group {
    char seg[17], sect[17];
    uint32_t align, size, flags;
    gaddr_t addr;
};

static uint8_t *slurp(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    fstat(fd, &st);
    uint8_t *buf = malloc(st.st_size ? st.st_size : 1);
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

static bool parse_obj(struct obj *o, const char *name, const uint8_t *data, size_t size) {
    const struct mach_header *mh = (const struct mach_header *)data;
    if (size < sizeof *mh || mh->magic != MH_MAGIC || mh->cputype != CPU_TYPE_I386 || mh->filetype != MH_OBJECT) {
        LOGE("obj: %s is not an i386 object", name);
        return false;
    }
    memset(o, 0, sizeof *o);
    o->name = name;
    o->data = data;
    o->mh = mh;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *cmd = (const struct load_command *)lc;
        if (cmd->cmd == LC_SEGMENT) {
            const struct segment_command *sc = (const struct segment_command *)cmd;
            o->nsects = sc->nsects;
            o->sects = calloc(sc->nsects ? sc->nsects : 1, sizeof *o->sects);
            const struct section *s = (const struct section *)(sc + 1);
            for (uint32_t j = 0; j < sc->nsects; j++) o->sects[j].hdr = &s[j];
        } else if (cmd->cmd == LC_SYMTAB) {
            const struct symtab_command *st = (const struct symtab_command *)cmd;
            o->syms = (const struct nlist *)(data + st->symoff);
            o->nsyms = st->nsyms;
            o->strtab = (const char *)(data + st->stroff);
        } else if (cmd->cmd == LC_DYSYMTAB) {
            o->dysym = (const struct dysymtab_command *)cmd;
        }
    }
    o->sym_addr = calloc(o->nsyms ? o->nsyms : 1, sizeof(gaddr_t));
    return true;
}

// Address translation for an address in the object's original address space.
static int32_t obj_delta(const struct obj *o, uint32_t orig, int *sect_out) {
    for (uint32_t i = 0; i < o->nsects; i++) {
        const struct section *h = o->sects[i].hdr;
        if (orig >= h->addr && orig < h->addr + (h->size ? h->size : 1)) {
            if (sect_out) *sect_out = (int)i;
            return (int32_t)(o->sects[i].new_addr - h->addr);
        }
    }
    // One past the end of a section (e.g. section end symbols).
    for (uint32_t i = 0; i < o->nsects; i++) {
        const struct section *h = o->sects[i].hdr;
        if (orig == h->addr + h->size) return (int32_t)(o->sects[i].new_addr - h->addr);
    }
    LOGE("obj: %s: address %#x not in any section", o->name, orig);
    return 0;
}

static uint32_t read_field(gaddr_t a, int len) {
    switch (len) {
    case 0: return gread8(a);
    case 1: return gread16(a);
    default: return gread32(a);
    }
}

static void write_field(gaddr_t a, int len, uint32_t v) {
    switch (len) {
    case 0: gwrite8(a, (uint8_t)v); break;
    case 1: gwrite16(a, (uint16_t)v); break;
    default: gwrite32(a, v); break;
    }
}

static void relocate_section(struct obj *o, uint32_t si) {
    const struct section *h = o->sects[si].hdr;
    const struct relocation_info *rel = (const struct relocation_info *)(o->data + h->reloff);
    int32_t dp = (int32_t)(o->sects[si].new_addr - h->addr);
    for (uint32_t i = 0; i < h->nreloc; i++) {
        uint32_t raw = *(const uint32_t *)&rel[i];
        if (raw & R_SCATTERED) {
            const struct scattered_relocation_info *s = (const struct scattered_relocation_info *)&rel[i];
            gaddr_t loc = o->sects[si].new_addr + s->r_address;
            int len = s->r_length;
            uint32_t field = read_field(loc, len);
            if (s->r_type == GENERIC_RELOC_SECTDIFF || s->r_type == GENERIC_RELOC_LOCAL_SECTDIFF) {
                const struct scattered_relocation_info *pair = (const struct scattered_relocation_info *)&rel[++i];
                int32_t da = obj_delta(o, s->r_value, NULL);
                int32_t db = obj_delta(o, pair->r_value, NULL);
                write_field(loc, len, field + (uint32_t)(da - db));
            } else if (s->r_type == GENERIC_RELOC_VANILLA) {
                int32_t dt = obj_delta(o, s->r_value, NULL);
                write_field(loc, len, field + (uint32_t)dt - (s->r_pcrel ? (uint32_t)dp : 0));
            } else if (s->r_type == GENERIC_RELOC_PAIR) {
                continue;
            } else {
                LOGE("obj: %s: unsupported scattered reloc type %d", o->name, s->r_type);
            }
            continue;
        }
        const struct relocation_info *ri = &rel[i];
        gaddr_t loc = o->sects[si].new_addr + (uint32_t)ri->r_address;
        uint32_t field = read_field(loc, ri->r_length);
        if (ri->r_type != GENERIC_RELOC_VANILLA) {
            if (ri->r_type == GENERIC_RELOC_PAIR) continue;
            LOGE("obj: %s: unsupported reloc type %d", o->name, ri->r_type);
            continue;
        }
        if (ri->r_extern) {
            gaddr_t sym = o->sym_addr[ri->r_symbolnum];
            write_field(loc, ri->r_length, field + sym - (ri->r_pcrel ? (uint32_t)dp : 0));
        } else {
            if (ri->r_symbolnum == R_ABS) continue;
            uint32_t ts = (uint32_t)ri->r_symbolnum - 1;
            int32_t dt = (int32_t)(o->sects[ts].new_addr - o->sects[ts].hdr->addr);
            write_field(loc, ri->r_length, field + (uint32_t)dt - (ri->r_pcrel ? (uint32_t)dp : 0));
        }
    }
}

static void add_member(struct obj **objs, int *n, int *cap, const char *name, const uint8_t *data, size_t size) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *objs = realloc(*objs, sizeof(struct obj) * (size_t)*cap);
    }
    if (parse_obj(&(*objs)[*n], name, data, size)) (*n)++;
}

// BSD ar format with "#1/<len>" extended names (what Apple's ar/libtool write).
static void add_archive(struct obj **objs, int *n, int *cap, const char *path, const uint8_t *data, size_t size) {
    size_t off = 8;
    while (off + 60 <= size) {
        const char *hdr = (const char *)data + off;
        char name[17] = {0}, sizestr[11] = {0};
        memcpy(name, hdr, 16);
        memcpy(sizestr, hdr + 48, 10);
        size_t msize = strtoul(sizestr, NULL, 10);
        size_t body = off + 60;
        const uint8_t *mdata = data + body;
        size_t mlen = msize;
        char *mname;
        if (strncmp(name, "#1/", 3) == 0) {
            size_t nl = strtoul(name + 3, NULL, 10);
            mname = strndup((const char *)mdata, nl);
            mdata += nl;
            mlen -= nl;
        } else {
            char *end = name + 15;
            while (end >= name && (*end == ' ' || *end == '/' || *end == 0)) *end-- = 0;
            mname = strdup(name);
        }
        if (strncmp(mname, "__.SYMDEF", 9) != 0 && mlen >= 4 && *(const uint32_t *)mdata == MH_MAGIC) {
            char *full;
            asprintf(&full, "%s(%s)", path, mname);
            add_member(objs, n, cap, full, mdata, mlen);
        }
        free(mname);
        off = body + msize;
        if (off & 1) off++;
    }
}

struct image *macho_load_objects(const char *name, const char *const *paths, int npaths, symbol_resolver resolve) {
    struct obj *objs = NULL;
    int nobjs = 0, cap = 0;
    for (int i = 0; i < npaths; i++) {
        size_t size;
        uint8_t *data = slurp(paths[i], &size);
        if (!data) {
            LOGE("obj: cannot read %s", paths[i]);
            continue;
        }
        if (size >= 8 && memcmp(data, "!<arch>\n", 8) == 0)
            add_archive(&objs, &nobjs, &cap, paths[i], data, size);
        else
            add_member(&objs, &nobjs, &cap, strdup(paths[i]), data, size);
    }
    if (!nobjs) return NULL;

    // Global symbol table: name -> (obj index, sym index) of the winning definition.
    struct strmap defs;
    strmap_init(&defs);
    for (int k = 0; k < nobjs; k++) {
        struct obj *o = &objs[k];
        for (uint32_t i = 0; i < o->nsyms; i++) {
            const struct nlist *s = &o->syms[i];
            if ((s->n_type & N_STAB) || !(s->n_type & N_EXT)) continue;
            int t = s->n_type & N_TYPE;
            if (t != N_SECT && t != N_ABS) continue;
            const char *nm = o->strtab + s->n_un.n_strx;
            uint64_t prev;
            bool weak = (s->n_desc & N_WEAK_DEF) != 0;
            if (strmap_get(&defs, nm, &prev)) {
                // Keep the first definition, but a strong one beats a weak one.
                struct obj *po = &objs[prev >> 32];
                bool prev_weak = (po->syms[(uint32_t)prev].n_desc & N_WEAK_DEF) != 0;
                if (prev_weak && !weak) strmap_put(&defs, nm, ((uint64_t)k << 32) | i, true);
                else if (!prev_weak && !weak) LOGD("obj: duplicate symbol %s in %s", nm, o->name);
                continue;
            }
            strmap_put(&defs, nm, ((uint64_t)k << 32) | i, false);
        }
    }

    // Group sections by name and compute layout.
    struct group *groups = NULL;
    int ngroups = 0, gcap = 0;
    for (int k = 0; k < nobjs; k++) {
        for (uint32_t i = 0; i < objs[k].nsects; i++) {
            const struct section *h = objs[k].sects[i].hdr;
            int g;
            for (g = 0; g < ngroups; g++)
                if (!strncmp(groups[g].seg, h->segname, 16) && !strncmp(groups[g].sect, h->sectname, 16)) break;
            if (g == ngroups) {
                if (ngroups == gcap) groups = realloc(groups, sizeof *groups * (size_t)(gcap = gcap ? gcap * 2 : 32));
                memset(&groups[g], 0, sizeof groups[g]);
                memcpy(groups[g].seg, h->segname, 16);
                memcpy(groups[g].sect, h->sectname, 16);
                groups[g].flags = h->flags;
                ngroups++;
            }
            objs[k].sects[i].group = g;
            uint32_t align = 1u << h->align;
            if (align > groups[g].align) groups[g].align = align;
            groups[g].size = (groups[g].size + align - 1) & ~(align - 1);
            objs[k].sects[i].new_addr = groups[g].size; // offset within group for now
            groups[g].size += h->size;
        }
    }
    // Common symbols not defined elsewhere go into a __DATA,__common group.
    struct strmap commons;
    strmap_init(&commons);
    uint32_t common_size = 0;
    for (int k = 0; k < nobjs; k++) {
        struct obj *o = &objs[k];
        for (uint32_t i = 0; i < o->nsyms; i++) {
            const struct nlist *s = &o->syms[i];
            if ((s->n_type & N_TYPE) != N_UNDF || !(s->n_type & N_EXT) || s->n_value == 0) continue;
            const char *nm = o->strtab + s->n_un.n_strx;
            if (strmap_get(&defs, nm, NULL) || strmap_get(&commons, nm, NULL)) continue;
            uint32_t align = 1u << GET_COMM_ALIGN(s->n_desc);
            if (align < 4) align = 4;
            common_size = (common_size + align - 1) & ~(align - 1);
            strmap_put(&commons, nm, common_size, false);
            common_size += s->n_value;
        }
    }

    // Code and read-only sections first, then writable data on separate pages.
    // Rosetta mistranslates later mode switches once guest code writes to a page
    // that holds translated code, so the two must never share a page.
    uint32_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) total = (total + GPAGE_SIZE - 1) & ~(GPAGE_SIZE - 1);
        for (int g = 0; g < ngroups; g++) {
            bool code = strcmp(groups[g].seg, "__TEXT") == 0 ||
                        (groups[g].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS));
            if (code != (pass == 0)) continue;
            uint32_t a = groups[g].align ? groups[g].align : 1;
            if (a < 16) a = 16;
            total = (total + a - 1) & ~(a - 1);
            groups[g].addr = total;
            total += groups[g].size;
        }
    }
    uint32_t common_off = (total + 15) & ~15u;
    total = common_off + common_size;

    gaddr_t base = gmem_map_anywhere(total ? total : 1, GPROT_RWX, 0);
    if (!base) fatal("obj: no memory for %s", name);
    for (int g = 0; g < ngroups; g++) groups[g].addr += base;
    for (int k = 0; k < nobjs; k++) {
        for (uint32_t i = 0; i < objs[k].nsects; i++) {
            struct obj_sect *s = &objs[k].sects[i];
            s->new_addr += groups[s->group].addr;
            int type = s->hdr->flags & SECTION_TYPE;
            if (type != S_ZEROFILL && type != S_GB_ZEROFILL && s->hdr->size)
                memcpy(g2h(s->new_addr), objs[k].data + s->hdr->offset, s->hdr->size);
        }
    }

    // The image's mach header (ld normally synthesises ___dso_handle pointing at it).
    gaddr_t hdr = gheap_calloc(1, sizeof(struct mach_header));
    struct mach_header *mh = g2h(hdr);
    mh->magic = MH_MAGIC;
    mh->cputype = CPU_TYPE_I386;
    mh->cpusubtype = CPU_SUBTYPE_I386_ALL;
    mh->filetype = MH_DYLIB;

    // Resolve every symbol's address.
    unsigned unresolved = 0;
    for (int k = 0; k < nobjs; k++) {
        struct obj *o = &objs[k];
        for (uint32_t i = 0; i < o->nsyms; i++) {
            const struct nlist *s = &o->syms[i];
            if (s->n_type & N_STAB) continue;
            const char *nm = o->strtab + s->n_un.n_strx;
            int t = s->n_type & N_TYPE;
            uint64_t def;
            if ((s->n_type & N_EXT) && strmap_get(&defs, nm, &def)) {
                struct obj *d = &objs[def >> 32];
                const struct nlist *ds = &d->syms[(uint32_t)def];
                if ((ds->n_type & N_TYPE) == N_ABS) o->sym_addr[i] = ds->n_value;
                else o->sym_addr[i] = ds->n_value + (uint32_t)obj_delta(d, ds->n_value, NULL);
            } else if (t == N_SECT) {
                o->sym_addr[i] = s->n_value + (uint32_t)obj_delta(o, s->n_value, NULL);
            } else if (t == N_ABS) {
                o->sym_addr[i] = s->n_value;
            } else if (t == N_UNDF) {
                uint64_t coff;
                if (strcmp(nm, "___dso_handle") == 0) {
                    o->sym_addr[i] = hdr;
                } else if (strmap_get(&commons, nm, &coff)) {
                    o->sym_addr[i] = base + common_off + (uint32_t)coff;
                } else {
                    gaddr_t a = resolve ? resolve(nm, name, (s->n_desc & N_WEAK_REF) != 0) : 0;
                    if (!a && !(s->n_desc & N_WEAK_REF)) {
                        a = hostsym_missing_stub(nm);
                        unresolved++;
                        LOGD("obj: unresolved %s (%s)", nm, o->name);
                    }
                    o->sym_addr[i] = a;
                }
            }
        }
    }

    // Relocations and indirect symbol pointers.
    for (int k = 0; k < nobjs; k++) {
        struct obj *o = &objs[k];
        for (uint32_t i = 0; i < o->nsects; i++) relocate_section(o, i);
        if (!o->dysym) continue;
        const uint32_t *ind = (const uint32_t *)(o->data + o->dysym->indirectsymoff);
        for (uint32_t i = 0; i < o->nsects; i++) {
            const struct section *h = o->sects[i].hdr;
            int type = h->flags & SECTION_TYPE;
            uint32_t stride;
            if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) stride = 4;
            else if (type == S_SYMBOL_STUBS) stride = h->reserved2;
            else continue;
            for (uint32_t j = 0; j < h->size / stride; j++) {
                uint32_t idx = ind[h->reserved1 + j];
                gaddr_t slot = o->sects[i].new_addr + j * stride;
                if (idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
                    if ((idx & INDIRECT_SYMBOL_LOCAL) && type != S_SYMBOL_STUBS) {
                        uint32_t v = gread32(slot);
                        gwrite32(slot, v + (uint32_t)obj_delta(o, v, NULL));
                    }
                    continue;
                }
                gaddr_t target = o->sym_addr[idx];
                if (type == S_SYMBOL_STUBS) {
                    gwrite8(slot, 0xE9);
                    gwrite32(slot + 1, target - (slot + 5));
                } else {
                    gwrite32(slot, target);
                }
            }
        }
    }

    // Build the image: one section per group, a synthetic symbol table.
    struct image *img = calloc(1, sizeof *img);
    img->path = strdup(name);
    img->install_name = strdup(name);
    img->filetype = MH_DYLIB;
    img->lo = base;
    img->hi = base + ((total + GPAGE_SIZE - 1) & ~(GPAGE_SIZE - 1));
    img->nsects = ngroups;
    img->sects = calloc(ngroups ? ngroups : 1, sizeof *img->sects);
    for (int g = 0; g < ngroups; g++) {
        memcpy(img->sects[g].segname, groups[g].seg, 16);
        memcpy(img->sects[g].sectname, groups[g].sect, 16);
        img->sects[g].addr = groups[g].addr;
        img->sects[g].size = groups[g].size;
        img->sects[g].flags = groups[g].flags;
    }
    img->nsegs = 1;
    strcpy(img->segs[0].name, "__TEXT");
    img->segs[0].addr = base;
    img->segs[0].vmsize = total;
    img->segs[0].prot = GPROT_RWX;

    size_t nsyms = 0, strsize = 1;
    for (int k = 0; k < nobjs; k++)
        for (uint32_t i = 0; i < objs[k].nsyms; i++) {
            const struct nlist *s = &objs[k].syms[i];
            if (!(s->n_type & N_STAB) && (s->n_type & N_TYPE) == N_SECT) {
                nsyms++;
                strsize += strlen(objs[k].strtab + s->n_un.n_strx) + 1;
            }
        }
    struct nlist *syms = calloc(nsyms ? nsyms : 1, sizeof *syms);
    char *strtab = calloc(strsize, 1);
    size_t si = 0, so = 1;
    for (int k = 0; k < nobjs; k++)
        for (uint32_t i = 0; i < objs[k].nsyms; i++) {
            const struct nlist *s = &objs[k].syms[i];
            if ((s->n_type & N_STAB) || (s->n_type & N_TYPE) != N_SECT) continue;
            const char *nm = objs[k].strtab + s->n_un.n_strx;
            syms[si] = *s;
            syms[si].n_un.n_strx = (uint32_t)so;
            syms[si].n_value = objs[k].sym_addr[i];
            // Losing duplicates of weak symbols should not be exported.
            uint64_t def;
            if ((s->n_type & N_EXT) && strmap_get(&defs, nm, &def) && ((def >> 32) != (uint64_t)k || (uint32_t)def != i))
                syms[si].n_type &= ~N_EXT;
            strcpy(strtab + so, nm);
            so += strlen(nm) + 1;
            si++;
        }
    img->syms = syms;
    img->nsyms = (uint32_t)si;
    img->strtab = strtab;
    img->header = hdr;

    LOGI("obj: linked %s from %d objects at %#x-%#x (%u unresolved)", name, nobjs, img->lo, img->hi, unresolved);
    return img;
}
