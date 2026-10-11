// Loader for i386 Mach-O images (executables and dylibs).
#pragma once
#include "gmem.h"

#include <stdbool.h>

struct image_section {
    char segname[17], sectname[17];
    gaddr_t addr;
    uint32_t size, offset, flags, reserved1, reserved2;
    uint32_t reloff, nreloc;
};

struct image_segment {
    char name[17];
    gaddr_t addr; // after slide
    uint32_t vmsize, fileoff, filesize;
    int prot;
};

struct image {
    char *path;
    char *install_name;
    uint32_t filetype;
    int32_t slide;
    gaddr_t header;       // guest copy of the mach header + load commands
    gaddr_t lo, hi;       // mapped range
    uint8_t *file;        // host copy of the i386 slice
    size_t file_size;
    uint32_t mh_flags;

    struct image_segment segs[16];
    int nsegs;
    struct image_section *sects;
    int nsects;

    char **dylibs; // ordinal - 1
    bool *dylib_weak;
    int ndylibs;

    const struct nlist *syms;
    uint32_t nsyms;
    const char *strtab;
    const struct dysymtab_command *dysym;
    const struct dyld_info_command *dyld_info;

    gaddr_t entry; // LC_UNIXTHREAD eip / LC_MAIN, 0 for dylibs
    bool entry_is_main;
};

// Callback that resolves an imported symbol. Returns 0 if unresolved.
typedef gaddr_t (*symbol_resolver)(const char *name, const char *from_lib, bool weak_import);

struct image *macho_load(const char *path); // maps segments; no binding yet
// Apply rebases and bind imports. lazy binds are resolved eagerly.
void macho_link(struct image *img, symbol_resolver resolve);
// Look up a symbol exported by img (external, defined). Returns 0 if absent.
gaddr_t macho_export(const struct image *img, const char *name, bool *is_weak);
const struct image_section *macho_section(const struct image *img, const char *seg, const char *sect);
// Find which image/offset an address belongs to (for diagnostics).
const char *macho_describe_addr(gaddr_t addr, char *buf, size_t len);
void macho_register(struct image *img);
