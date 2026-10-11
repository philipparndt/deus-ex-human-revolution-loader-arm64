// Logging helpers. Level is taken from the DXH_LOG environment variable
// (0 = errors only, 1 = info, 2 = debug, 3 = trace every thunk call).
#pragma once
#include <stdio.h>
#include <stdlib.h>

extern int log_level;

void log_init(void);
void log_write(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGE(...) log_write(0, __VA_ARGS__)
#define LOGI(...) do { if (log_level >= 1) log_write(1, __VA_ARGS__); } while (0)
#define LOGD(...) do { if (log_level >= 2) log_write(2, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (log_level >= 3) log_write(3, __VA_ARGS__); } while (0)

_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
