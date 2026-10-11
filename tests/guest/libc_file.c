// fd I/O, stat (legacy + 64-bit inode layouts), directories, mmap, aio.
// Built with legacy (32-bit inode) symbols; the $INODE64 variants are declared explicitly.
#define _DARWIN_NO_64_BIT_INODE 1
#include <aio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include "libc_check.h"

// 64-bit inode layout (struct stat64 == struct stat$INODE64), 108 bytes in i386.
int stat_inode64(const char *, struct stat64 *) __asm("_stat$INODE64");
int fstat_inode64(int, struct stat64 *) __asm("_fstat$INODE64");
struct dirent64 {
    uint64_t d_ino, d_seekoff;
    uint16_t d_reclen, d_namlen;
    uint8_t d_type;
    char d_name[1024];
};
struct dirent64 *readdir_inode64(DIR *) __asm("_readdir$INODE64");
DIR *opendir_inode64(const char *) __asm("_opendir$INODE64$UNIX2003");
off_t lseek_(int, off_t, int) __asm("_lseek");

_Static_assert(sizeof(struct stat) == 96, "legacy stat");
_Static_assert(sizeof(struct stat64) == 108, "stat64");
_Static_assert(offsetof(struct stat64, st_size) == 60, "stat64 size");
_Static_assert(sizeof(struct dirent) == 264, "legacy dirent");
_Static_assert(sizeof(struct dirent64) == 1048 && offsetof(struct dirent64, d_name) == 21, "dirent64");

static char dir[256], path[300];

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/dxh_libc_file_%d", tmp ? tmp : "/tmp", getpid());
    CHECK(mkdir(dir, 0755) == 0, "mkdir errno %d", errno);
    snprintf(path, sizeof path, "%s/a.bin", dir);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open");
    CHECK(write(fd, "hello world", 11) == 11, "write");
    struct iovec iov[2] = {{"ABC", 3}, {"DEFG", 4}};
    CHECK(writev(fd, iov, 2) == 7, "writev");
    CHECK(lseek(fd, 0, SEEK_CUR) == 18, "lseek cur");
    // 64-bit offsets: seek past 4 GB on a sparse file
    off_t far = lseek_(fd, 0x100000010LL, SEEK_SET);
    CHECK(far == 0x100000010LL, "lseek 64-bit %llx", (long long)far);
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek set");
    char buf[64] = {0};
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0, "read");
    CHECK(pread(fd, buf, 4, 11) == 4 && memcmp(buf, "ABCD", 4) == 0, "pread");
    CHECK(pwrite(fd, "xy", 2, 0) == 2, "pwrite");
    CHECK(fcntl(fd, F_GETFL) & O_RDWR, "fcntl getfl");
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC, "fcntl fd flags");
    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 4};
    CHECK(fcntl(fd, F_SETLK, &fl) == 0, "fcntl setlk");
    char gp[1024];
    CHECK(fcntl(fd, F_GETPATH, gp) == 0 && strstr(gp, "a.bin"), "fcntl getpath");
    struct pollfd pfd = {fd, POLLIN, 0};
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN), "poll");
    CHECK(fsync(fd) == 0, "fsync");

    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 18 && S_ISREG(st.st_mode) && st.st_nlink == 1, "fstat size %lld", (long long)st.st_size);
    CHECK(st.st_blksize > 0 && st.st_mtimespec.tv_sec > 1000000000, "fstat times");
    struct stat64 st64;
    CHECK(fstat_inode64(fd, &st64) == 0 && st64.st_size == 18 && S_ISREG(st64.st_mode), "fstat$INODE64");
    CHECK((uint32_t)st64.st_ino == st.st_ino && st64.st_birthtimespec.tv_sec > 1000000000, "inode/birthtime");
    CHECK(ftruncate(fd, 0x100000000LL + 5) == 0, "ftruncate >4G");
    CHECK(stat(path, &st) == 0 && st.st_size == 0x100000005LL, "stat after ftruncate %llx", (long long)st.st_size);
    CHECK(stat_inode64(path, &st64) == 0 && st64.st_size == 0x100000005LL && st64.st_mode == st.st_mode, "stat$INODE64");
    CHECK(truncate(path, 18) == 0, "truncate");
    CHECK(stat64(path, &st64) == 0 && st64.st_size == 18, "stat64");
    CHECK(access(path, R_OK | W_OK) == 0 && access("/nonexistent/x", F_OK) == -1 && errno == ENOENT, "access/errno");
    CHECK(chmod(path, 0600) == 0 && stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "chmod");
    struct timeval tv[2] = {{1000000000, 0}, {1200000000, 500000}};
    CHECK(utimes(path, tv) == 0 && stat(path, &st) == 0 && st.st_mtimespec.tv_sec == 1200000000 &&
              st.st_atimespec.tv_sec == 1000000000, "utimes");
    CHECK(futimes(fd, NULL) == 0, "futimes");

    char link[300], target[300] = {0};
    snprintf(link, sizeof link, "%s/link", dir);
    CHECK(symlink(path, link) == 0, "symlink");
    CHECK(readlink(link, target, sizeof target) == (ssize_t)strlen(path) && strcmp(target, path) == 0, "readlink");
    CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode), "lstat");

    char tmpl[300];
    snprintf(tmpl, sizeof tmpl, "%s/tmpXXXXXX", dir);
    int tfd = mkstemp(tmpl);
    CHECK(tfd >= 0 && strstr(tmpl, "XXXXXX") == NULL, "mkstemp template %s", tmpl);
    close(tfd);

    // directories: legacy and 64-bit inode dirents
    DIR *d = opendir(dir);
    CHECK(d != NULL, "opendir");
    int seen = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (strcmp(de->d_name, "a.bin") == 0) {
            seen |= 1;
            CHECK(de->d_namlen == 5 && de->d_type == DT_REG && de->d_ino == (uint32_t)st64.st_ino, "dirent fields");
        }
        if (strcmp(de->d_name, "link") == 0) {
            seen |= 2;
            CHECK(de->d_type == DT_LNK, "dirent link type");
        }
    }
    CHECK(seen == 3, "readdir found entries %d", seen);
    rewinddir(d);
    CHECK(readdir(d) != NULL, "rewinddir");
    CHECK(closedir(d) == 0, "closedir");
    d = opendir_inode64(dir);
    seen = 0;
    struct dirent64 *de64;
    while (d && (de64 = readdir_inode64(d)))
        if (strcmp(de64->d_name, "a.bin") == 0) {
            seen = 1;
            CHECK(de64->d_ino == st64.st_ino && de64->d_namlen == 5 && de64->d_type == DT_REG, "dirent64 fields");
        }
    CHECK(seen, "readdir$INODE64");
    closedir(d);
    CHECK(opendir("/nonexistent") == NULL && errno == ENOENT, "opendir errno");

    // mmap
    char *anon = mmap(NULL, 10000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(anon != MAP_FAILED && anon[0] == 0 && anon[9999] == 0, "mmap anon");
    if (anon != MAP_FAILED) {
        memset(anon, 7, 10000);
        CHECK(munmap(anon, 10000) == 0, "munmap anon");
        char *again = mmap(anon, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        CHECK(again == anon && again[100] == 0, "mmap fixed");
        munmap(again, 8192);
    }
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 999, 0) == MAP_FAILED && errno == EBADF, "mmap EBADF");
    lseek(fd, 0, SEEK_SET);
    char *fm = mmap(NULL, 18, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(fm != MAP_FAILED && memcmp(fm, "xyllo worldABCDEFG", 18) == 0 && fm[18] == 0, "mmap file (direct)");
    if (fm != MAP_FAILED) munmap(fm, 18);
    // 4 KB-aligned offset: not host-page aligned -> copied mapping, shared write-back
    char page[8192];
    memset(page, 'p', sizeof page);
    pwrite(fd, page, sizeof page, 4096);
    char *sm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    CHECK(sm != MAP_FAILED && sm[0] == 'p' && sm[4095] == 'p', "mmap file offset 4096");
    if (sm != MAP_FAILED) {
        memcpy(sm, "SHARED", 6);
        CHECK(msync(sm, 4096, MS_SYNC) == 0, "msync");
        CHECK(pread(fd, buf, 6, 4096) == 6 && memcmp(buf, "SHARED", 6) == 0, "msync wrote back");
        sm[10] = 'Q';
        munmap(sm, 4096);
        CHECK(pread(fd, buf, 1, 4106) == 1 && buf[0] == 'Q', "munmap wrote back");
    }
    char *dm = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(dm != MAP_FAILED, "mmap shared direct");
    if (dm != MAP_FAILED) {
        dm[1] = 'Z';
        msync(dm, 16384, MS_SYNC);
        CHECK(pread(fd, buf, 2, 0) == 2 && buf[1] == 'Z', "shared direct mapping writes file");
        munmap(dm, 16384);
    }

    // aio
    struct aiocb cb;
    _Static_assert(sizeof cb == 48 && offsetof(struct aiocb, aio_buf) == 12 && offsetof(struct aiocb, aio_lio_opcode) == 44, "aiocb");
    memset(&cb, 0, sizeof cb);
    memset(buf, 0, sizeof buf);
    cb.aio_fildes = fd;
    cb.aio_offset = 4096;
    cb.aio_buf = buf;
    cb.aio_nbytes = 6;
    CHECK(aio_read(&cb) == 0, "aio_read");
    const struct aiocb *list[1] = {&cb};
    CHECK(aio_suspend(list, 1, NULL) == 0, "aio_suspend");
    CHECK(aio_error(&cb) == 0, "aio_error");
    CHECK(aio_return(&cb) == 6 && memcmp(buf, "SHARED", 6) == 0, "aio_return");
    cb.aio_buf = "AIO";
    cb.aio_nbytes = 3;
    cb.aio_offset = 0;
    CHECK(aio_write(&cb) == 0 && aio_error(&cb) == 0 && aio_return(&cb) == 3, "aio_write");
    CHECK(pread(fd, buf, 3, 0) == 3 && memcmp(buf, "AIO", 3) == 0, "aio_write data");

    CHECK(close(fd) == 0 && close(fd) == -1 && errno == EBADF, "close");
    CHECK(unlink(link) == 0 && unlink(path) == 0 && unlink(tmpl) == 0 && rmdir(dir) == 0, "cleanup");
    DONE();
}
