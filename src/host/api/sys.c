// System information: sysctl, users, resource limits/usage, uname, mach VM/port calls.
//
// sysctl values are converted by their declared format (queried from the
// kernel's sysctl.oidfmt): "L"/"LU" (long) shrink to 32 bit, "S,timeval" and
// "S,loadavg" are converted, KERN_PROC returns i386 struct kinfo_proc (492 bytes).
// A few values are reported as an i386 Intel Mac would (CPU type, machine).
#include "common.h"
#include "libc.h"

#include <mach/mach.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <uuid/uuid.h>

// --- spoofed values ---------------------------------------------------------------------
enum { SV_INT, SV_QUAD, SV_STR };
struct spoof {
    const char *name;
    int type;
    uint64_t (*value)(void); // numeric values
    const char *str;
};

static uint64_t host_u64(const char *name, uint64_t dflt) {
    uint64_t v = 0;
    size_t l = sizeof v;
    if (sysctlbyname(name, &v, &l, NULL, 0) == 0) return l == 4 ? (uint32_t)v : v;
    return dflt;
}
static uint64_t v_cputype(void) { return 7; }    // CPU_TYPE_X86
static uint64_t v_cpusubtype(void) { return 4; } // CPU_SUBTYPE_X86_ARCH1 (i486-class "all")
static uint64_t v_one(void) { return 1; }
static uint64_t v_zero(void) { return 0; }
static uint64_t v_pagesize(void) { return 4096; } // matches getpagesize()
// hw.physmem / hw.usermem are 32-bit ints, capped at 2 GB on real machines.
static uint64_t v_physmem(void) {
    uint64_t m = host_u64("hw.memsize", 0x80000000ULL);
    return m > 0x80000000ULL ? 0x80000000ULL : m;
}
static uint64_t v_cpufreq(void) { return host_u64("hw.cpufrequency", 2400000000ULL); }
static uint64_t v_cpufreq_max(void) { return host_u64("hw.cpufrequency_max", v_cpufreq()); }
static uint64_t v_busfreq(void) { return host_u64("hw.busfrequency", 100000000ULL); }

static const struct spoof spoofs[] = {
    {"hw.cputype", SV_INT, v_cputype, NULL},
    {"hw.cpusubtype", SV_INT, v_cpusubtype, NULL},
    {"hw.cpu64bit_capable", SV_INT, v_one, NULL},
    {"hw.optional.x86_64", SV_INT, v_one, NULL},
    {"hw.machine", SV_STR, NULL, "i386"},
    {"hw.pagesize", SV_QUAD, v_pagesize, NULL},
    {"hw.physmem", SV_INT, v_physmem, NULL},
    {"hw.usermem", SV_INT, v_physmem, NULL},
    {"hw.cpufrequency", SV_QUAD, v_cpufreq, NULL},
    {"hw.cpufrequency_max", SV_QUAD, v_cpufreq_max, NULL},
    {"hw.cpufrequency_min", SV_QUAD, v_cpufreq, NULL},
    {"hw.busfrequency", SV_QUAD, v_busfreq, NULL},
    {"hw.busfrequency_max", SV_QUAD, v_busfreq, NULL},
    {"sysctl.proc_native", SV_INT, v_one, NULL},
    {"sysctl.proc_translated", SV_INT, v_zero, NULL},
};

static const struct spoof *find_spoof(const char *name) {
    for (size_t i = 0; i < sizeof spoofs / sizeof spoofs[0]; i++)
        if (strcmp(spoofs[i].name, name) == 0) return &spoofs[i];
    return NULL;
}

// Copy out a guest-format value following sysctl semantics. Returns 0 or an errno.
static int copy_out(const void *data, uint32_t len, gaddr_t oldp, gaddr_t oldlenp) {
    if (!oldlenp) return 0;
    if (!oldp) {
        gwrite32(oldlenp, len);
        return 0;
    }
    uint32_t cap = gread32(oldlenp);
    uint32_t n = len < cap ? len : cap;
    memcpy(g2h(oldp), data, n);
    gwrite32(oldlenp, n);
    return n < len ? ENOMEM : 0;
}

static int spoof_out(const struct spoof *s, gaddr_t oldp, gaddr_t oldlenp) {
    if (s->type == SV_STR) return copy_out(s->str, (uint32_t)strlen(s->str) + 1, oldp, oldlenp);
    uint64_t v = s->value();
    if (s->type == SV_INT) {
        uint32_t w = (uint32_t)v;
        return copy_out(&w, 4, oldp, oldlenp);
    }
    // 64-bit values: a 4-byte buffer gets the (saturated) 32-bit value, like the
    // kernel's generic hw handlers.
    if (oldp && oldlenp && gread32(oldlenp) == 4) {
        uint32_t w = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
        return copy_out(&w, 4, oldp, oldlenp);
    }
    return copy_out(&v, 8, oldp, oldlenp);
}

// --- generic path --------------------------------------------------------------------------
static bool mib_name(const int *mib, u_int n, char *buf, size_t cap) {
    int q[CTL_MAXNAME + 2] = {0, 1};
    if (n > CTL_MAXNAME) return false;
    memcpy(q + 2, mib, n * sizeof(int));
    size_t l = cap;
    return sysctl(q, n + 2, buf, &l, NULL, 0) == 0;
}

static bool mib_format(const int *mib, u_int n, u_int *kind, char *fmt, size_t cap) {
    int q[CTL_MAXNAME + 2] = {0, 4};
    if (n > CTL_MAXNAME) return false;
    memcpy(q + 2, mib, n * sizeof(int));
    char buf[256];
    size_t l = sizeof buf;
    if (sysctl(q, n + 2, buf, &l, NULL, 0) != 0 || l < sizeof(u_int)) return false;
    memcpy(kind, buf, sizeof(u_int));
    buf[sizeof buf - 1] = 0;
    strlcpy(fmt, buf + sizeof(u_int), cap);
    return true;
}

// i386 struct kinfo_proc: 492 bytes. Only commonly inspected fields are filled.
#define KP32_SIZE 492
static void put_kinfo_proc(uint8_t *g, const struct kinfo_proc *kp) {
    memset(g, 0, KP32_SIZE);
    uint32_t v;
#define PUT32(off, val) (v = (uint32_t)(val), memcpy(g + (off), &v, 4))
    PUT32(0, kp->kp_proc.p_starttime.tv_sec);
    PUT32(4, kp->kp_proc.p_starttime.tv_usec);
    PUT32(16, kp->kp_proc.p_flag);
    g[20] = (uint8_t)kp->kp_proc.p_stat;
    PUT32(24, kp->kp_proc.p_pid);
    memcpy(g + 163, kp->kp_proc.p_comm, MAXCOMLEN + 1);
    PUT32(280, kp->kp_eproc.e_pcred.p_ruid);
    PUT32(304, kp->kp_eproc.e_ucred.cr_uid);
    PUT32(416, kp->kp_eproc.e_ppid);
    PUT32(420, kp->kp_eproc.e_pgid);
#undef PUT32
}

static int do_sysctl(int *mib, u_int n, const char *name, gaddr_t oldp, gaddr_t oldlenp, gaddr_t newp,
                     uint32_t newlen) {
    char namebuf[256];
    if (!name && mib_name(mib, n, namebuf, sizeof namebuf)) name = namebuf;
    if (name) {
        const struct spoof *s = find_spoof(name);
        if (s) {
            if (newp) return EPERM;
            return spoof_out(s, oldp, oldlenp);
        }
    }
    LOGD("sysctl %s", name ? name : "(unnamed)");
    void *hnew = newp ? g2h(newp) : NULL;

    // kern.proc.*: arrays of struct kinfo_proc.
    if (n >= 3 && mib[0] == CTL_KERN && mib[1] == KERN_PROC) {
        size_t hl = 0;
        if (sysctl(mib, n, NULL, &hl, NULL, 0)) return errno;
        uint32_t count = (uint32_t)(hl / sizeof(struct kinfo_proc));
        if (!oldp) return copy_out(NULL, (count + 8) * KP32_SIZE, 0, oldlenp);
        struct kinfo_proc *kp = malloc(hl ? hl : 1);
        if (sysctl(mib, n, kp, &hl, NULL, 0)) {
            int e = errno;
            free(kp);
            return e;
        }
        count = (uint32_t)(hl / sizeof(struct kinfo_proc));
        uint8_t *out = calloc(count ? count : 1, KP32_SIZE);
        for (uint32_t i = 0; i < count; i++) put_kinfo_proc(out + i * KP32_SIZE, &kp[i]);
        int rc = copy_out(out, count * KP32_SIZE, oldp, oldlenp);
        free(out);
        free(kp);
        return rc;
    }

    u_int kind = 0;
    char fmt[64] = "";
    bool have_fmt = mib_format(mib, n, &kind, fmt, sizeof fmt);
    bool is_long = have_fmt && (strcmp(fmt, "L") == 0 || strcmp(fmt, "LU") == 0);
    bool is_tv = have_fmt && strcmp(fmt, "S,timeval") == 0;
    bool is_loadavg = have_fmt && strcmp(fmt, "S,loadavg") == 0;

    if (!is_long && !is_tv && !is_loadavg) {
        // Same representation on both sides (int, quad, string, identical structs).
        if (have_fmt && (kind & CTLTYPE) == CTLTYPE_STRUCT && strcmp(fmt, "S,clockinfo") &&
            strcmp(fmt, "S,xsw_usage"))
            LOGI("sysctl %s: struct format '%s' passed through unconverted", name ? name : "?", fmt);
        size_t hl = oldlenp ? gread32(oldlenp) : 0;
        int rc = sysctl(mib, n, oldp ? g2h(oldp) : NULL, oldlenp ? &hl : NULL, hnew, newlen);
        int e = rc ? errno : 0;
        if (oldlenp && (rc == 0 || e == ENOMEM)) gwrite32(oldlenp, (uint32_t)hl);
        return e;
    }

    // Converted types: read the host value into a temporary buffer.
    if (newp) {
        if (!is_long) return EPERM;
        uint32_t cnt = newlen / 4;
        int64_t *nv = calloc(cnt ? cnt : 1, 8);
        for (uint32_t i = 0; i < cnt; i++) nv[i] = (int32_t)gread32(newp + 4 * i);
        int rc = sysctl(mib, n, NULL, NULL, nv, cnt * 8);
        free(nv);
        if (rc) return errno;
        if (!oldlenp) return 0;
    }
    size_t hl = 0;
    if (sysctl(mib, n, NULL, &hl, NULL, 0)) return errno;
    uint8_t *hbuf = calloc(1, hl + 16);
    if (sysctl(mib, n, hbuf, &hl, NULL, 0)) {
        int e = errno;
        free(hbuf);
        return e;
    }
    uint8_t gbuf[512];
    uint32_t gl = 0;
    if (is_long) {
        for (size_t i = 0; i + 8 <= hl && gl + 4 <= sizeof gbuf; i += 8, gl += 4) {
            int64_t v;
            memcpy(&v, hbuf + i, 8);
            uint32_t w = (uint32_t)v;
            if (v > INT32_MAX && (uint64_t)v <= UINT32_MAX) w = (uint32_t)v;
            else if (v > UINT32_MAX) w = INT32_MAX;
            memcpy(gbuf + gl, &w, 4);
        }
    } else if (is_tv) {
        struct timeval tv;
        memcpy(&tv, hbuf, sizeof tv);
        uint32_t w[2] = {(uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec};
        memcpy(gbuf, w, 8);
        gl = 8;
    } else { // struct loadavg {fixpt_t ldavg[3]; long fscale}
        struct loadavg la;
        memcpy(&la, hbuf, sizeof la);
        uint32_t w[4] = {la.ldavg[0], la.ldavg[1], la.ldavg[2], (uint32_t)la.fscale};
        memcpy(gbuf, w, 16);
        gl = 16;
    }
    free(hbuf);
    return copy_out(gbuf, gl, oldp, oldlenp);
}

// sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
THUNK(sysctl) {
    u_int n = ARG(1);
    if (n > CTL_MAXNAME) {
        set_gerrno(EINVAL);
        RET(-1);
        return;
    }
    int mib[CTL_MAXNAME];
    memcpy(mib, ARGP(0), n * sizeof(int));
    int e = do_sysctl(mib, n, NULL, ARG(2), ARG(3), ARG(4), ARG(5));
    if (e) set_gerrno(e);
    RET(e ? -1 : 0);
}
// sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
THUNK(sysctlbyname) {
    const char *name = ARGS(0);
    int e;
    const struct spoof *s = find_spoof(name);
    if (s) {
        e = ARG(3) ? EPERM : spoof_out(s, ARG(1), ARG(2));
    } else {
        int mib[CTL_MAXNAME];
        size_t n = CTL_MAXNAME;
        if (sysctlnametomib(name, mib, &n)) e = errno;
        else e = do_sysctl(mib, (u_int)n, name, ARG(1), ARG(2), ARG(3), ARG(4));
    }
    if (e) {
        LOGD("sysctlbyname(%s) -> %s", name, strerror(e));
        set_gerrno(e);
    }
    RET(e ? -1 : 0);
}
// sysctlnametomib(const char *name, int *mibp, size_t *sizep)
THUNK(sysctlnametomib) {
    size_t n = gread32(ARG(2));
    int rc = sysctlnametomib(ARGS(0), ARGP(1), &n);
    if (rc) set_gerrno(errno);
    else gwrite32(ARG(2), (uint32_t)n);
    RET(rc);
}

// --- users -----------------------------------------------------------------------------------
// i386 struct passwd (40 bytes): name@0 passwd@4 uid@8 gid@12 change@16 class@20
// gecos@24 dir@28 shell@32 expire@36. Strings live in the same per-thread block.
static _Thread_local gaddr_t tls_passwd;

static gaddr_t put_passwd(const struct passwd *pw) {
    if (!pw) return 0;
    const char *strs[6] = {pw->pw_name, pw->pw_passwd, pw->pw_class, pw->pw_gecos, pw->pw_dir, pw->pw_shell};
    static const uint32_t offs[6] = {0, 4, 20, 24, 28, 32};
    uint32_t size = 40;
    for (int i = 0; i < 6; i++) size += (uint32_t)strlen(strs[i] ? strs[i] : "") + 1;
    gheap_free(tls_passwd);
    gaddr_t g = tls_passwd = gheap_calloc(1, size), p = g + 40;
    for (int i = 0; i < 6; i++) {
        const char *s = strs[i] ? strs[i] : "";
        strcpy(g2h(p), s);
        gwrite32(g + offs[i], p);
        p += (uint32_t)strlen(s) + 1;
    }
    gwrite32(g + 8, pw->pw_uid);
    gwrite32(g + 12, pw->pw_gid);
    gwrite32(g + 16, (uint32_t)pw->pw_change);
    gwrite32(g + 36, (uint32_t)pw->pw_expire);
    return g;
}
THUNK(getpwuid) { RET(put_passwd(getpwuid(ARG(0)))); }
THUNK(getpwnam) { RET(put_passwd(getpwnam(ARGS(0)))); }
THUNK(getlogin) {
    const char *s = getlogin();
    RET(s ? guest_static_string(5, s) : 0);
}
THUNK(getegid) { RET(getegid()); }
THUNK(getpgrp) { RET(getpgrp()); }
THUNK(getsid) { RET_ERRNO(getsid(ARGI(0))); }

// --- resources --------------------------------------------------------------------------------
// struct rlimit (two 64-bit rlim_t) is identical.
THUNK(getrlimit) { RET_ERRNO(getrlimit(ARGI(0), ARGP(1))); }
THUNK_ALIAS(getrlimit_unix2003, "_getrlimit$UNIX2003", getrlimit)
THUNK_AS(setrlimit_unix2003, "_setrlimit$UNIX2003") { RET(0); } // like _setrlimit in process.c

// i386 struct rusage: two timevals then 14 longs (72 bytes).
THUNK(getrusage) {
    struct rusage ru;
    int rc = getrusage(ARGI(0), &ru);
    if (rc) {
        set_gerrno(errno);
        RET(rc);
        return;
    }
    gaddr_t g = ARG(1);
    put_gtimeval(g, &ru.ru_utime);
    put_gtimeval(g + 8, &ru.ru_stime);
    const long *f = &ru.ru_maxrss;
    for (int i = 0; i < 14; i++) gwrite32(g + 16 + 4 * (uint32_t)i, (uint32_t)f[i]);
    RET(0);
}

// sysconf: long results are 32-bit; page-based values use the guest's 4 KB pages.
THUNK(sysconf) {
    int name = ARGI(0);
    errno = 0;
    long v = sysconf(name);
    if (name == _SC_PAGESIZE) v = 4096;
    else if (name == _SC_PHYS_PAGES && v > 0) {
        uint64_t bytes = (uint64_t)v * (uint64_t)getpagesize();
        v = (long)(bytes / 4096);
    }
    if (v == -1 && errno) set_gerrno(errno);
    if (v > INT32_MAX) v = INT32_MAX;
    RET((uint32_t)v);
}
THUNK(getdtablesize) { RET(getdtablesize()); }
THUNK(getloadavg) { RET(getloadavg(ARGP(0), ARGI(1))); }

THUNK(uname) {
    struct utsname u;
    int rc = uname(&u);
    if (rc) {
        set_gerrno(errno);
        RET(rc);
        return;
    }
    strlcpy(u.machine, "i386", sizeof u.machine);
    memcpy(ARGP(0), &u, sizeof u); // 5 x char[256]: identical
    RET(0);
}

// gethostuuid(uuid_t id, const struct timespec *wait)
THUNK(gethostuuid) {
    struct timespec ts = {0, 0};
    if (ARG(1)) get_gtimespec(ARG(1), &ts);
    RET_ERRNO(gethostuuid(ARGP(0), &ts));
}

// --- mach ---------------------------------------------------------------------------------------
static void init_task_self(gaddr_t a) { gwrite32(a, mach_task_self()); }
GDATA(mach_task_self_, 4, init_task_self);
THUNK(mach_task_self) { RET(mach_task_self()); }
THUNK(mach_host_self) { RET(mach_host_self()); }

// Port names handed to the guest may be synthetic; releasing a reference the
// guest does not own could break host state, so deallocation is a no-op.
THUNK(mach_port_deallocate) { RET(KERN_SUCCESS); }

// vm_deallocate(task, vm_address_t addr, vm_size_t size): 32-bit address and size.
THUNK(vm_deallocate) {
    if (ARG(2)) libc_munmap_tracked(ARG(1), ARG(2));
    RET(KERN_SUCCESS);
}

// vm_read_overwrite(task, addr, size, data, vm_size_t *outsize)
THUNK(vm_read_overwrite) {
    gaddr_t src = ARG(1), dst = ARG(3);
    uint32_t size = ARG(2);
    if ((uint64_t)src + size > 0x100000000ULL || (uint64_t)dst + size > 0x100000000ULL ||
        !gmem_is_mapped(src, size) || !gmem_is_mapped(dst, size)) {
        RET(KERN_INVALID_ADDRESS);
        return;
    }
    // Let the kernel do the copy: it fails cleanly on unreadable (e.g. guard) pages.
    vm_size_t out = 0;
    kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)g2h(src), size, (vm_address_t)g2h(dst), &out);
    if (kr == KERN_SUCCESS && ARG(4)) gwrite32(ARG(4), (uint32_t)out);
    RET(kr);
}
