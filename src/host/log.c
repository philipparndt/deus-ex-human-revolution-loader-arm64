#include "log.h"

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

int log_level = 1;
static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// kill -USR1 <pid>: raise the log level to 3 (trace); kill -USR2: back to the start level.
static int start_level = 1;
static void on_usr1(int sig) { (void)sig; log_level = 3; }
static void on_usr2(int sig) { (void)sig; log_level = start_level; }

void log_init(void) {
    const char *lvl = getenv("DXH_LOG");
    if (lvl) log_level = atoi(lvl);
    start_level = log_level;
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    const char *path = getenv("DXH_LOG_FILE");
    log_file = path ? fopen(path, "w") : NULL;
    if (!log_file) log_file = stderr;
}

static void vlog(int level, const char *fmt, va_list ap) {
    static const char tags[] = "EIDT";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    // Signal handlers may log while this thread already holds the lock.
    bool locked = level != 0 ? (pthread_mutex_lock(&log_lock), true) : pthread_mutex_trylock(&log_lock) == 0;
    fprintf(log_file ? log_file : stderr, "[%5ld.%03ld %c %5llu] ", (long)ts.tv_sec % 100000,
            ts.tv_nsec / 1000000, tags[level & 3], (unsigned long long)tid);
    vfprintf(log_file ? log_file : stderr, fmt, ap);
    fputc('\n', log_file ? log_file : stderr);
    fflush(log_file ? log_file : stderr);
    if (locked) pthread_mutex_unlock(&log_lock);
}

void log_write(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(0, fmt, ap);
    va_end(ap);
    abort();
}
