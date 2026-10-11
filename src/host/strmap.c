#include "strmap.h"

#include <stdlib.h>
#include <string.h>

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ULL;
    return h;
}

void strmap_init(struct strmap *m) {
    m->cap = 1024;
    m->count = 0;
    m->slots = calloc(m->cap, sizeof *m->slots);
}

static void grow(struct strmap *m) {
    struct strmap old = *m;
    m->cap *= 2;
    m->count = 0;
    m->slots = calloc(m->cap, sizeof *m->slots);
    for (size_t i = 0; i < old.cap; i++) {
        if (!old.slots[i].key) continue;
        size_t j = hash_str(old.slots[i].key) & (m->cap - 1);
        while (m->slots[j].key) j = (j + 1) & (m->cap - 1);
        m->slots[j] = old.slots[i];
        m->count++;
    }
    free(old.slots);
}

bool strmap_put(struct strmap *m, const char *key, uint64_t value, bool replace) {
    if ((m->count + 1) * 2 > m->cap) grow(m);
    size_t j = hash_str(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (strcmp(m->slots[j].key, key) == 0) {
            if (replace) m->slots[j].value = value;
            return false;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].key = strdup(key);
    m->slots[j].value = value;
    m->count++;
    return true;
}

bool strmap_get(const struct strmap *m, const char *key, uint64_t *value) {
    if (!m->slots) return false;
    size_t j = hash_str(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (strcmp(m->slots[j].key, key) == 0) {
            if (value) *value = m->slots[j].value;
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
    return false;
}
