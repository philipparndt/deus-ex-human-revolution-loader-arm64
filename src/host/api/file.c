// File descriptors, file metadata, directories, memory mappings and POSIX aio.
//
// Layouts (Darwin i386, verified against the SDK headers with offsetof):
//   struct stat (legacy, 32-bit inode)      96 bytes
//   struct stat64 / stat$INODE64           108 bytes
//   struct dirent (legacy)                 264 bytes  {u32 ino; u16 reclen; u8 type; u8 namlen; char name[256]}
//   struct dirent$INODE64                 1048 bytes  (same layout as the host's)
//   struct iovec                             8 bytes  {ptr32, len32}
//   struct aiocb                            48 bytes  fildes@0 offset@4 buf@12 nbytes@16 reqprio@20
//                                                     sigevent@24 (20 bytes) lio_opcode@44
//   struct flock, fstore_t, struct log2phys and struct pollfd are layout-identical.
#include "common.h"
#include "cpu.h"
#include "libc.h"

#include <aio.h>
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#define GPAGE_GUEST 4096u // page size the guest believes in (getpagesize)

// --- basic fd I/O ---------------------------------------------------------------------
THUNK(open) { RET_ERRNO(open(ARGS(0), ARGI(1), (int)ARG(2))); }
THUNK_ALIAS(open_unix2003, "_open$UNIX2003", open)
THUNK_ALIAS(open_nocancel, "_open$NOCANCEL$UNIX2003", open)
THUNK(creat) { RET_ERRNO(creat(ARGS(0), (mode_t)ARG(1))); }
THUNK_ALIAS(creat_unix2003, "_creat$UNIX2003", creat)
THUNK(close) { RET_ERRNO(close(ARGI(0))); }
THUNK_ALIAS(close_unix2003, "_close$UNIX2003", close)
THUNK_ALIAS(close_nocancel, "_close$NOCANCEL$UNIX2003", close)
THUNK(read) { RET_ERRNO(read(ARGI(0), ARGP(1), ARG(2))); }
THUNK_ALIAS(read_unix2003, "_read$UNIX2003", read)
THUNK_ALIAS(read_nocancel, "_read$NOCANCEL$UNIX2003", read)
THUNK(write) { RET_ERRNO(write(ARGI(0), ARGP(1), ARG(2))); }
THUNK_ALIAS(write_unix2003, "_write$UNIX2003", write)
THUNK_ALIAS(write_nocancel, "_write$NOCANCEL$UNIX2003", write)
// pread/pwrite(fd, buf, count, off_t offset): offset takes slots 3-4.
THUNK(pread) { RET_ERRNO(pread(ARGI(0), ARGP(1), ARG(2), (off_t)ARG64(3))); }
THUNK_ALIAS(pread_unix2003, "_pread$UNIX2003", pread)
THUNK_ALIAS(pread_nocancel, "_pread$NOCANCEL$UNIX2003", pread)
THUNK(pwrite) { RET_ERRNO(pwrite(ARGI(0), ARGP(1), ARG(2), (off_t)ARG64(3))); }
THUNK_ALIAS(pwrite_unix2003, "_pwrite$UNIX2003", pwrite)
THUNK_ALIAS(pwrite_nocancel, "_pwrite$NOCANCEL$UNIX2003", pwrite)
THUNK(lseek) {
    off_t o = lseek(ARGI(0), (off_t)ARG64(1), ARGI(3));
    if (o == -1) set_gerrno(errno);
    RET64(o);
}

static int get_iov(gaddr_t giov, int cnt, struct iovec *iov) {
    if (cnt < 0 || cnt > IOV_MAX) return -1;
    for (int i = 0; i < cnt; i++) {
        iov[i].iov_base = g2h(gread32(giov + 8 * (uint32_t)i));
        iov[i].iov_len = gread32(giov + 8 * (uint32_t)i + 4);
    }
    return 0;
}
THUNK(writev) {
    struct iovec iov[IOV_MAX];
    if (get_iov(ARG(1), ARGI(2), iov)) {
        set_gerrno(EINVAL);
        RET(-1);
        return;
    }
    RET_ERRNO(writev(ARGI(0), iov, ARGI(2)));
}
THUNK_ALIAS(writev_unix2003, "_writev$UNIX2003", writev)
THUNK_ALIAS(writev_nocancel, "_writev$NOCANCEL$UNIX2003", writev)
THUNK(readv) {
    struct iovec iov[IOV_MAX];
    if (get_iov(ARG(1), ARGI(2), iov)) {
        set_gerrno(EINVAL);
        RET(-1);
        return;
    }
    RET_ERRNO(readv(ARGI(0), iov, ARGI(2)));
}
THUNK_ALIAS(readv_unix2003, "_readv$UNIX2003", readv)
THUNK_ALIAS(readv_nocancel, "_readv$NOCANCEL$UNIX2003", readv)

THUNK(fcntl) {
    int fd = ARGI(0), cmd = ARGI(1);
    uint32_t a = ARG(2);
    int rc;
    switch (cmd) {
    case F_GETLK: case F_SETLK: case F_SETLKW: // struct flock: identical layout
    case F_PREALLOCATE:                         // fstore_t: identical
    case F_LOG2PHYS: case F_LOG2PHYS_EXT:       // struct log2phys: identical
    case F_GETPATH:                             // char[MAXPATHLEN]
    case F_SETSIZE:                             // off_t *
        rc = fcntl(fd, cmd, g2h(a));
        break;
    case F_RDADVISE: { // struct radvisory {off_t; int}: 12 bytes in i386, 16 on the host
        struct radvisory ra = {.ra_offset = (off_t)gread64(a), .ra_count = (int)gread32(a + 8)};
        rc = fcntl(fd, cmd, &ra);
        break;
    }
    default:
        rc = fcntl(fd, cmd, (int)a);
        break;
    }
    if (rc == -1) set_gerrno(errno);
    RET(rc);
}
THUNK_ALIAS(fcntl_unix2003, "_fcntl$UNIX2003", fcntl)
THUNK_ALIAS(fcntl_nocancel, "_fcntl$NOCANCEL$UNIX2003", fcntl)

// ioctl(fd, unsigned long request, ...): requests that carry a parameter pass a pointer.
THUNK(ioctl) {
    uint32_t req = ARG(1);
    int rc;
    if ((req & (IOC_IN | IOC_OUT)) && IOCPARM_LEN(req)) rc = ioctl(ARGI(0), (unsigned long)req, ARGP(2));
    else rc = ioctl(ARGI(0), (unsigned long)req, (int)ARG(2));
    if (rc == -1) set_gerrno(errno);
    RET(rc);
}

THUNK(poll) { RET_ERRNO(poll(ARGP(0), ARG(1), ARGI(2))); }
THUNK_ALIAS(poll_unix2003, "_poll$UNIX2003", poll)
THUNK_ALIAS(poll_nocancel, "_poll$NOCANCEL$UNIX2003", poll)

THUNK(fsync) { RET_ERRNO(fsync(ARGI(0))); }
THUNK_ALIAS(fsync_unix2003, "_fsync$UNIX2003", fsync)
THUNK_ALIAS(fsync_nocancel, "_fsync$NOCANCEL$UNIX2003", fsync)
THUNK(ftruncate) { RET_ERRNO(ftruncate(ARGI(0), (off_t)ARG64(1))); }
THUNK(truncate) { RET_ERRNO(truncate(ARGS(0), (off_t)ARG64(1))); }
THUNK(dup) { RET_ERRNO(dup(ARGI(0))); }
THUNK(dup2) { RET_ERRNO(dup2(ARGI(0), ARGI(1))); }
THUNK(pipe) { RET_ERRNO(pipe(ARGP(0))); }
THUNK(isatty) { RET(isatty(ARGI(0))); }
THUNK(flock) { RET_ERRNO(flock(ARGI(0), ARGI(1))); }

// --- paths --------------------------------------------------------------------------------
THUNK(access) { RET_ERRNO(access(ARGS(0), ARGI(1))); }
THUNK(chmod) { RET_ERRNO(chmod(ARGS(0), (mode_t)ARG(1))); }
THUNK_ALIAS(chmod_unix2003, "_chmod$UNIX2003", chmod)
THUNK(fchmod) { RET_ERRNO(fchmod(ARGI(0), (mode_t)ARG(1))); }
THUNK_ALIAS(fchmod_unix2003, "_fchmod$UNIX2003", fchmod)
THUNK(chown) { RET_ERRNO(chown(ARGS(0), ARG(1), ARG(2))); }
THUNK(fchown) { RET_ERRNO(fchown(ARGI(0), ARG(1), ARG(2))); }
THUNK(lchown) { RET_ERRNO(lchown(ARGS(0), ARG(1), ARG(2))); }
THUNK_ALIAS(lchown_unix2003, "_lchown$UNIX2003", lchown)
THUNK(mkdir) { RET_ERRNO(mkdir(ARGS(0), (mode_t)ARG(1))); }
THUNK(rmdir) { RET_ERRNO(rmdir(ARGS(0))); }
THUNK(unlink) { RET_ERRNO(unlink(ARGS(0))); }
THUNK(link) { RET_ERRNO(link(ARGS(0), ARGS(1))); }
THUNK(symlink) { RET_ERRNO(symlink(ARGS(0), ARGS(1))); }
THUNK(readlink) { RET_ERRNO(readlink(ARGS(0), ARGP(1), ARG(2))); }
THUNK(chdir) { RET_ERRNO(chdir(ARGS(0))); }
THUNK(fchdir) { RET_ERRNO(fchdir(ARGI(0))); }
THUNK(umask) { RET(umask((mode_t)ARG(0))); }
THUNK(mkfifo) { RET_ERRNO(mkfifo(ARGS(0), (mode_t)ARG(1))); }

static int get_tv2(gaddr_t g, struct timeval tv[2]) {
    if (!g) return 0;
    get_gtimeval(g, &tv[0]);
    get_gtimeval(g + 8, &tv[1]);
    return 1;
}
THUNK(utimes) {
    struct timeval tv[2];
    RET_ERRNO(utimes(ARGS(0), get_tv2(ARG(1), tv) ? tv : NULL));
}
THUNK(lutimes) {
    struct timeval tv[2];
    RET_ERRNO(lutimes(ARGS(0), get_tv2(ARG(1), tv) ? tv : NULL));
}
THUNK(futimes) {
    struct timeval tv[2];
    RET_ERRNO(futimes(ARGI(0), get_tv2(ARG(1), tv) ? tv : NULL));
}
// struct utimbuf {time_t actime, modtime}: two 32-bit fields in i386.
THUNK(utime) {
    struct timeval tv[2] = {{0, 0}, {0, 0}};
    if (ARG(1)) {
        tv[0].tv_sec = (int32_t)gread32(ARG(1));
        tv[1].tv_sec = (int32_t)gread32(ARG(1) + 4);
    }
    RET_ERRNO(utimes(ARGS(0), ARG(1) ? tv : NULL));
}

// copyfile(from, to, copyfile_state_t state, flags): state objects are not supported.
THUNK(copyfile) {
    if (ARG(2)) LOGI("copyfile: ignoring copyfile_state_t %#x", ARG(2));
    RET_ERRNO(copyfile(ARGS(0), ARGS(1), NULL, ARG(3)));
}

// The template is guest memory, so the host writes the generated name in place.
THUNK(mkstemp) { RET_ERRNO(mkstemp(ARGP(0))); }
THUNK(mkstemps) { RET_ERRNO(mkstemps(ARGP(0), ARGI(1))); }
THUNK(mkdtemp) {
    char *p = mkdtemp(ARGP(0));
    if (!p) set_gerrno(errno);
    RET(p ? ARG(0) : 0);
}
THUNK(mktemp) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    char *p = mktemp(ARGP(0));
#pragma clang diagnostic pop
    RET(p ? ARG(0) : 0);
}

THUNK(getcwd) {
    if (!ARG(0)) {
        char *s = getcwd(NULL, 0);
        if (!s) {
            set_gerrno(errno);
            RET(0);
            return;
        }
        if (ARG(1) && strlen(s) >= ARG(1)) {
            free(s);
            set_gerrno(ERANGE);
            RET(0);
            return;
        }
        gaddr_t g = gheap_strdup(s);
        free(s);
        RET(g);
    }
    char *s = getcwd(ARGP(0), ARG(1));
    if (!s) set_gerrno(errno);
    RET(s ? ARG(0) : 0);
}

THUNK(realpath) {
    char buf[PATH_MAX];
    char *s = realpath(ARGS(0), buf);
    if (!s) {
        set_gerrno(errno);
        // Like the host: the resolved buffer holds the partial path on error.
        if (ARG(1)) gstrlcpy(ARG(1), buf, PATH_MAX);
        RET(0);
        return;
    }
    if (ARG(1)) {
        gstrlcpy(ARG(1), buf, PATH_MAX);
        RET(ARG(1));
    } else
        RET(gheap_strdup(buf));
}
THUNK_ALIAS(realpath_extsn, "_realpath$DARWIN_EXTSN", realpath)

// --- stat -----------------------------------------------------------------------------
static void put_stat_legacy(gaddr_t g, const struct stat *st) {
    memset(g2h(g), 0, 96);
    gwrite32(g + 0, (uint32_t)st->st_dev);
    gwrite32(g + 4, (uint32_t)st->st_ino);
    gwrite16(g + 8, st->st_mode);
    gwrite16(g + 10, st->st_nlink);
    gwrite32(g + 12, st->st_uid);
    gwrite32(g + 16, st->st_gid);
    gwrite32(g + 20, (uint32_t)st->st_rdev);
    put_gtimespec(g + 24, &st->st_atimespec);
    put_gtimespec(g + 32, &st->st_mtimespec);
    put_gtimespec(g + 40, &st->st_ctimespec);
    gwrite64(g + 48, (uint64_t)st->st_size);
    gwrite64(g + 56, (uint64_t)st->st_blocks);
    gwrite32(g + 64, (uint32_t)st->st_blksize);
    gwrite32(g + 68, st->st_flags);
    gwrite32(g + 72, st->st_gen);
}
static void put_stat64(gaddr_t g, const struct stat *st) {
    memset(g2h(g), 0, 108);
    gwrite32(g + 0, (uint32_t)st->st_dev);
    gwrite16(g + 4, st->st_mode);
    gwrite16(g + 6, st->st_nlink);
    gwrite64(g + 8, st->st_ino);
    gwrite32(g + 16, st->st_uid);
    gwrite32(g + 20, st->st_gid);
    gwrite32(g + 24, (uint32_t)st->st_rdev);
    put_gtimespec(g + 28, &st->st_atimespec);
    put_gtimespec(g + 36, &st->st_mtimespec);
    put_gtimespec(g + 44, &st->st_ctimespec);
    put_gtimespec(g + 52, &st->st_birthtimespec);
    gwrite64(g + 60, (uint64_t)st->st_size);
    gwrite64(g + 68, (uint64_t)st->st_blocks);
    gwrite32(g + 76, (uint32_t)st->st_blksize);
    gwrite32(g + 80, st->st_flags);
    gwrite32(g + 84, st->st_gen);
}

#define STAT_THUNKS(id, call, putfn)                                                               \
    THUNK_AS(id, "_" #id) {                                                                        \
        struct stat st;                                                                            \
        int rc = call;                                                                             \
        if (rc == 0) putfn(ARG(1), &st);                                                           \
        else set_gerrno(errno);                                                                    \
        RET(rc);                                                                                   \
    }
STAT_THUNKS(stat, stat(ARGS(0), &st), put_stat_legacy)
STAT_THUNKS(lstat, lstat(ARGS(0), &st), put_stat_legacy)
STAT_THUNKS(fstat, fstat(ARGI(0), &st), put_stat_legacy)
STAT_THUNKS(stat64, stat(ARGS(0), &st), put_stat64)
STAT_THUNKS(lstat64, lstat(ARGS(0), &st), put_stat64)
STAT_THUNKS(fstat64, fstat(ARGI(0), &st), put_stat64)
THUNK_ALIAS(stat_inode64, "_stat$INODE64", stat64)
THUNK_ALIAS(lstat_inode64, "_lstat$INODE64", lstat64)
THUNK_ALIAS(fstat_inode64, "_fstat$INODE64", fstat64)

// --- directories --------------------------------------------------------------------------
// The guest DIR* is a host object handle. Its first word (where the i386 DIR
// keeps __dd_fd, read by the 10.8 dirfd() macro) holds the descriptor.
struct gdir {
    DIR *d;
    gaddr_t ent, ent64; // per-DIR guest dirent buffers
};

static gaddr_t gdir_new(DIR *d) {
    if (!d) {
        set_gerrno(errno);
        return 0;
    }
    struct gdir *gd = calloc(1, sizeof *gd);
    gd->d = d;
    gaddr_t h = hobj_wrap(gd);
    hobj_set_isa(h, (gaddr_t)dirfd(d));
    return h;
}
static struct gdir *gdir_get(gaddr_t h) {
    struct gdir *gd = hobj_unwrap(h);
    if (!gd) LOGE("dir: %#x is not a DIR handle", h);
    return gd;
}

THUNK(opendir) { RET(gdir_new(opendir(ARGS(0)))); }
THUNK_ALIAS(opendir_unix2003, "_opendir$UNIX2003", opendir)
THUNK_ALIAS(opendir_inode64, "_opendir$INODE64", opendir)
THUNK_ALIAS(opendir_inode64_unix2003, "_opendir$INODE64$UNIX2003", opendir)
THUNK(fdopendir) { RET(gdir_new(fdopendir(ARGI(0)))); }
THUNK_ALIAS(fdopendir_inode64, "_fdopendir$INODE64", fdopendir)
THUNK_ALIAS(fdopendir_inode64_unix2003, "_fdopendir$INODE64$UNIX2003", fdopendir)

static void put_dirent_legacy(gaddr_t g, const struct dirent *e) {
    uint32_t nl = e->d_namlen > 255 ? 255 : e->d_namlen;
    gwrite32(g, (uint32_t)e->d_ino);
    gwrite16(g + 4, (uint16_t)((8 + nl + 1 + 3) & ~3u));
    gwrite8(g + 6, e->d_type);
    gwrite8(g + 7, (uint8_t)nl);
    memcpy(g2h(g + 8), e->d_name, nl);
    gwrite8(g + 8 + nl, 0);
}
static void put_dirent64(gaddr_t g, const struct dirent *e) {
    // Host and guest 64-bit-inode dirents have the same layout.
    memcpy(g2h(g), e, offsetof(struct dirent, d_name) + e->d_namlen + 1);
}

static gaddr_t do_readdir(gaddr_t h, bool ino64) {
    struct gdir *gd = gdir_get(h);
    if (!gd) {
        set_gerrno(EBADF);
        return 0;
    }
    errno = 0;
    struct dirent *e = readdir(gd->d);
    if (!e) {
        if (errno) set_gerrno(errno);
        return 0;
    }
    if (ino64) {
        if (!gd->ent64) gd->ent64 = gheap_calloc(1, sizeof(struct dirent));
        put_dirent64(gd->ent64, e);
        return gd->ent64;
    }
    if (!gd->ent) gd->ent = gheap_calloc(1, 264);
    put_dirent_legacy(gd->ent, e);
    return gd->ent;
}
THUNK(readdir) { RET(do_readdir(ARG(0), false)); }
THUNK_AS(readdir_inode64, "_readdir$INODE64") { RET(do_readdir(ARG(0), true)); }

// readdir_r(dirp, entry, result*)
static int do_readdir_r(struct guest_regs *r, bool ino64) {
    struct gdir *gd = gdir_get(ARG(0));
    if (!gd) return EBADF;
    errno = 0;
    struct dirent *e = readdir(gd->d);
    if (!e) {
        gwrite32(ARG(2), 0);
        return errno;
    }
    if (ino64) put_dirent64(ARG(1), e);
    else put_dirent_legacy(ARG(1), e);
    gwrite32(ARG(2), ARG(1));
    return 0;
}
THUNK(readdir_r) { RET(do_readdir_r(r, false)); }
THUNK_AS(readdir_r_inode64, "_readdir_r$INODE64") { RET(do_readdir_r(r, true)); }

THUNK(closedir) {
    struct gdir *gd = gdir_get(ARG(0));
    if (!gd) {
        set_gerrno(EBADF);
        RET(-1);
        return;
    }
    int rc = closedir(gd->d);
    if (rc) set_gerrno(errno);
    gheap_free(gd->ent);
    gheap_free(gd->ent64);
    hobj_forget(ARG(0));
    free(gd);
    RET(rc);
}
THUNK_ALIAS(closedir_unix2003, "_closedir$UNIX2003", closedir)
THUNK(rewinddir) {
    struct gdir *gd = gdir_get(ARG(0));
    if (gd) rewinddir(gd->d);
}
THUNK_ALIAS(rewinddir_inode64, "_rewinddir$INODE64", rewinddir)
THUNK_ALIAS(rewinddir_inode64_unix2003, "_rewinddir$INODE64$UNIX2003", rewinddir)
THUNK_ALIAS(rewinddir_unix2003, "_rewinddir$UNIX2003", rewinddir)
THUNK(telldir) {
    struct gdir *gd = gdir_get(ARG(0));
    RET(gd ? (uint32_t)telldir(gd->d) : (uint32_t)-1);
}
THUNK_ALIAS(telldir_inode64, "_telldir$INODE64", telldir)
THUNK_ALIAS(telldir_inode64_unix2003, "_telldir$INODE64$UNIX2003", telldir)
THUNK_ALIAS(telldir_unix2003, "_telldir$UNIX2003", telldir)
THUNK(seekdir) {
    struct gdir *gd = gdir_get(ARG(0));
    if (gd) seekdir(gd->d, ARGI(1));
}
THUNK_ALIAS(seekdir_inode64, "_seekdir$INODE64", seekdir)
THUNK_ALIAS(seekdir_inode64_unix2003, "_seekdir$INODE64$UNIX2003", seekdir)
THUNK_ALIAS(seekdir_unix2003, "_seekdir$UNIX2003", seekdir)
THUNK(dirfd) {
    struct gdir *gd = gdir_get(ARG(0));
    RET(gd ? dirfd(gd->d) : -1);
}

// --- memory mappings ------------------------------------------------------------------------
// Mappings live in guest memory managed by gmem (16 KB pages). File mappings
// whose offset and address are host-page aligned are real host file mappings
// placed over the guest pages; otherwise the file content is read in (and, for
// writable MAP_SHARED mappings, written back on msync/munmap).
struct gmapping {
    gaddr_t start;
    uint32_t len;   // requested length rounded to guest (4 KB) pages
    uint32_t alloc; // length rounded to gmem pages
    int fd;         // dup'd descriptor for write-back, or -1
    off_t off;
};
static struct gmapping *maps;
static size_t nmaps, maps_cap;
static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAP_FAILED_G 0xffffffffu

static uint64_t round_up(uint64_t v, uint32_t a) { return (v + a - 1) & ~(uint64_t)(a - 1); }

static void maps_add(struct gmapping m) {
    if (nmaps == maps_cap) {
        maps_cap = maps_cap ? maps_cap * 2 : 64;
        maps = realloc(maps, maps_cap * sizeof *maps);
    }
    maps[nmaps++] = m;
}

static int host_mprot(int prot) {
    int p = prot & (PROT_READ | PROT_WRITE);
    if ((prot & PROT_EXEC) && CPU_EXECUTES_GUEST_PAGES) p |= PROT_EXEC;
    return p;
}

// Apply prot to the gmem pages fully inside [addr, addr+len); pages only
// partially covered are shared with other data and only ever gain access.
static void protect_range(gaddr_t addr, uint32_t len, int prot) {
    uint64_t end = (uint64_t)addr + len;
    uint64_t fs = round_up(addr, GPAGE_SIZE), fe = end & ~(uint64_t)(GPAGE_SIZE - 1);
    if (fe > fs) gmem_protect((gaddr_t)fs, (uint32_t)(fe - fs), prot);
    int grow = prot | GPROT_RW;
    if (addr < fs) gmem_protect(addr & ~(GPAGE_SIZE - 1), GPAGE_SIZE, grow);
    if (end > fe && fe >= fs && (end & (GPAGE_SIZE - 1))) gmem_protect((gaddr_t)fe, GPAGE_SIZE, grow);
}

static void writeback(const struct gmapping *m, gaddr_t a, gaddr_t b) {
    if (m->fd < 0) return;
    if (a < m->start) a = m->start;
    if (b > m->start + m->len) b = m->start + m->len;
    if (b <= a) return;
    struct stat st;
    if (fstat(m->fd, &st)) return;
    off_t foff = m->off + (a - m->start);
    if (foff >= st.st_size) return;
    uint64_t n = b - a;
    if ((uint64_t)(st.st_size - foff) < n) n = (uint64_t)(st.st_size - foff);
    if (pwrite(m->fd, g2h(a), n, foff) < 0) LOGE("mmap: write-back failed: %s", strerror(errno));
}

static bool fill_from_file(gaddr_t start, uint32_t len, int fd, off_t off) {
    uint8_t *p = g2h(start);
    uint32_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, off + done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;
        done += (uint32_t)n;
    }
    memset(p + done, 0, len - done);
    return true;
}

// Claim [addr, addr+len) for a MAP_FIXED mapping: map free gmem pages, make
// already mapped ones writable.
static bool claim_fixed(gaddr_t addr, uint32_t len) {
    uint64_t end = (uint64_t)addr + len;
    for (uint64_t pg = addr & ~(uint64_t)(GPAGE_SIZE - 1); pg < end; pg += GPAGE_SIZE) {
        if (gmem_is_mapped((gaddr_t)pg, GPAGE_SIZE)) {
            bool full = pg >= addr && pg + GPAGE_SIZE <= end;
            gmem_protect((gaddr_t)pg, GPAGE_SIZE, full ? GPROT_RW : GPROT_RWX);
        } else if (!gmem_map_fixed((gaddr_t)pg, GPAGE_SIZE, GPROT_RW)) {
            return false;
        }
    }
    return true;
}

// Drop bookkeeping for mappings fully covered by [a, b).
static void maps_forget_range(gaddr_t a, uint64_t b) {
    for (size_t i = 0; i < nmaps;) {
        if (maps[i].start >= a && (uint64_t)maps[i].start + maps[i].len <= b) {
            if (maps[i].fd >= 0) close(maps[i].fd);
            maps[i] = maps[--nmaps];
        } else
            i++;
    }
}

// mmap(addr, len, prot, flags, fd, off_t offset)
THUNK(mmap) {
    gaddr_t addr = ARG(0);
    uint32_t len = ARG(1);
    int prot = ARGI(2), flags = ARGI(3), fd = ARGI(4);
    off_t off = (off_t)ARG64(5);
    bool anon = (flags & MAP_ANON) != 0, fixed = (flags & MAP_FIXED) != 0;
    if (len == 0 || (off & (GPAGE_GUEST - 1)) || (fixed && (addr & (GPAGE_GUEST - 1))) ||
        (uint64_t)len + GPAGE_SIZE > 0xffffffffULL) {
        set_gerrno(EINVAL);
        RET(MAP_FAILED_G);
        return;
    }
    if (fixed) cpu_code_changed(addr, len); // replaces whatever was mapped there
    if (!anon) {
        struct stat st;
        if (fstat(fd, &st)) {
            set_gerrno(EBADF);
            RET(MAP_FAILED_G);
            return;
        }
    }
    uint32_t glen = (uint32_t)round_up(len, GPAGE_GUEST);
    pthread_mutex_lock(&maps_lock);
    gaddr_t start;
    bool own_pages; // the whole gmem allocation belongs to this mapping
    if (fixed) {
        if (!claim_fixed(addr, glen)) {
            pthread_mutex_unlock(&maps_lock);
            set_gerrno(ENOMEM);
            RET(MAP_FAILED_G);
            return;
        }
        start = addr;
        own_pages = (addr & (GPAGE_SIZE - 1)) == 0 && (glen & (GPAGE_SIZE - 1)) == 0;
        maps_forget_range(addr, (uint64_t)addr + glen);
    } else {
        start = gmem_map_anywhere(glen, GPROT_RW, 0);
        if (!start) {
            pthread_mutex_unlock(&maps_lock);
            set_gerrno(ENOMEM);
            RET(MAP_FAILED_G);
            return;
        }
        own_pages = true;
    }
    struct gmapping m = {start, glen, (uint32_t)round_up(glen, GPAGE_SIZE), -1, off};
    bool ok = true, direct = false;
    if (anon) {
        if (fixed) memset(g2h(start), 0, glen);
    } else if (own_pages && (off & (GPAGE_SIZE - 1)) == 0) {
        // Real file mapping over our own pages.
        void *want = g2h(start);
        void *p = mmap(want, m.alloc, host_mprot(prot), (flags & (MAP_SHARED | MAP_PRIVATE)) | MAP_FIXED, fd, off);
        if (p == want) direct = true;
        else {
            // e.g. shared-writable on a read-only fd: report the host error
            ok = false;
            set_gerrno(errno);
            if (p != MAP_FAILED) munmap(p, m.alloc);
            mmap(want, m.alloc, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        }
    } else {
        ok = fill_from_file(start, glen, fd, off);
        if (!ok) set_gerrno(errno);
        if (ok && (flags & MAP_SHARED) && (prot & PROT_WRITE)) m.fd = dup(fd);
    }
    if (!ok) {
        if (!fixed) gmem_unmap(start, glen);
        pthread_mutex_unlock(&maps_lock);
        RET(MAP_FAILED_G);
        return;
    }
    if (!direct) {
        if (own_pages && !fixed) gmem_protect(start, m.alloc, prot);
        else protect_range(start, glen, prot);
    }
    maps_add(m);
    pthread_mutex_unlock(&maps_lock);
    LOGD("mmap(%#x, %#x, prot %d, flags %#x, fd %d, off %lld) -> %#x%s", addr, len, prot, flags, fd,
         (long long)off, start, direct ? " (direct)" : "");
    RET(start);
}

THUNK_ALIAS(mmap_unix2003, "_mmap$UNIX2003", mmap)

// Unmap [a, b): whole gmem pages only. A range reaching the (4 KB-rounded) end
// of a tracked mapping extends to the end of its gmem allocation.
static bool do_munmap(gaddr_t a, uint64_t b) {
    bool found = false;
    for (size_t i = 0; i < nmaps;) {
        struct gmapping *m = &maps[i];
        uint64_t ms = m->start, me = (uint64_t)m->start + m->len, ma = (uint64_t)m->start + m->alloc;
        if (b <= ms || a >= me) {
            i++;
            continue;
        }
        found = true;
        writeback(m, a, (gaddr_t)(b < me ? b : me));
        uint64_t ua = a > ms ? a : ms, ub = b >= me ? ma : b;
        ua = round_up(ua, GPAGE_SIZE);
        ub &= ~(uint64_t)(GPAGE_SIZE - 1);
        if (ub > ua) gmem_unmap((gaddr_t)ua, (uint32_t)(ub - ua));
        if (a <= ms && b >= me) {
            if (m->fd >= 0) close(m->fd);
            maps[i] = maps[--nmaps];
            continue;
        }
        if (a <= ms) { // prefix removed
            m->len = (uint32_t)(me - b);
            m->alloc = (uint32_t)(ma - b);
            m->off += (off_t)(b - ms);
            m->start = (gaddr_t)b;
        } else if (b >= me) { // suffix removed
            m->len = (uint32_t)(a - ms);
            m->alloc = (uint32_t)round_up(m->len, GPAGE_SIZE);
        }
        i++;
    }
    return found;
}

bool libc_munmap_tracked(gaddr_t addr, uint32_t size) {
    pthread_mutex_lock(&maps_lock);
    bool found = do_munmap(addr, (uint64_t)addr + round_up(size, GPAGE_GUEST));
    pthread_mutex_unlock(&maps_lock);
    return found;
}

THUNK(munmap) {
    gaddr_t a = ARG(0);
    uint32_t len = ARG(1);
    if ((a & (GPAGE_GUEST - 1)) || len == 0) {
        set_gerrno(EINVAL);
        RET(-1);
        return;
    }
    uint64_t b = (uint64_t)a + round_up(len, GPAGE_GUEST);
    cpu_code_changed(a, (uint32_t)(b - a)); // the range may be reused for other code
    pthread_mutex_lock(&maps_lock);
    if (!do_munmap(a, b)) {
        // Not created by mmap: release only whole pages inside the range.
        uint64_t ua = round_up(a, GPAGE_SIZE), ub = b & ~(uint64_t)(GPAGE_SIZE - 1);
        if (ub > ua) gmem_unmap((gaddr_t)ua, (uint32_t)(ub - ua));
    }
    pthread_mutex_unlock(&maps_lock);
    RET(0);
}
THUNK_ALIAS(munmap_unix2003, "_munmap$UNIX2003", munmap)

THUNK(msync) {
    gaddr_t a = ARG(0);
    uint64_t b = (uint64_t)a + ARG(1);
    pthread_mutex_lock(&maps_lock);
    for (size_t i = 0; i < nmaps; i++) {
        struct gmapping *m = &maps[i];
        if (b <= m->start || a >= (uint64_t)m->start + m->len) continue;
        if (m->fd >= 0) writeback(m, a, (gaddr_t)b);
    }
    pthread_mutex_unlock(&maps_lock);
    // Direct file mappings: let the host flush them (page-aligned range).
    uint64_t ha = a & ~(uint64_t)(GPAGE_SIZE - 1);
    if (b > ha && gmem_is_mapped((gaddr_t)ha, (uint32_t)(b - ha)))
        msync(g2h((gaddr_t)ha), (size_t)round_up(b - ha, GPAGE_SIZE), ARGI(2) & (MS_ASYNC | MS_SYNC | MS_INVALIDATE));
    RET(0);
}
THUNK_ALIAS(msync_unix2003, "_msync$UNIX2003", msync)
THUNK_ALIAS(msync_nocancel, "_msync$NOCANCEL$UNIX2003", msync)

THUNK(mprotect) {
    if (!gmem_is_mapped(ARG(0), ARG(1))) {
        set_gerrno(ENOMEM);
        RET(-1);
        return;
    }
    int prot = ARGI(2) & 7;
    uint64_t end = (uint64_t)ARG(0) + ARG(1);
    if (prot & GPROT_EXEC) cpu_code_changed(ARG(0), ARG(1)); // code written, now made executable
    uint64_t fs = round_up(ARG(0), GPAGE_SIZE), fe = end & ~(uint64_t)(GPAGE_SIZE - 1);
    if (fe > fs) gmem_protect((gaddr_t)fs, (uint32_t)(fe - fs), prot);
    // Partially covered pages are shared: only widen their access.
    if (prot & (GPROT_WRITE | GPROT_EXEC)) {
        if (ARG(0) < fs) gmem_protect(ARG(0) & ~(GPAGE_SIZE - 1), GPAGE_SIZE, prot | GPROT_RW);
        if (end > fe && fe >= fs) gmem_protect((gaddr_t)fe, GPAGE_SIZE, prot | GPROT_RW);
    }
    RET(0);
}
THUNK_ALIAS(mprotect_unix2003, "_mprotect$UNIX2003", mprotect)
THUNK(madvise) { RET(0); }
THUNK(posix_madvise) { RET(0); }
THUNK(mlock) { RET(0); }
THUNK(munlock) { RET(0); }

// --- POSIX aio (performed synchronously at submission) -------------------------------------------
struct aio_done {
    gaddr_t cb;
    int err;
    int32_t ret;
};
static struct aio_done *aio_tab;
static size_t naio, aio_cap;
static pthread_mutex_t aio_lock = PTHREAD_MUTEX_INITIALIZER;

static struct aio_done *aio_find(gaddr_t cb) {
    for (size_t i = 0; i < naio; i++)
        if (aio_tab[i].cb == cb) return &aio_tab[i];
    return NULL;
}

#define AIO_OP_FSYNC 100
static void aio_run(gaddr_t cb, int op) {
    int fd = (int)gread32(cb);
    off_t off = (off_t)gread64(cb + 4);
    void *buf = g2h(gread32(cb + 12));
    size_t n = gread32(cb + 16);
    if (gread32(cb + 24) != SIGEV_NONE) LOGD("aio: notification %u ignored", gread32(cb + 24));
    ssize_t rc;
    if (op == LIO_READ) rc = pread(fd, buf, n, off);
    else if (op == LIO_WRITE) rc = pwrite(fd, buf, n, off);
    else rc = fsync(fd);
    int err = rc < 0 ? errno : 0;
    pthread_mutex_lock(&aio_lock);
    struct aio_done *d = aio_find(cb);
    if (!d) {
        if (naio == aio_cap) {
            aio_cap = aio_cap ? aio_cap * 2 : 32;
            aio_tab = realloc(aio_tab, aio_cap * sizeof *aio_tab);
        }
        d = &aio_tab[naio++];
    }
    *d = (struct aio_done){cb, err, (int32_t)rc};
    pthread_mutex_unlock(&aio_lock);
}

THUNK(aio_read) { aio_run(ARG(0), LIO_READ); RET(0); }
THUNK(aio_write) { aio_run(ARG(0), LIO_WRITE); RET(0); }
THUNK(aio_fsync) { aio_run(ARG(1), AIO_OP_FSYNC); RET(0); }
THUNK(aio_error) {
    pthread_mutex_lock(&aio_lock);
    struct aio_done *d = aio_find(ARG(0));
    int e = d ? d->err : -1;
    pthread_mutex_unlock(&aio_lock);
    if (e < 0) set_gerrno(EINVAL);
    RET(e);
}
THUNK(aio_return) {
    pthread_mutex_lock(&aio_lock);
    struct aio_done *d = aio_find(ARG(0));
    int32_t ret = -1;
    int err = EINVAL;
    if (d) {
        ret = d->ret;
        err = d->err;
        *d = aio_tab[--naio];
    }
    pthread_mutex_unlock(&aio_lock);
    if (ret < 0) set_gerrno(err);
    RET(ret);
}
// aio_suspend(const struct aiocb *const list[], int nent, const struct timespec *timeout)
THUNK(aio_suspend) {
    bool any = false;
    pthread_mutex_lock(&aio_lock);
    for (int i = 0; i < ARGI(1); i++) {
        gaddr_t cb = gread32(ARG(0) + 4 * (uint32_t)i);
        if (cb && aio_find(cb)) any = true;
    }
    pthread_mutex_unlock(&aio_lock);
    if (!any) set_gerrno(EAGAIN);
    RET(any ? 0 : -1);
}
THUNK_ALIAS(aio_suspend_unix2003, "_aio_suspend$UNIX2003", aio_suspend)
THUNK_ALIAS(aio_suspend_nocancel, "_aio_suspend$NOCANCEL$UNIX2003", aio_suspend)
THUNK(aio_cancel) { RET(AIO_ALLDONE); }
// lio_listio(mode, list[], nent, sigevent*)
THUNK(lio_listio) {
    for (int i = 0; i < ARGI(2); i++) {
        gaddr_t cb = gread32(ARG(1) + 4 * (uint32_t)i);
        if (!cb) continue;
        int op = (int)gread32(cb + 44);
        if (op == LIO_READ || op == LIO_WRITE) aio_run(cb, op);
    }
    RET(0);
}
