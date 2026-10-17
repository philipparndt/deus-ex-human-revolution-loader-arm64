// DXH_SPIKES=<ms>: where did a slow frame's time go?
//
// Every thread that runs guest code keeps a stack of states: guest code, or the
// host call (thunk id) it is in. Host calls push on entry and pop on return;
// guest code called from the host (cpu_call, e.g. callbacks inside a host call)
// pushes "guest" again, so time is exclusive: a host call is charged only for
// its own host time, nested guest code counts as guest time. Each state change
// charges the time since the previous one to the state that ends:
//   - per thread to one of four buckets (guest, busy in a host call, waiting
//     in a host call, outside of guest code), readable from other threads
//     through a sequence counter;
//   - on the frame thread (the one calling CGLFlushDrawable, "WinMain" in HR)
//     also per thunk id, reset every frame.
// At every presented frame the frame thread snapshots all threads; when the
// frame took longer than the threshold it logs a few "spike:" lines: its own
// guest/busy/wait split and top host calls, its longest single call (and for a
// condition wait, which thread signalled it), the busiest other threads, and
// counts of notable calls (texture/buffer uploads, shader compiles, GPU syncs,
// file access, inflate jobs, main-queue work) from all threads.
//
// Cost when off: one predictable branch per host call and guest call. When on:
// two mach_absolute_time() reads and a few stores per host call.
#include "spikes.h"
#include "log.h"
#include "thunk.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SPK_IDS 65536u              // thunk ids (MAX_THUNKS in thunk.c)
#define SPK_GUEST SPK_IDS           // state: guest code
#define SPK_OUT (SPK_IDS + 1)       // state: host code outside of any guest call
#define SPK_STACK 64
#define SPK_TOP_CALLS 6
#define SPK_TOP_THREADS 6
#define SPK_MAX_PER_SEC 10          // reports per second (loading screens are slow throughout)

enum { B_GUEST, B_BUSY, B_WAIT, B_OUT, NBUCKETS };
static const char *const bucket_names[] = {"g", "b", "w", "o"};

// Per-thunk classification, computed on first use: bit 0 known, bit 1 waits,
// bits 2-4 event class, bits 5-6 condition variable role.
#define CAT_KNOWN 1u
#define CAT_WAIT 2u
#define CAT_EV(c) (((c) >> 2) & 7u)
#define CAT_CONDWAIT (1u << 5)
#define CAT_SIGNAL (2u << 5)
#define CAT_COND_MASK (3u << 5)
enum { EV_NONE, EV_TEX, EV_BUF, EV_SHADER, EV_SYNC, EV_FILE, EV_INFLATE, NEV };
static const char *const ev_names[] = {"", "tex", "buf", "shader", "gpusync", "file", "inflate"};
static uint8_t cats[SPK_IDS];

bool spikes_on;
static uint64_t threshold_ticks;
static double tick_ms; // mach_absolute_time ticks -> ms
static uint32_t mainq_id;

struct spk_thread {
    // Written by the owner inside seq (odd while writing), read by the frame thread.
    _Atomic uint32_t seq;
    _Atomic uint64_t cum[NBUCKETS];
    _Atomic uint64_t last;      // time of the last state change
    _Atomic uint32_t cur_id;    // current state
    _Atomic uint32_t cur_bucket;
    _Atomic uint64_t cur_since; // when the current state was entered
    _Atomic uint64_t maxcall;   // longest non-waiting host call: gen << 48 | id << 32 | microseconds
    // Owner only.
    int depth, overflow;
    struct {
        uint32_t id;
        uint8_t bucket, cat;
        uint64_t t;
    } st[SPK_STACK];
    // Frame thread only.
    uint64_t prev[NBUCKETS];
    // List (all_lock).
    pthread_t self;
    bool is_main;
    _Atomic bool dead;
    int index;
    struct spk_thread *next;
};

static _Thread_local struct spk_thread *me;
static struct spk_thread *all_threads;
static int nthreads_seen;
static pthread_mutex_t all_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t exit_key;

static struct spk_thread *_Atomic frame_thread;
static _Atomic uint32_t gen;                      // frame window number (maxcall validity)
static _Atomic uint32_t watched_cond;             // condition the frame thread waits on
static struct spk_thread *_Atomic cond_signaller; // last thread that signalled it

// Frame thread: exclusive time and calls per thunk id in the current frame.
static uint64_t fr_ticks[SPK_IDS + 2];
static uint32_t fr_calls[SPK_IDS + 2];
static uint8_t fr_seen[SPK_IDS + 2];
static uint32_t fr_touched[SPK_IDS + 2], fr_ntouched;
static uint32_t fr_long_id;
static uint64_t fr_long_ticks;
static struct spk_thread *fr_long_woke;

// Events from all threads in the current frame.
static _Atomic uint64_t ev_calls[NEV], ev_ticks[NEV];
static _Atomic uint64_t mq_items, mq_ticks, mq_maxwait;

uint64_t spikes_now(void) { return mach_absolute_time(); }

static const char *short_name(uint32_t id) {
    if (id == SPK_GUEST) return "(guest)";
    if (id == SPK_OUT) return "(outside)";
    const char *n = thunk_name(id);
    return n[0] == '_' ? n + 1 : n;
}

static bool name_in(const char *n, const char *const *list) {
    for (; *list; list++)
        if (strcmp(n, *list) == 0) return true;
    return false;
}

static uint8_t classify(uint32_t id) {
    const char *full = thunk_name(id), *n = full[0] == '_' ? full + 1 : full;
    char base[64]; // without a $UNIX2003-style suffix
    strlcpy(base, n, sizeof base);
    char *dollar = strchr(base, '$');
    if (dollar) *dollar = 0;
    uint8_t c = CAT_KNOWN;
    if (thunk_is_waiting_call(full) || strstr(n, "mutex_lock") || strstr(n, "LockContext")) c |= CAT_WAIT;
    if (strstr(n, "cond_wait") || strstr(n, "cond_timedwait")) c |= CAT_CONDWAIT;
    else if (strstr(n, "cond_signal") || strstr(n, "cond_broadcast")) c |= CAT_SIGNAL;
    static const char *const files[] = {"open", "fopen", "stat", "stat64", "lstat", "lstat64", "access", "read",
                                        "pread", "fread", "opendir", "readdir", "getattrlist", NULL};
    static const char *const syncs[] = {"glFinish", "glFlush", "glClientWaitSync", "glWaitSync", "glReadPixels",
                                        "glGetTexImage", "glFinishFenceAPPLE", "glFinishObjectAPPLE", NULL};
    unsigned ev = EV_NONE;
    if (strstr(n, "TexImage") || strstr(n, "TexSubImage")) ev = EV_TEX;
    else if (strncmp(n, "glBuffer", 8) == 0 || strstr(n, "MapBuffer") || strstr(n, "FlushMapped")) ev = EV_BUF;
    else if (strstr(n, "CompileShader") || strstr(n, "LinkProgram") || strstr(n, "ProgramBinary")) ev = EV_SHADER;
    else if (name_in(base, syncs) || strncmp(n, "glGetQueryObject", 16) == 0) ev = EV_SYNC;
    else if (name_in(base, files) || strncmp(n, "CFURL", 5) == 0 || strncmp(n, "FSPathMakeRef", 13) == 0 ||
             strncmp(n, "FSGetCatalogInfo", 16) == 0)
        ev = EV_FILE;
    else if (strstr(n, "inflate")) ev = EV_INFLATE;
    return (uint8_t)(c | ev << 2);
}

static inline uint8_t cat_of(uint32_t id) {
    uint8_t c = cats[id];
    if (__builtin_expect(!c, 0)) cats[id] = c = classify(id); // benign race: same value
    return c;
}

static void thread_gone(void *p) {
    struct spk_thread *t = p;
    pthread_mutex_lock(&all_lock);
    atomic_store(&t->dead, true);
    pthread_mutex_unlock(&all_lock);
}

static struct spk_thread *self_rec(void) {
    if (__builtin_expect(me != NULL, 1)) return me;
    struct spk_thread *t = calloc(1, sizeof *t);
    uint64_t now = spikes_now();
    t->self = pthread_self();
    t->is_main = pthread_main_np();
    t->st[0].id = SPK_OUT;
    t->st[0].bucket = B_OUT;
    t->st[0].t = now;
    t->depth = 1;
    atomic_store(&t->last, now);
    atomic_store(&t->cur_id, SPK_OUT);
    atomic_store(&t->cur_bucket, B_OUT);
    atomic_store(&t->cur_since, now);
    pthread_mutex_lock(&all_lock);
    t->index = nthreads_seen++;
    t->next = all_threads;
    all_threads = t;
    pthread_mutex_unlock(&all_lock);
    pthread_setspecific(exit_key, t);
    me = t;
    return t;
}

// Charge the time since the last change to the current (top) state, then make
// the top of the stack current. The caller has changed the stack in between.
static inline void seq_begin(struct spk_thread *t) {
    atomic_store_explicit(&t->seq, atomic_load_explicit(&t->seq, memory_order_relaxed) + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}
static inline void seq_end(struct spk_thread *t) {
    atomic_store_explicit(&t->seq, atomic_load_explicit(&t->seq, memory_order_relaxed) + 1, memory_order_release);
}

static inline void charge(struct spk_thread *t, uint64_t now) {
    uint32_t id = atomic_load_explicit(&t->cur_id, memory_order_relaxed);
    uint32_t b = atomic_load_explicit(&t->cur_bucket, memory_order_relaxed);
    uint64_t d = now - atomic_load_explicit(&t->last, memory_order_relaxed);
    atomic_store_explicit(&t->cum[b], atomic_load_explicit(&t->cum[b], memory_order_relaxed) + d,
                          memory_order_relaxed);
    atomic_store_explicit(&t->last, now, memory_order_relaxed);
    if (t == atomic_load_explicit(&frame_thread, memory_order_relaxed)) {
        if (!fr_seen[id]) {
            fr_seen[id] = 1;
            fr_touched[fr_ntouched++] = id;
        }
        fr_ticks[id] += d;
    }
}

static inline void set_current(struct spk_thread *t) {
    int top = t->depth - 1;
    atomic_store_explicit(&t->cur_id, t->st[top].id, memory_order_relaxed);
    atomic_store_explicit(&t->cur_bucket, t->st[top].bucket, memory_order_relaxed);
    atomic_store_explicit(&t->cur_since, t->st[top].t, memory_order_relaxed);
}

static inline void push(struct spk_thread *t, uint32_t id, uint8_t bucket, uint8_t cat) {
    uint64_t now = spikes_now();
    if (t->depth >= SPK_STACK) { // never expected; keep the books balanced
        t->overflow++;
        return;
    }
    seq_begin(t);
    charge(t, now);
    t->st[t->depth].id = id;
    t->st[t->depth].bucket = bucket;
    t->st[t->depth].cat = cat;
    t->st[t->depth].t = now;
    t->depth++;
    set_current(t);
    seq_end(t);
}

void spikes_thunk_enter(uint32_t id, const struct guest_regs *r) {
    struct spk_thread *t = self_rec();
    uint8_t c = cat_of(id);
    if (t == atomic_load_explicit(&frame_thread, memory_order_relaxed)) {
        if (!fr_seen[id]) {
            fr_seen[id] = 1;
            fr_touched[fr_ntouched++] = id;
        }
        fr_calls[id]++;
        if ((c & CAT_COND_MASK) == CAT_CONDWAIT && r) {
            atomic_store_explicit(&cond_signaller, NULL, memory_order_relaxed);
            atomic_store_explicit(&watched_cond, gread32(r->esp + 4), memory_order_relaxed);
        }
    } else if ((c & CAT_COND_MASK) == CAT_SIGNAL && r) {
        uint32_t w = atomic_load_explicit(&watched_cond, memory_order_relaxed);
        if (w && gread32(r->esp + 4) == w) atomic_store_explicit(&cond_signaller, t, memory_order_relaxed);
    }
    push(t, id, (c & CAT_WAIT) ? B_WAIT : B_BUSY, c);
}

void spikes_thunk_exit(void) {
    struct spk_thread *t = self_rec();
    if (t->overflow) {
        t->overflow--;
        return;
    }
    if (t->depth <= 1) return;
    uint64_t now = spikes_now();
    int top = t->depth - 1;
    uint32_t id = t->st[top].id;
    uint8_t c = t->st[top].cat;
    uint64_t dur = now - t->st[top].t; // including nested guest code
    seq_begin(t);
    charge(t, now);
    t->depth--;
    set_current(t);
    seq_end(t);
    if (CAT_EV(c)) {
        atomic_fetch_add_explicit(&ev_calls[CAT_EV(c)], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&ev_ticks[CAT_EV(c)], dur, memory_order_relaxed);
    }
    if (t == atomic_load_explicit(&frame_thread, memory_order_relaxed)) {
        struct spk_thread *woke = NULL;
        if ((c & CAT_COND_MASK) == CAT_CONDWAIT) {
            woke = atomic_load_explicit(&cond_signaller, memory_order_relaxed);
            atomic_store_explicit(&watched_cond, 0, memory_order_relaxed);
        }
        if (dur > fr_long_ticks) {
            fr_long_ticks = dur;
            fr_long_id = id;
            fr_long_woke = woke;
        }
    } else if (!(c & CAT_WAIT)) {
        uint64_t g = atomic_load_explicit(&gen, memory_order_relaxed) & 0xffff;
        uint64_t us = (uint64_t)(dur * tick_ms * 1000);
        if (us > 0xffffffffu) us = 0xffffffffu;
        uint64_t m = atomic_load_explicit(&t->maxcall, memory_order_relaxed);
        if (m >> 48 != g || us > (m & 0xffffffffu))
            atomic_store_explicit(&t->maxcall, g << 48 | (uint64_t)id << 32 | us, memory_order_relaxed);
    }
}

uint32_t spikes_guest_enter(void) {
    struct spk_thread *t = self_rec();
    uint32_t saved = (uint32_t)t->depth << 16 | (uint32_t)t->overflow;
    push(t, SPK_GUEST, B_GUEST, 0);
    return saved;
}

void spikes_guest_exit(uint32_t saved) {
    struct spk_thread *t = self_rec();
    seq_begin(t);
    charge(t, spikes_now());
    t->depth = (int)(saved >> 16);
    t->overflow = (int)(saved & 0xffff);
    set_current(t);
    seq_end(t);
}

// Items can nest (a block may drain the queue); their time then counts twice
// in the "mainq" total, not in the per-thread buckets.
uint64_t spikes_mainq_begin(uint64_t queued) {
    uint64_t now = spikes_now();
    if (queued) {
        uint64_t w = now - queued, m = atomic_load_explicit(&mq_maxwait, memory_order_relaxed);
        while (w > m && !atomic_compare_exchange_weak(&mq_maxwait, &m, w)) {
        }
    }
    spikes_thunk_enter(mainq_id, NULL);
    return now;
}

void spikes_mainq_end(uint64_t begin) {
    spikes_thunk_exit();
    atomic_fetch_add_explicit(&mq_items, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&mq_ticks, spikes_now() - begin, memory_order_relaxed);
}

static void mainq_marker(struct guest_regs *r) { fatal("guest called the main-queue accounting marker (%#x)", r->esp); }

void spikes_init(void) {
    const char *e = getenv("DXH_SPIKES");
    if (!e || atof(e) <= 0) return;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    tick_ms = (double)tb.numer / tb.denom / 1e6;
    threshold_ticks = (uint64_t)(atof(e) / tick_ms);
    pthread_key_create(&exit_key, thread_gone);
    mainq_id = thunk_register_marker("_(main queue work)", mainq_marker);
    spikes_on = true;
    LOGI("spikes: logging frames slower than %.1f ms", atof(e));
}

// --- frame end ------------------------------------------------------------------------------------

// Consistent bucket totals of a thread up to now.
static void snapshot(struct spk_thread *t, uint64_t now, uint64_t out[NBUCKETS], uint32_t *cur_id, uint64_t *since) {
    for (int tries = 0;; tries++) {
        if (tries > 64) sched_yield(); // the writer was preempted mid-update
        uint32_t s1 = atomic_load_explicit(&t->seq, memory_order_acquire);
        if (s1 & 1) continue;
        for (int b = 0; b < NBUCKETS; b++) out[b] = atomic_load_explicit(&t->cum[b], memory_order_relaxed);
        uint64_t last = atomic_load_explicit(&t->last, memory_order_relaxed);
        uint32_t b = atomic_load_explicit(&t->cur_bucket, memory_order_relaxed);
        *cur_id = atomic_load_explicit(&t->cur_id, memory_order_relaxed);
        *since = atomic_load_explicit(&t->cur_since, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&t->seq, memory_order_relaxed) != s1) continue;
        if (now > last && b < NBUCKETS) out[b] += now - last;
        return;
    }
}

struct appender {
    char buf[1024];
    size_t n;
};
__attribute__((format(printf, 2, 3))) static void app(struct appender *a, const char *fmt, ...) {
    if (a->n >= sizeof a->buf) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(a->buf + a->n, sizeof a->buf - a->n, fmt, ap);
    va_end(ap);
    if (k > 0) a->n += (size_t)k;
}

static void thread_label(struct spk_thread *t, char *out, size_t n) { // all_lock held, t alive
    out[0] = 0;
    if (t->is_main) strlcpy(out, "main", n);
    else pthread_getname_np(t->self, out, n);
    if (!out[0]) snprintf(out, n, "t%d", t->index);
}

struct thread_row {
    struct spk_thread *t;
    uint64_t d[NBUCKETS], since;
    uint32_t cur_id;
    char name[24];
};

static int cmp_rows(const void *a, const void *b) {
    const struct thread_row *x = a, *y = b;
    uint64_t bx = x->d[B_GUEST] + x->d[B_BUSY], by = y->d[B_GUEST] + y->d[B_BUSY];
    return bx < by ? 1 : bx > by ? -1 : 0;
}

static int cmp_ids(const void *a, const void *b) {
    uint64_t x = fr_ticks[*(const uint32_t *)a], y = fr_ticks[*(const uint32_t *)b];
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(struct spk_thread *self, uint64_t now, uint64_t frame_ticks, uint64_t number,
                   struct thread_row *rows, int nrows, unsigned suppressed) {
    uint64_t own[NBUCKETS] = {0};
    char self_name[24] = "?", woke_name[24] = "";
    for (int i = 0; i < nrows; i++) {
        if (rows[i].t == self) {
            memcpy(own, rows[i].d, sizeof own);
            strlcpy(self_name, rows[i].name, sizeof self_name);
        }
        if (fr_long_woke && rows[i].t == fr_long_woke) strlcpy(woke_name, rows[i].name, sizeof woke_name);
    }
    struct appender a = {.n = 0};
    app(&a, "spike: frame %llu took %.1f ms: %s guest %.1f, host %.1f, waiting %.1f ms", (unsigned long long)number,
        frame_ticks * tick_ms, self_name, own[B_GUEST] * tick_ms, own[B_BUSY] * tick_ms, own[B_WAIT] * tick_ms);
    if (fr_long_ticks) {
        app(&a, "; longest call %s %.1f ms", short_name(fr_long_id), fr_long_ticks * tick_ms);
        if (fr_long_woke) app(&a, " (signalled by %s)", woke_name[0] ? woke_name : "an exited thread");
    }
    if (suppressed) app(&a, " [%u spikes not logged]", suppressed);
    LOGI("%s", a.buf);

    // The frame thread's host calls by exclusive time.
    a.n = 0;
    qsort(fr_touched, fr_ntouched, sizeof *fr_touched, cmp_ids);
    app(&a, "spike:   %s calls:", self_name);
    int shown = 0;
    for (uint32_t i = 0; i < fr_ntouched && shown < SPK_TOP_CALLS; i++) {
        uint32_t id = fr_touched[i];
        if (id >= SPK_IDS || !fr_ticks[id]) continue;
        app(&a, "%s %s %.1f ms/%u", shown ? "," : "", short_name(id), fr_ticks[id] * tick_ms, fr_calls[id]);
        shown++;
    }
    LOGI("%s", a.buf);

    // Other threads, busiest (guest + host) first.
    a.n = 0;
    app(&a, "spike:   threads (g guest, b host, w waiting ms):");
    shown = 0;
    uint32_t g = atomic_load_explicit(&gen, memory_order_relaxed) & 0xffff;
    for (int i = 0; i < nrows && shown < SPK_TOP_THREADS; i++) {
        struct thread_row *row = &rows[i];
        if (row->t == self || row->d[B_GUEST] + row->d[B_BUSY] < frame_ticks / 50) continue;
        app(&a, "%s %s", shown ? ";" : "", row->name);
        for (int b = 0; b < B_OUT; b++)
            if (row->d[b] * tick_ms >= 0.1) app(&a, " %s%.1f", bucket_names[b], row->d[b] * tick_ms);
        uint64_t m = atomic_load_explicit(&row->t->maxcall, memory_order_relaxed);
        if (m >> 48 == g && (m & 0xffffffffu) >= 1000)
            app(&a, " max %s %.1f", short_name((uint32_t)(m >> 32) & 0xffff), (m & 0xffffffffu) / 1000.0);
        if (row->cur_id < SPK_IDS && now - row->since >= frame_ticks / 4)
            app(&a, " [in %s %.0f]", short_name(row->cur_id), (now - row->since) * tick_ms);
        shown++;
    }
    if (shown) LOGI("%s", a.buf);

    // Notable calls on all threads (time includes nested guest code).
    a.n = 0;
    app(&a, "spike:   events:");
    shown = 0;
    for (int ev = 1; ev < NEV; ev++) {
        uint64_t c = atomic_load_explicit(&ev_calls[ev], memory_order_relaxed);
        if (!c) continue;
        app(&a, " %s %llu/%.1f ms", ev_names[ev], (unsigned long long)c, ev_ticks[ev] * tick_ms);
        shown++;
    }
    uint64_t mqi = atomic_load_explicit(&mq_items, memory_order_relaxed);
    if (mqi) {
        app(&a, " mainq %llu/%.1f ms (max wait %.1f)", (unsigned long long)mqi, mq_ticks * tick_ms,
            mq_maxwait * tick_ms);
        shown++;
    }
    if (shown) LOGI("%s", a.buf);
}

void spikes_frame(void) {
    static uint64_t last_frame, frames, second_start;
    static unsigned in_second, suppressed;
    struct spk_thread *t = self_rec();
    uint64_t now = spikes_now();
    // Charge the flush so far to this frame.
    seq_begin(t);
    charge(t, now);
    seq_end(t);
    // The flush itself is still open: it can be the longest call.
    if (t->depth > 1 && now - t->st[t->depth - 1].t > fr_long_ticks) {
        fr_long_ticks = now - t->st[t->depth - 1].t;
        fr_long_id = t->st[t->depth - 1].id;
        fr_long_woke = NULL;
    }
    bool first = atomic_load_explicit(&frame_thread, memory_order_relaxed) != t;
    static int switches;
    if (first && switches++ < 8) LOGI("spikes: frames are presented by thread %d", t->index);
    uint64_t frame_ticks = now - last_frame;
    bool slow = !first && last_frame && frame_ticks >= threshold_ticks;
    if (slow) {
        if (now - second_start >= (uint64_t)(1000 / tick_ms)) {
            second_start = now;
            in_second = 0;
        }
        if (in_second >= SPK_MAX_PER_SEC) {
            suppressed++;
            slow = false;
        } else {
            in_second++;
        }
    }

    // Snapshot every thread (the window ends now for all of them).
    static struct thread_row *rows;
    static int rows_cap;
    int nrows = 0;
    pthread_mutex_lock(&all_lock);
    if (rows_cap < nthreads_seen) {
        rows_cap = nthreads_seen + 16;
        rows = realloc(rows, sizeof *rows * (size_t)rows_cap);
    }
    for (struct spk_thread *x = all_threads; x; x = x->next) {
        if (atomic_load_explicit(&x->dead, memory_order_relaxed)) continue;
        struct thread_row *row = &rows[nrows++];
        uint64_t s[NBUCKETS];
        snapshot(x, now, s, &row->cur_id, &row->since);
        for (int b = 0; b < NBUCKETS; b++) {
            row->d[b] = s[b] - x->prev[b];
            x->prev[b] = s[b];
        }
        row->t = x;
        if (slow) thread_label(x, row->name, sizeof row->name);
    }
    pthread_mutex_unlock(&all_lock);

    if (slow) {
        qsort(rows, (size_t)nrows, sizeof *rows, cmp_rows);
        report(t, now, frame_ticks, frames, rows, nrows, suppressed);
        suppressed = 0;
    }

    // Next frame.
    for (uint32_t i = 0; i < fr_ntouched; i++) {
        uint32_t id = fr_touched[i];
        fr_ticks[id] = 0;
        fr_calls[id] = 0;
        fr_seen[id] = 0;
    }
    fr_ntouched = 0;
    fr_long_ticks = 0;
    fr_long_woke = NULL;
    for (int ev = 1; ev < NEV; ev++) {
        atomic_store_explicit(&ev_calls[ev], 0, memory_order_relaxed);
        atomic_store_explicit(&ev_ticks[ev], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&mq_items, 0, memory_order_relaxed);
    atomic_store_explicit(&mq_ticks, 0, memory_order_relaxed);
    atomic_store_explicit(&mq_maxwait, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&gen, 1, memory_order_relaxed);
    atomic_store_explicit(&frame_thread, t, memory_order_relaxed);
    last_frame = now;
    frames++;
}
