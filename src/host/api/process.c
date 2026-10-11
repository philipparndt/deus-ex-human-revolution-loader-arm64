// Process lifetime: exit/atexit, abort, ids, environment.
#include "common.h"
#include "../runtime.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct atexit_entry {
    gaddr_t fn, arg, dso;
    bool cxa;
};
static struct atexit_entry *atexit_list;
static size_t natexit, atexit_cap;
static pthread_mutex_t atexit_lock = PTHREAD_MUTEX_INITIALIZER;

static void add_atexit(gaddr_t fn, gaddr_t arg, gaddr_t dso, bool cxa) {
    pthread_mutex_lock(&atexit_lock);
    if (natexit == atexit_cap) {
        atexit_cap = atexit_cap ? atexit_cap * 2 : 256;
        atexit_list = realloc(atexit_list, atexit_cap * sizeof *atexit_list);
    }
    atexit_list[natexit++] = (struct atexit_entry){fn, arg, dso, cxa};
    pthread_mutex_unlock(&atexit_lock);
}

void stdio_flush_all(void); // stdio.c

_Noreturn void runtime_exit(int code) {
    // Run handlers in reverse registration order; handlers may register more.
    for (;;) {
        pthread_mutex_lock(&atexit_lock);
        if (!natexit) {
            pthread_mutex_unlock(&atexit_lock);
            break;
        }
        struct atexit_entry e = atexit_list[--natexit];
        pthread_mutex_unlock(&atexit_lock);
        if (e.cxa) gcall1(e.fn, e.arg);
        else gcall0(e.fn);
    }
    stdio_flush_all();
    LOGI("guest exited with code %d", code);
    exit(code);
}

THUNK(__cxa_atexit) {
    add_atexit(ARG(0), ARG(1), ARG(2), true);
    RET(0);
}
THUNK(atexit) {
    add_atexit(ARG(0), 0, 0, false);
    RET(0);
}
THUNK(exit) { runtime_exit(ARGI(0)); }
THUNK(_exit) {
    stdio_flush_all();
    _exit(ARGI(0));
}
THUNK(_Exit) {
    stdio_flush_all();
    _exit(ARGI(0));
}
THUNK(abort) {
    char buf[256];
    fatal("guest called abort() from %s", macho_describe_addr(gread32(r->esp), buf, sizeof buf));
}

THUNK(__stack_chk_fail) {
    char buf[256];
    fatal("guest stack smashing detected (caller %s)", macho_describe_addr(gread32(r->esp), buf, sizeof buf));
}
static void init_stack_guard(gaddr_t a) { gwrite32(a, arc4random() & ~0xffu); }
GDATA(__stack_chk_guard, 4, init_stack_guard);

THUNK(getpid) { RET(getpid()); }
THUNK(getppid) { RET(getppid()); }
THUNK(getuid) { RET(getuid()); }
THUNK(getgid) { RET(getgid()); }
THUNK(geteuid) { RET(geteuid()); }

THUNK(getprogname) {
    static gaddr_t name;
    if (!name) name = gheap_strdup(getprogname());
    RET(name);
}

// --- environment: kept in guest memory, initialised from the guest envp -------

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static gaddr_t env_find(const char *name, size_t *index) {
    size_t nl = strlen(name);
    gaddr_t envp = runtime_envp();
    for (size_t i = 0;; i++) {
        gaddr_t e = gread32(envp + 4 * (uint32_t)i);
        if (!e) return 0;
        const char *s = g2h(e);
        if (strncmp(s, name, nl) == 0 && s[nl] == '=') {
            if (index) *index = i;
            return e;
        }
    }
}

THUNK(getenv) {
    pthread_mutex_lock(&env_lock);
    gaddr_t e = env_find(ARGS(0), NULL);
    pthread_mutex_unlock(&env_lock);
    RET(e ? e + (uint32_t)strlen(ARGS(0)) + 1 : 0);
}

THUNK(unsetenv) {
    pthread_mutex_lock(&env_lock);
    size_t idx;
    gaddr_t envp = runtime_envp();
    while (env_find(ARGS(0), &idx)) {
        for (size_t j = idx;; j++) {
            gaddr_t next = gread32(envp + 4 * (uint32_t)(j + 1));
            gwrite32(envp + 4 * (uint32_t)j, next);
            if (!next) break;
        }
    }
    pthread_mutex_unlock(&env_lock);
    unsetenv(ARGS(0));
    RET(0);
}

THUNK(setenv) {
    // Not imported by the game; implemented for completeness via host only.
    RET_ERRNO(setenv(ARGS(0), ARGS(1), ARGI(2)));
}

THUNK(system) {
    LOGI("guest system(\"%s\")", ARG(0) ? ARGS(0) : "(null)");
    RET(ARG(0) ? system(ARGS(0)) : 1);
}

THUNK(sleep) { RET(sleep(ARG(0))); }
THUNK(usleep) { RET_ERRNO(usleep(ARG(0))); }
THUNK_AS(usleep_unix2003, "_usleep$UNIX2003") { RET_ERRNO(usleep(ARG(0))); }
THUNK(getpagesize) { RET(4096); } // the guest was built for 4 KB pages

THUNK(raise) {
    LOGI("guest raise(%d)", ARGI(0));
    if (ARGI(0) == SIGABRT || ARGI(0) == SIGTRAP) fatal("guest raised signal %d", ARGI(0));
    RET(0);
}

// Signals: record handlers but do not install them for now.
THUNK(signal) { RET(0); }
THUNK(sigaction) {
    if (ARG(2)) memset(ARGP(2), 0, 16);
    RET(0);
}
THUNK(sigprocmask) {
    if (ARG(2)) gwrite32(ARG(2), 0);
    RET(0);
}
THUNK(sigaltstack) {
    if (ARG(1)) memset(ARGP(1), 0, 12);
    RET(0);
}
THUNK(setrlimit) { RET(0); }
THUNK(strsignal) { RET(guest_static_string(0, strsignal(ARGI(0)))); }

// --- $UNIX2003 symbol variants ---------------------------------------------------------------------
THUNK_AS(setenv_unix2003, "_setenv$UNIX2003") { thunk_impl_setenv(r); }
THUNK_AS(unsetenv_unix2003, "_unsetenv$UNIX2003") { thunk_impl_unsetenv(r); }
THUNK_AS(sleep_unix2003, "_sleep$UNIX2003") { thunk_impl_sleep(r); }
THUNK_AS(system_unix2003, "_system$UNIX2003") { thunk_impl_system(r); }
THUNK_AS(sigaltstack_unix2003, "_sigaltstack$UNIX2003") { thunk_impl_sigaltstack(r); }
