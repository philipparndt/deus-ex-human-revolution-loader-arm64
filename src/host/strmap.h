// Minimal string -> uint64 hash map (open addressing). Keys are copied.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct strmap {
    struct strmap_entry { char *key; uint64_t value; } *slots;
    size_t cap, count;
};

void strmap_init(struct strmap *m);
// Returns true if the key was newly inserted. Existing values are kept unless replace.
bool strmap_put(struct strmap *m, const char *key, uint64_t value, bool replace);
bool strmap_get(const struct strmap *m, const char *key, uint64_t *value);
