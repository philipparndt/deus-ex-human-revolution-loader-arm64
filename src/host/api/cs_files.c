// CoreServices File Manager (FSRef / fork APIs pass straight to the host),
// a small emulation of the FSSpec-based APIs that 64-bit macOS removed,
// FindFolder, FSFileOperation and the Resource Manager entry points the game uses.
//
// FSRef: 80 opaque bytes with identical layout on both sides -> g2h() directly.
// Structs that differ (i386 vs x86_64):
//   FSCatalogInfo   144 vs 148: FSPermissionInfo.fileSec is a pointer (4 vs 8 bytes);
//                   fields from finderInfo on are shifted by 4.
//   FSVolumeInfo    126 vs 128: same fields, only tail padding differs.
//   GetVolParmsInfoBuffer 32 vs 44: vMLocalHand is a Handle (pointer).
//   FSIORefNum/ResFileRefNum: SInt16 in the guest, int on the host.
//   ByteCount/ItemCount/UniCharCount: 32-bit in the guest.
#include "cf_bridge.h"

#include <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

__asm__(".linker_option \"-framework\", \"CoreServices\"");

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define GRET_ERR(e) RET((uint32_t)(int32_t)(e))
#define FAIL(e)                                                                                    \
    do {                                                                                           \
        GRET_ERR(e);                                                                               \
        return;                                                                                    \
    } while (0)

// --- struct conversion --------------------------------------------------------------
#define G_CATINFO_SIZE 144
_Static_assert(sizeof(FSCatalogInfo) == 148, "host FSCatalogInfo");
_Static_assert(offsetof(FSCatalogInfo, finderInfo) == 76, "host FSCatalogInfo.finderInfo");

static void catinfo_to_guest(gaddr_t g, const FSCatalogInfo *h) {
    const uint8_t *hb = (const uint8_t *)h;
    memcpy(g2h(g), hb, 68);              // up to FSPermissionInfo.mode
    gwrite32(g + 68, 0);                 // fileSec (no ACL object for the guest)
    memcpy(g2h(g + 72), hb + 76, 72);    // finderInfo .. textEncodingHint
}

static void catinfo_from_guest(FSCatalogInfo *h, gaddr_t g) {
    uint8_t *hb = (uint8_t *)h;
    memset(h, 0, sizeof *h);
    memcpy(hb, g2h(g), 68);
    memcpy(hb + 76, g2h(g + 72), 72);
}

// --- FSRef APIs -----------------------------------------------------------------------
THUNK(FSPathMakeRef) {
    Boolean dir = 0;
    OSStatus st = FSPathMakeRef((const UInt8 *)ARGP(0), (FSRef *)ARGP(1), &dir);
    if (ARG(2)) gwrite8(ARG(2), dir);
    GRET_ERR(st);
}

THUNK(FSRefMakePath) { GRET_ERR(FSRefMakePath((const FSRef *)ARGP(0), (UInt8 *)ARGP(1), ARG(2))); }

THUNK(FSCompareFSRefs) { GRET_ERR(FSCompareFSRefs((const FSRef *)ARGP(0), (const FSRef *)ARGP(1))); }

THUNK(FSExchangeObjects) { GRET_ERR(FSExchangeObjects((const FSRef *)ARGP(0), (const FSRef *)ARGP(1))); }

THUNK(FSDeleteObject) { GRET_ERR(FSDeleteObject((const FSRef *)ARGP(0))); }

THUNK(FSMakeFSRefUnicode) {
    GRET_ERR(FSMakeFSRefUnicode((const FSRef *)ARGP(0), ARG(1), (const UniChar *)ARGP(2), ARG(3), (FSRef *)ARGP(4)));
}

THUNK(FSGetDataForkName) { GRET_ERR(FSGetDataForkName((HFSUniStr255 *)ARGP(0))); }

THUNK(FSIsAliasFile) {
    Boolean alias = 0, folder = 0;
    OSErr e = FSIsAliasFile((const FSRef *)ARGP(0), &alias, &folder);
    if (ARG(1)) gwrite8(ARG(1), alias);
    if (ARG(2)) gwrite8(ARG(2), folder);
    GRET_ERR(e);
}

// FSGetCatalogInfo(ref, whichInfo, catalogInfo, outName, fsSpec, parentRef)
THUNK(FSGetCatalogInfo) {
    FSCatalogInfo ci;
    // FSSpec is 70 bytes on both sides (opaque on x86_64): pass through.
    OSErr e = FSGetCatalogInfo((const FSRef *)ARGP(0), ARG(1), ARG(2) ? &ci : NULL, (HFSUniStr255 *)ARGP(3),
                               (FSSpecPtr)ARGP(4), (FSRef *)ARGP(5));
    if (e == noErr && ARG(2)) catinfo_to_guest(ARG(2), &ci);
    GRET_ERR(e);
}

THUNK(FSSetCatalogInfo) {
    FSCatalogInfo ci;
    catinfo_from_guest(&ci, ARG(2));
    GRET_ERR(FSSetCatalogInfo((const FSRef *)ARGP(0), ARG(1), &ci));
}

// FSGetVolumeInfo(volume, volumeIndex, actualVolume*, whichInfo, info*, volumeName*, rootDirectory*)
THUNK(FSGetVolumeInfo) {
    FSVolumeInfo vi;
    FSVolumeRefNum actual = 0;
    OSErr e = FSGetVolumeInfo((FSVolumeRefNum)ARG(0), ARG(1), &actual, ARG(3), ARG(4) ? &vi : NULL,
                              (HFSUniStr255 *)ARGP(5), (FSRef *)ARGP(6));
    if (e == noErr) {
        if (ARG(2)) gwrite16(ARG(2), (uint16_t)actual);
        if (ARG(4)) memcpy(g2h(ARG(4)), &vi, 126);
    }
    GRET_ERR(e);
}

// FSGetVolumeParms(volume, GetVolParmsInfoBuffer*, ByteCount bufferSize)
THUNK(FSGetVolumeParms) {
    GetVolParmsInfoBuffer b;
    memset(&b, 0, sizeof b);
    OSStatus e = FSGetVolumeParms((FSVolumeRefNum)ARG(0), &b, sizeof b);
    gaddr_t g = ARG(1);
    uint32_t cap = ARG(2);
    if (e == noErr && g) {
        uint8_t tmp[32];
        memset(tmp, 0, sizeof tmp);
        memcpy(tmp + 0, &b.vMVersion, 2);
        memcpy(tmp + 2, &b.vMAttrib, 4);
        // vMLocalHand (Handle) @6: not exposed
        memcpy(tmp + 10, &b.vMServerAdr, 4);
        memcpy(tmp + 14, &b.vMVolumeGrade, 4);
        memcpy(tmp + 18, &b.vMForeignPrivID, 2);
        memcpy(tmp + 20, &b.vMExtendedAttributes, 4);
        // vMDeviceID (void *) @24: device name string -> guest copy
        if (b.vMDeviceID) {
            static gaddr_t devname;
            if (devname) gheap_free(devname);
            devname = gheap_strdup((const char *)b.vMDeviceID);
            memcpy(tmp + 24, &devname, 4);
        }
        uint32_t maxlen = (uint32_t)b.vMMaxNameLength;
        memcpy(tmp + 28, &maxlen, 4);
        memcpy(g2h(g), tmp, cap < 32 ? cap : 32);
    }
    GRET_ERR(e);
}

THUNK(FSFindFolder) {
    GRET_ERR(FSFindFolder((FSVolumeRefNum)ARG(0), ARG(1), ARGBOOL(2), (FSRef *)ARGP(3)));
}

// --- forks ----------------------------------------------------------------------------------
// FSOpenFork(ref, forkNameLength, forkName, SInt8 permissions, FSIORefNum *forkRefNum)
THUNK(FSOpenFork) {
    FSIORefNum ref = 0;
    OSErr e = FSOpenFork((const FSRef *)ARGP(0), ARG(1), (const UniChar *)ARGP(2), (SInt8)ARG(3), &ref);
    if (e == noErr && ARG(4)) {
        if (ref > 32767 || ref < -32768) LOGE("FSOpenFork: host fork refnum %d does not fit SInt16", ref);
        gwrite16(ARG(4), (uint16_t)ref);
    }
    GRET_ERR(e);
}

THUNK(FSCloseFork) { GRET_ERR(FSCloseFork((int16_t)ARG(0))); }

// FSReadFork(ref, UInt16 positionMode, SInt64 positionOffset, ByteCount requestCount, void *buffer, ByteCount *actual)
THUNK(FSReadFork) {
    ByteCount actual = 0;
    OSErr e = FSReadFork((int16_t)ARG(0), (UInt16)ARG(1), (SInt64)ARG64(2), ARG(4), ARGP(5), &actual);
    if (ARG(6)) gwrite32(ARG(6), (uint32_t)actual);
    GRET_ERR(e);
}

THUNK(FSWriteFork) {
    ByteCount actual = 0;
    OSErr e = FSWriteFork((int16_t)ARG(0), (UInt16)ARG(1), (SInt64)ARG64(2), ARG(4), ARGP(5), &actual);
    if (ARG(6)) gwrite32(ARG(6), (uint32_t)actual);
    GRET_ERR(e);
}

THUNK(FSSetForkPosition) { GRET_ERR(FSSetForkPosition((int16_t)ARG(0), (UInt16)ARG(1), (SInt64)ARG64(2))); }

THUNK(FSGetForkPosition) {
    SInt64 pos = 0;
    OSErr e = FSGetForkPosition((int16_t)ARG(0), &pos);
    if (ARG(1)) {
        gwrite32(ARG(1), (uint32_t)pos);
        gwrite32(ARG(1) + 4, (uint32_t)((uint64_t)pos >> 32));
    }
    GRET_ERR(e);
}

THUNK(FSGetForkSize) {
    SInt64 size = 0;
    OSErr e = FSGetForkSize((int16_t)ARG(0), &size);
    if (ARG(1)) {
        gwrite32(ARG(1), (uint32_t)size);
        gwrite32(ARG(1) + 4, (uint32_t)((uint64_t)size >> 32));
    }
    GRET_ERR(e);
}

// --- guest Handles ---------------------------------------------------------------------------
// A guest Handle is a guest master pointer (4 bytes) to a guest data block.
static gaddr_t guest_handle_new(const void *data, uint32_t size) {
    gaddr_t mp = gheap_calloc(1, 16);
    gaddr_t blk = gheap_alloc(size ? size : 1);
    if (size && data) memcpy(g2h(blk), data, size);
    gwrite32(mp, blk);
    return mp;
}
static void guest_handle_free(gaddr_t h) {
    if (!h) return;
    gheap_free(gread32(h));
    gheap_free(h);
}

// FSNewAlias(fromFile, target, AliasHandle *inAlias): the guest gets a guest Handle
// with a copy of the alias record.
THUNK(FSNewAlias) {
    AliasHandle a = NULL;
    OSErr e = FSNewAlias((const FSRef *)ARGP(0), (const FSRef *)ARGP(1), &a);
    if (e == noErr && a) {
        Size sz = GetHandleSize((Handle)a);
        if (ARG(2)) gwrite32(ARG(2), guest_handle_new(*a, (uint32_t)sz));
        DisposeHandle((Handle)a);
    } else if (ARG(2))
        gwrite32(ARG(2), 0);
    GRET_ERR(e);
}

// --- FSFileOperation ---------------------------------------------------------------------------
THUNK(FSFileOperationCreate) { RET_CF(FSFileOperationCreate(gcf_allocator(ARG(0)))); }
THUNK(FSFileOperationScheduleWithRunLoop) {
    GRET_ERR(FSFileOperationScheduleWithRunLoop((FSFileOperationRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2)));
}
THUNK(FSFileOperationUnscheduleFromRunLoop) {
    GRET_ERR(FSFileOperationUnscheduleFromRunLoop((FSFileOperationRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2)));
}
THUNK(FSFileOperationCancel) { GRET_ERR(FSFileOperationCancel((FSFileOperationRef)ARGCF(0))); }

struct fsop_ctx {
    gaddr_t callback;
    struct gcf_context g;
    gaddr_t refbuf; // guest FSRef buffer for the "current item" argument
};

static void fsop_status(FSFileOperationRef op, const FSRef *item, FSFileOperationStage stage, OSStatus err,
                        CFDictionaryRef status, void *info) {
    struct fsop_ctx *c = info;
    if (!c->callback) return;
    if (item) memcpy(g2h(c->refbuf), item, sizeof(FSRef));
    uint32_t args[6] = {cfg(op), item ? c->refbuf : 0, stage, (uint32_t)err, cfg(status), c->g.info};
    cpu_call(c->callback, 6, args);
}

static void fsop_release(void *info) {
    struct fsop_ctx *c = info;
    gcf_context_release(&c->g);
    gheap_free(c->refbuf);
    free(c);
}

// FSCopyObjectAsync(op, source, destDir, destName, flags, callback, double interval, clientContext)
THUNK(FSCopyObjectAsync) {
    struct fsop_ctx *c = calloc(1, sizeof *c);
    c->callback = ARG(5);
    c->refbuf = gheap_calloc(1, sizeof(FSRef));
    gcf_context_read(ARG(8), &c->g, true);
    FSFileOperationClientContext hc = {0, c, NULL, (CFAllocatorReleaseCallBack)fsop_release, NULL};
    OSStatus st = FSCopyObjectAsync((FSFileOperationRef)ARGCF(0), (const FSRef *)ARGP(1), (const FSRef *)ARGP(2),
                                    ARGSTR(3), ARG(4), c->callback ? fsop_status : NULL, ARGD(6), &hc);
    if (st != noErr) fsop_release(c);
    GRET_ERR(st);
}

// --- FindFolder + FSSpec emulation ---------------------------------------------------------------
// FSSpec (i386, pack 2): SInt16 vRefNum @0; SInt32 parID @2; Str63 name @6 (70 bytes).
// Directories are identified by IDs in a side table (path strings). IDs handed
// out by FindFolder are the real node IDs; directories first seen through
// FSMakeFSSpec get synthetic IDs from SYNTH_DIR_BASE.
#define SYNTH_DIR_BASE 0x40000000
#define FAKE_VREFNUM (-100)

static pthread_mutex_t spec_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dirent_ {
    int16_t vref;
    int32_t id;
    char *path;
} *dirs;
static int ndirs, capdirs;
static int32_t next_synth = SYNTH_DIR_BASE;

static void dir_add_locked(int16_t vref, int32_t id, const char *path) {
    if (ndirs == capdirs) {
        capdirs = capdirs ? capdirs * 2 : 64;
        dirs = realloc(dirs, sizeof *dirs * capdirs);
    }
    dirs[ndirs++] = (struct dirent_){vref, id, strdup(path)};
}

static const char *dir_lookup_locked(int16_t vref, int32_t id) {
    for (int i = 0; i < ndirs; i++)
        if (dirs[i].id == id && (id >= SYNTH_DIR_BASE || dirs[i].vref == vref || vref == 0)) return dirs[i].path;
    return NULL;
}

// Directory ID for a host directory path (synthetic unless already known).
static int32_t dir_id_for(const char *path) {
    pthread_mutex_lock(&spec_lock);
    for (int i = 0; i < ndirs; i++)
        if (strcmp(dirs[i].path, path) == 0) {
            int32_t id = dirs[i].id;
            pthread_mutex_unlock(&spec_lock);
            return id;
        }
    int32_t id = next_synth++;
    dir_add_locked(FAKE_VREFNUM, id, path);
    pthread_mutex_unlock(&spec_lock);
    return id;
}

// Resolve (vRefNum, dirID) to a host directory path. Returns false if unknown.
static bool dir_path(int16_t vref, int32_t id, char *out, size_t cap) {
    if (id == 0 || id == 1) { // default / root-parent
        if (id == 0) {
            if (!getcwd(out, cap)) return false;
        } else
            strlcpy(out, "/", cap);
        return true;
    }
    if (id == 2) { // fsRtDirID: volume root
        strlcpy(out, "/", cap);
        return true;
    }
    pthread_mutex_lock(&spec_lock);
    const char *p = dir_lookup_locked(vref, id);
    if (p) strlcpy(out, p, cap);
    pthread_mutex_unlock(&spec_lock);
    if (p) return true;
    // Fall back to volfs (/.vol/<dev>/<id>) for real node IDs.
    FSRef root;
    if (FSGetVolumeInfo(vref, 0, NULL, kFSVolInfoNone, NULL, NULL, &root) == noErr) {
        char rp[PATH_MAX];
        struct stat st;
        if (FSRefMakePath(&root, (UInt8 *)rp, sizeof rp) == noErr && stat(rp, &st) == 0) {
            char vol[64];
            snprintf(vol, sizeof vol, "/.vol/%d/%d", (int)st.st_dev, (int)id);
            if (realpath(vol, out)) return true;
        }
    }
    return false;
}

// FindFolder(vRefNum, folderType, createFolder, FSVolumeRefNum *foundVRefNum, SInt32 *foundDirID)
THUNK(FindFolder) {
    FSRef ref;
    OSErr e = FSFindFolder((FSVolumeRefNum)ARG(0), ARG(1), ARGBOOL(2), &ref);
    if (e == noErr) {
        FSCatalogInfo ci;
        char path[PATH_MAX];
        e = FSGetCatalogInfo(&ref, kFSCatInfoVolume | kFSCatInfoNodeID, &ci, NULL, NULL, NULL);
        if (e == noErr && FSRefMakePath(&ref, (UInt8 *)path, sizeof path) == noErr) {
            pthread_mutex_lock(&spec_lock);
            if (!dir_lookup_locked(ci.volume, (int32_t)ci.nodeID)) dir_add_locked(ci.volume, (int32_t)ci.nodeID, path);
            pthread_mutex_unlock(&spec_lock);
        }
        if (e == noErr) {
            if (ARG(3)) gwrite16(ARG(3), (uint16_t)ci.volume);
            if (ARG(4)) gwrite32(ARG(4), ci.nodeID);
        }
    }
    GRET_ERR(e);
}

// Pascal MacRoman string -> UTF-8 (HFS ':' in names becomes '/' on the host and vice versa).
static void pstr_to_utf8(gaddr_t p, char *out, size_t cap) {
    out[0] = 0;
    if (!p) return;
    uint8_t n = gread8(p);
    CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)g2h(p + 1), n, kCFStringEncodingMacRoman, false);
    if (!s) return;
    CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(s);
}

static void utf8_to_pstr(const char *in, uint8_t *out, int maxlen) {
    char tmp[256];
    strlcpy(tmp, in, sizeof tmp);
    for (char *c = tmp; *c; c++)
        if (*c == ':') *c = '/';
    CFStringRef s = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
    CFIndex used = 0;
    if (s) {
        CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingMacRoman, '?', false, out + 1,
                         maxlen, &used);
        CFRelease(s);
    }
    out[0] = (uint8_t)used;
}

static void boot_volume_name(char *out, size_t cap) {
    HFSUniStr255 name;
    FSRef root;
    out[0] = 0;
    if (FSPathMakeRef((const UInt8 *)"/", &root, NULL) != noErr) return;
    FSVolumeRefNum vol;
    FSCatalogInfo ci;
    if (FSGetCatalogInfo(&root, kFSCatInfoVolume, &ci, NULL, NULL, NULL) != noErr) return;
    vol = ci.volume;
    if (FSGetVolumeInfo(vol, 0, NULL, kFSVolInfoNone, NULL, &name, NULL) != noErr) return;
    CFStringRef s = CFStringCreateWithCharacters(NULL, name.unicode, name.length);
    if (s) {
        CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

// Build a host path from (vRefNum, dirID, HFS name/partial path).
static OSErr hfs_to_path(int16_t vref, int32_t dirID, const char *name, char *out, size_t cap) {
    char base[PATH_MAX];
    const char *rest = name;
    if (name[0] && name[0] != ':' && strchr(name, ':')) {
        // Absolute "Volume:dir:file"
        const char *colon = strchr(name, ':');
        char vol[256], boot[256];
        snprintf(vol, sizeof vol, "%.*s", (int)(colon - name), name);
        boot_volume_name(boot, sizeof boot);
        if (strcmp(vol, boot) == 0) strlcpy(base, "/", sizeof base);
        else snprintf(base, sizeof base, "/Volumes/%s", vol);
        rest = colon + 1;
    } else {
        if (!dir_path(vref, dirID, base, sizeof base)) return dirNFErr;
        if (*rest == ':') rest++;
    }
    strlcpy(out, base, cap);
    // Walk components; an empty component ("::") means parent directory.
    while (*rest) {
        const char *c = strchr(rest, ':');
        size_t n = c ? (size_t)(c - rest) : strlen(rest);
        if (n == 0) {
            char tmp[PATH_MAX];
            strlcpy(tmp, out, sizeof tmp);
            strlcpy(out, dirname(tmp), cap);
        } else {
            char comp[256];
            snprintf(comp, sizeof comp, "%.*s", (int)n, rest);
            for (char *p = comp; *p; p++)
                if (*p == '/') *p = ':';
            size_t l = strlen(out);
            if (l && out[l - 1] != '/') strlcat(out, "/", cap);
            strlcat(out, comp, cap);
        }
        if (!c) break;
        rest = c + 1;
    }
    return noErr;
}

static bool spec_path(gaddr_t spec, char *out, size_t cap) {
    int16_t vref = (int16_t)gread16(spec);
    int32_t par = (int32_t)gread32(spec + 2);
    char dir[PATH_MAX], name[256];
    if (!dir_path(vref, par, dir, sizeof dir)) return false;
    pstr_to_utf8(spec + 6, name, sizeof name);
    for (char *p = name; *p; p++)
        if (*p == ':') *p = '/'; // (already host-style after conversion)
    snprintf(out, cap, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
    return true;
}

// FSMakeFSSpec(vRefNum, dirID, ConstStr255Param fileName, FSSpec *spec)
THUNK(FSMakeFSSpec) {
    char name[512], path[PATH_MAX];
    pstr_to_utf8(ARG(2), name, sizeof name);
    OSErr e = hfs_to_path((int16_t)ARG(0), ARGI(1), name, path, sizeof path);
    if (e != noErr) FAIL(e);
    size_t l = strlen(path);
    while (l > 1 && path[l - 1] == '/') path[--l] = 0;
    char d[PATH_MAX], b[PATH_MAX];
    strlcpy(d, path, sizeof d);
    strlcpy(b, path, sizeof b);
    const char *parent = dirname(d), *leaf = basename(b);
    struct stat st;
    if (stat(parent, &st) != 0) FAIL(dirNFErr);
    gaddr_t spec = ARG(3);
    uint8_t pname[64];
    utf8_to_pstr(leaf, pname, 63);
    gwrite16(spec, (uint16_t)FAKE_VREFNUM);
    gwrite32(spec + 2, (uint32_t)dir_id_for(parent));
    memset(g2h(spec + 6), 0, 64);
    memcpy(g2h(spec + 6), pname, pname[0] + 1u);
    GRET_ERR(stat(path, &st) == 0 ? noErr : fnfErr);
}

static OSErr errno_to_oserr(int e) {
    switch (e) {
    case ENOENT: return fnfErr;
    case EEXIST: return dupFNErr;
    case EACCES:
    case EPERM: return permErr;
    case ENOTDIR: return dirNFErr;
    case ENOSPC: return dskFulErr;
    case EROFS: return wPrErr;
    case ENOTEMPTY: return fBsyErr;
    default: return ioErr;
    }
}

static OSErr set_type_creator(const char *path, uint32_t type, uint32_t creator) {
    FSRef ref;
    if (FSPathMakeRef((const UInt8 *)path, &ref, NULL) != noErr) return fnfErr;
    FSCatalogInfo ci;
    OSErr e = FSGetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci, NULL, NULL, NULL);
    if (e != noErr) return e;
    FileInfo *fi = (FileInfo *)ci.finderInfo;
    fi->fileType = type;
    fi->fileCreator = creator;
    return FSSetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci);
}

// FSpCreate(spec, OSType creator, OSType fileType, ScriptCode)
THUNK(FSpCreate) {
    char path[PATH_MAX];
    if (!spec_path(ARG(0), path, sizeof path)) FAIL(dirNFErr);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) FAIL(errno_to_oserr(errno));
    close(fd);
    if (ARG(1) || ARG(2)) set_type_creator(path, ARG(2), ARG(1));
    GRET_ERR(noErr);
}

THUNK(FSpDelete) {
    char path[PATH_MAX];
    struct stat st;
    if (!spec_path(ARG(0), path, sizeof path)) FAIL(dirNFErr);
    if (lstat(path, &st) != 0) FAIL(fnfErr);
    int rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    GRET_ERR(rc == 0 ? noErr : errno_to_oserr(errno));
}

// FInfo (16 bytes, same on both): fdType, fdCreator, fdFlags, fdLocation, fdFldr
THUNK(FSpGetFInfo) {
    char path[PATH_MAX];
    FSRef ref;
    FSCatalogInfo ci;
    if (!spec_path(ARG(0), path, sizeof path)) {
        GRET_ERR(dirNFErr);
        return;
    }
    if (FSPathMakeRef((const UInt8 *)path, &ref, NULL) != noErr) {
        GRET_ERR(fnfErr);
        return;
    }
    OSErr e = FSGetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci, NULL, NULL, NULL);
    if (e == noErr && ARG(1)) memcpy(g2h(ARG(1)), ci.finderInfo, 16);
    GRET_ERR(e);
}

THUNK(FSpSetFInfo) {
    char path[PATH_MAX];
    FSRef ref;
    FSCatalogInfo ci;
    if (!spec_path(ARG(0), path, sizeof path) || FSPathMakeRef((const UInt8 *)path, &ref, NULL) != noErr) {
        GRET_ERR(fnfErr);
        return;
    }
    memset(&ci, 0, sizeof ci);
    memcpy(ci.finderInfo, g2h(ARG(1)), 16);
    GRET_ERR(FSSetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci));
}

// Emulated refnums for FSpOpenDF; other refnums are host fork refnums.
#define EMU_REF_BASE 30000
#define EMU_REF_MAX 512
static int emu_fds[EMU_REF_MAX];
static pthread_mutex_t emu_lock = PTHREAD_MUTEX_INITIALIZER;

static int emu_fd(int16_t ref) {
    int i = ref - EMU_REF_BASE;
    return i >= 0 && i < EMU_REF_MAX ? emu_fds[i] - 1 : -1;
}

// FSpOpenDF(spec, SInt8 permission, FSIORefNum *refNum)
THUNK(FSpOpenDF) {
    char path[PATH_MAX];
    if (!spec_path(ARG(0), path, sizeof path)) {
        GRET_ERR(dirNFErr);
        return;
    }
    int perm = (int8_t)ARG(1), fd;
    switch (perm) {
    case fsRdPerm: fd = open(path, O_RDONLY); break;
    case fsWrPerm: fd = open(path, O_WRONLY); break;
    case fsRdWrPerm:
    case fsRdWrShPerm: fd = open(path, O_RDWR); break;
    default:
        fd = open(path, O_RDWR);
        if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) fd = open(path, O_RDONLY);
        break;
    }
    if (fd < 0) {
        GRET_ERR(errno_to_oserr(errno));
        return;
    }
    pthread_mutex_lock(&emu_lock);
    int i = 0;
    while (i < EMU_REF_MAX && emu_fds[i]) i++;
    if (i < EMU_REF_MAX) emu_fds[i] = fd + 1;
    pthread_mutex_unlock(&emu_lock);
    if (i == EMU_REF_MAX) {
        close(fd);
        GRET_ERR(tmfoErr);
        return;
    }
    if (ARG(2)) gwrite16(ARG(2), (uint16_t)(EMU_REF_BASE + i));
    GRET_ERR(noErr);
}

THUNK(FSClose) {
    int16_t ref = (int16_t)ARG(0);
    int fd = emu_fd(ref);
    if (fd >= 0) {
        close(fd);
        emu_fds[ref - EMU_REF_BASE] = 0;
        GRET_ERR(noErr);
    } else
        GRET_ERR(FSCloseFork(ref));
}

// FSRead(refNum, long *count, void *buffer) / FSWrite
static void fs_rw(struct guest_regs *r, bool write) {
    int16_t ref = (int16_t)ARG(0);
    int32_t want = (int32_t)gread32(ARG(1));
    int fd = emu_fd(ref);
    if (want < 0) {
        GRET_ERR(paramErr);
        return;
    }
    OSErr e = noErr;
    uint32_t done = 0;
    if (fd >= 0) {
        ssize_t n = write ? pwrite(fd, ARGP(2), (size_t)want, lseek(fd, 0, SEEK_CUR))
                          : read(fd, ARGP(2), (size_t)want);
        if (write && n > 0) lseek(fd, n, SEEK_CUR);
        if (n < 0) e = errno_to_oserr(errno), n = 0;
        done = (uint32_t)n;
        if (!write && done < (uint32_t)want && e == noErr) e = eofErr;
    } else {
        ByteCount actual = 0;
        e = write ? FSWriteFork(ref, fsAtMark, 0, (ByteCount)want, ARGP(2), &actual)
                  : FSReadFork(ref, fsAtMark, 0, (ByteCount)want, ARGP(2), &actual);
        done = (uint32_t)actual;
    }
    gwrite32(ARG(1), done);
    GRET_ERR(e);
}
THUNK(FSRead) { fs_rw(r, false); }
THUNK(FSWrite) { fs_rw(r, true); }

// GetEOF(refNum, long *logEOF)
THUNK(GetEOF) {
    int16_t ref = (int16_t)ARG(0);
    int fd = emu_fd(ref);
    OSErr e = noErr;
    int64_t size = 0;
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) size = st.st_size;
        else e = errno_to_oserr(errno);
    } else {
        SInt64 s = 0;
        e = FSGetForkSize(ref, &s);
        size = s;
    }
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)(size > 0x7FFFFFFF ? 0x7FFFFFFF : size));
    GRET_ERR(e);
}

THUNK(SetEOF) {
    int16_t ref = (int16_t)ARG(0);
    int fd = emu_fd(ref);
    if (fd >= 0) GRET_ERR(ftruncate(fd, ARGI(1)) == 0 ? noErr : errno_to_oserr(errno));
    else GRET_ERR(FSSetForkSize(ref, fsFromStart, ARGI(1)));
}

// SetFPos(refNum, short posMode, long posOff)
THUNK(SetFPos) {
    int16_t ref = (int16_t)ARG(0);
    int mode = (int16_t)ARG(1);
    int32_t off = ARGI(2);
    int fd = emu_fd(ref);
    if (fd < 0) {
        GRET_ERR(FSSetForkPosition(ref, (UInt16)mode, off));
        return;
    }
    int whence = SEEK_SET;
    switch (mode & 3) {
    case fsAtMark: GRET_ERR(noErr); return;
    case fsFromStart: whence = SEEK_SET; break;
    case fsFromLEOF: whence = SEEK_END; break;
    case fsFromMark: whence = SEEK_CUR; break;
    }
    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t pos = lseek(fd, off, whence);
    if (pos < 0) {
        lseek(fd, cur, SEEK_SET);
        GRET_ERR(posErr);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && pos > st.st_size) {
        lseek(fd, st.st_size, SEEK_SET);
        GRET_ERR(eofErr);
        return;
    }
    GRET_ERR(noErr);
}

THUNK(GetFPos) {
    int16_t ref = (int16_t)ARG(0);
    int fd = emu_fd(ref);
    int64_t pos = 0;
    OSErr e = noErr;
    if (fd >= 0) pos = lseek(fd, 0, SEEK_CUR);
    else {
        SInt64 p = 0;
        e = FSGetForkPosition(ref, &p);
        pos = p;
    }
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)pos);
    GRET_ERR(e);
}

// --- Resource Manager ------------------------------------------------------------------------
// FSOpenResourceFile(ref, forkNameLength, forkName, SInt8 permissions, ResFileRefNum *refNum)
THUNK(FSOpenResourceFile) {
    ResFileRefNum ref = -1;
    OSErr e = FSOpenResourceFile((const FSRef *)ARGP(0), ARG(1), (const UniChar *)ARGP(2), (SInt8)ARG(3), &ref);
    if (ARG(4)) gwrite16(ARG(4), (uint16_t)(e == noErr ? ref : -1));
    GRET_ERR(e);
}

THUNK(CloseResFile) { CloseResFile((int16_t)ARG(0)); }
THUNK(CurResFile) { RET((uint32_t)(int32_t)(int16_t)CurResFile()); }
THUNK(UseResFile) { UseResFile((int16_t)ARG(0)); }
THUNK(ResError) { RET((uint32_t)(int32_t)ResError()); }

// Resources reach the guest as guest Handles holding a copy of the data; the
// host handle is remembered for ReleaseResource.
static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef res_map; // guest handle -> host Handle

static uint32_t resource_to_guest(Handle h) {
    if (!h) return 0;
    LoadResource(h);
    Size sz = GetHandleSize(h);
    gaddr_t g = guest_handle_new(*h, (uint32_t)sz);
    pthread_mutex_lock(&res_lock);
    if (!res_map) res_map = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    CFDictionarySetValue(res_map, (void *)(uintptr_t)g, h);
    pthread_mutex_unlock(&res_lock);
    return g;
}

THUNK(GetIndResource) { RET(resource_to_guest(GetIndResource(ARG(0), (ResourceIndex)ARG(1)))); }
THUNK(Get1IndResource) { RET(resource_to_guest(Get1IndResource(ARG(0), (ResourceIndex)ARG(1)))); }
THUNK(GetResource) { RET(resource_to_guest(GetResource(ARG(0), (ResID)ARG(1)))); }
THUNK(Get1Resource) { RET(resource_to_guest(Get1Resource(ARG(0), (ResID)ARG(1)))); }
THUNK(CountResources) { RET((uint32_t)(int32_t)CountResources(ARG(0))); }

THUNK(ReleaseResource) {
    gaddr_t g = ARG(0);
    Handle h = NULL;
    pthread_mutex_lock(&res_lock);
    if (res_map) {
        h = (Handle)CFDictionaryGetValue(res_map, (void *)(uintptr_t)g);
        if (h) CFDictionaryRemoveValue(res_map, (void *)(uintptr_t)g);
    }
    pthread_mutex_unlock(&res_lock);
    if (h) {
        ReleaseResource(h);
        guest_handle_free(g);
    }
}
