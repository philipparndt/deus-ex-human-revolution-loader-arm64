// OpenGL thunks that cannot be generated mechanically: pointer results
// (glGetString, glMapBuffer), arrays of guest pointers (glShaderSource, ...),
// GLsync handles and the buffer binding tracking used by gl_gen.c.
#include "gl_internal.h"

#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#include <stdlib.h>

#include "gl_gen.h"

#ifndef GL_BUFFER_FLUSHING_UNMAP_APPLE
#define GL_BUFFER_FLUSHING_UNMAP_APPLE 0x8A13
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1F03
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER 0x8C2A
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif

// --- per-context state ------------------------------------------------------------
__thread CGLContextObj gl_tls_cgl;
__thread struct glctx *gl_tls_ctx;
__thread unsigned gl_tls_gen;
volatile unsigned gl_ctx_gen;

static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;
static struct glctx *ctx_list;
static struct glctx null_ctx; // GL calls without a current context (no-ops in the driver)

int gl_verify;
static void gl_verify_init(void) {
    const char *e = getenv("DXH_GL_VERIFY");
    gl_verify = e && *e && *e != '0';
}

void gl_verify_binding(int which, GLuint tracked) {
    GLenum q = which == GLB_ELEM           ? GL_ELEMENT_ARRAY_BUFFER_BINDING
               : which == GLB_ARRAY        ? GL_ARRAY_BUFFER_BINDING
               : which == GLB_PIXEL_PACK   ? GL_PIXEL_PACK_BUFFER_BINDING
               : which == GLB_PIXEL_UNPACK ? GL_PIXEL_UNPACK_BUFFER_BINDING
                                           : 0;
    if (!q || !CGLGetCurrentContext()) return;
    GLint real = 0;
    glGetIntegerv(q, &real);
    if ((GLuint)real != tracked) LOGE("gl: binding tracking mismatch for %#x: tracked %u, driver %d", q, tracked, real);
    else LOGT("gl: binding %#x = %u verified", q, tracked);
}

struct glctx *gl_ctx_lookup(CGLContextObj c) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, gl_verify_init);
    if (!c) return &null_ctx;
    pthread_mutex_lock(&ctx_lock);
    struct glctx *s;
    for (s = ctx_list; s; s = s->next)
        if (s->cgl == c) break;
    if (!s) {
        s = calloc(1, sizeof *s);
        s->cgl = c;
        s->next = ctx_list;
        ctx_list = s;
        LOGD("gl: tracking context %p", (void *)c);
    }
    pthread_mutex_unlock(&ctx_lock);
    return s;
}

void gl_ctx_forget(CGLContextObj c) {
    if (!c) return;
    pthread_mutex_lock(&ctx_lock);
    for (struct glctx **pp = &ctx_list; *pp; pp = &(*pp)->next) {
        struct glctx *s = *pp;
        if (s->cgl != c) continue;
        *pp = s->next;
        for (int i = 0; i < 16; i++)
            if (s->maps[i].buf) gheap_free(s->maps[i].guest);
        // Cached strings stay in guest memory: the guest may still hold them.
        free(s->vao_ebo);
        free(s->buf_noflush);
        free(s->strs);
        free(s);
        break;
    }
    __atomic_add_fetch(&gl_ctx_gen, 1, __ATOMIC_SEQ_CST); // invalidates every thread's cache
    pthread_mutex_unlock(&ctx_lock);
}

static int bind_index(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return GLB_ARRAY;
    case GL_PIXEL_PACK_BUFFER: return GLB_PIXEL_PACK;
    case GL_PIXEL_UNPACK_BUFFER: return GLB_PIXEL_UNPACK;
    case GL_COPY_READ_BUFFER: return GLB_COPY_READ;
    case GL_COPY_WRITE_BUFFER: return GLB_COPY_WRITE;
    case GL_UNIFORM_BUFFER: return GLB_UNIFORM;
    case GL_TEXTURE_BUFFER: return GLB_TEXTURE;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return GLB_TRANSFORM_FEEDBACK;
    case GL_DRAW_INDIRECT_BUFFER: return GLB_DRAW_INDIRECT;
    default: return -1;
    }
}

static void grow_u32(GLuint **arr, uint32_t *cap, uint32_t need) {
    if (need < *cap) return;
    uint32_t n = *cap ? *cap : 64;
    while (n <= need) n *= 2;
    *arr = realloc(*arr, n * sizeof **arr);
    memset(*arr + *cap, 0, (n - *cap) * sizeof **arr);
    *cap = n;
}

// Buffer currently bound to target (0 if none or unknown).
static GLuint bound_buffer(struct glctx *s, GLenum target) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) return gl_elem_binding(s);
    int i = bind_index(target);
    if (i >= 0) return s->bind[i];
    LOGI("gl: untracked buffer target %#x", target);
    return 0;
}

static void track_bind(GLenum target, GLuint buf) {
    struct glctx *s = gl_cur();
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        grow_u32(&s->vao_ebo, &s->vao_ebo_cap, s->cur_vao);
        s->vao_ebo[s->cur_vao] = buf;
        return;
    }
    int i = bind_index(target);
    if (i >= 0) s->bind[i] = buf;
}

THUNK(glBindBuffer) {
    track_bind(ARG(0), ARG(1));
    glBindBuffer(ARG(0), ARG(1));
}
THUNK(glBindBufferARB) {
    track_bind(ARG(0), ARG(1));
    glBindBufferARB(ARG(0), ARG(1));
}
// Indexed binds also replace the generic binding of the target.
THUNK(glBindBufferBase) {
    track_bind(ARG(0), ARG(2));
    glBindBufferBase(ARG(0), ARG(1), ARG(2));
}
THUNK(glBindBufferRange) {
    track_bind(ARG(0), ARG(2));
    glBindBufferRange(ARG(0), ARG(1), ARG(2), ARGIPTR(3), ARGIPTR(4));
}
THUNK(glBindVertexArray) {
    gl_cur()->cur_vao = ARG(0);
    glBindVertexArray(ARG(0));
}

static struct glmap *find_map(struct glctx *s, GLuint buf) {
    if (!buf) return NULL;
    for (int i = 0; i < 16; i++)
        if (s->maps[i].buf == buf) return &s->maps[i];
    return NULL;
}

THUNK(glDeleteBuffers) {
    struct glctx *s = gl_cur();
    GLsizei n = ARGI(0);
    const GLuint *names = ARGP(1);
    for (GLsizei i = 0; i < n && names; i++) {
        GLuint b = names[i];
        if (!b) continue;
        for (int k = 0; k < GLB_COUNT; k++)
            if (s->bind[k] == b) s->bind[k] = 0;
        if (gl_elem_binding(s) == b) s->vao_ebo[s->cur_vao] = 0;
        struct glmap *m = find_map(s, b);
        if (m) { // deleting a mapped buffer unmaps it
            gheap_free(m->guest);
            memset(m, 0, sizeof *m);
        }
        if (b < s->buf_noflush_cap) s->buf_noflush[b] = 0;
    }
    glDeleteBuffers(n, names);
}

THUNK(glDeleteVertexArrays) {
    struct glctx *s = gl_cur();
    GLsizei n = ARGI(0);
    const GLuint *names = ARGP(1);
    for (GLsizei i = 0; i < n && names; i++) {
        GLuint v = names[i];
        if (!v) continue;
        if (v < s->vao_ebo_cap) s->vao_ebo[v] = 0;
        if (s->cur_vao == v) s->cur_vao = 0;
    }
    glDeleteVertexArrays(n, names);
}

// --- buffer mapping through guest shadow memory ------------------------------------
// The driver's mapping lives in host memory outside the guest's 4 GB window, so
// the guest gets a shadow copy: filled from the mapping when the guest may read
// it (or when unwritten bytes must survive), written back on flush / unmap.
static gaddr_t map_shadow(GLenum target, void *host, uint32_t off, uint32_t len, GLbitfield access, bool range) {
    struct glctx *s = gl_cur();
    GLuint buf = bound_buffer(s, target);
    if (!host) return 0;
    if (!buf) {
        // Should not happen (mapping requires a bound buffer); keep it usable.
        GLint b = 0;
        GLenum q = target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING
                   : target == GL_ARRAY_BUFFER       ? GL_ARRAY_BUFFER_BINDING
                                                     : 0;
        if (q) glGetIntegerv(q, &b);
        buf = (GLuint)b;
        if (!buf) {
            LOGE("gl: map of target %#x without known buffer binding", target);
            return 0;
        }
    }
    struct glmap *m = find_map(s, buf);
    if (m) {
        LOGI("gl: buffer %u mapped twice", buf);
        gheap_free(m->guest);
    } else {
        for (int i = 0; i < 16 && !m; i++)
            if (!s->maps[i].buf) m = &s->maps[i];
        if (!m) fatal("gl: too many simultaneously mapped buffers");
    }
    gaddr_t g = gheap_memalign(64, len ? len : 1);
    if (!g) fatal("gl: out of guest memory for a %u byte buffer mapping", len);
    bool invalidate = access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if ((access & GL_MAP_READ_BIT) || !invalidate) memcpy(g2h(g), host, len);
    *m = (struct glmap){buf, host, g, off, len, access, range};
    LOGT("gl: map buffer %u [%u+%u] access %#x -> guest %#x", buf, off, len, access, g);
    return g;
}

static GLbitfield legacy_access(GLenum access) {
    switch (access) {
    case GL_READ_ONLY: return GL_MAP_READ_BIT;
    case GL_WRITE_ONLY: return GL_MAP_WRITE_BIT;
    default: return GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    }
}

THUNK(glMapBuffer) {
    GLenum target = ARG(0), access = ARG(1);
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    void *host = glMapBuffer(target, access);
    RET(map_shadow(target, host, 0, (uint32_t)size, legacy_access(access), false));
}

THUNK(glMapBufferRange) {
    GLenum target = ARG(0);
    GLintptr off = ARGIPTR(1);
    GLsizeiptr len = ARGIPTR(2);
    GLbitfield access = ARG(3);
    void *host = glMapBufferRange(target, off, len, access);
    RET(map_shadow(target, host, (uint32_t)off, (uint32_t)len, access, true));
}

static void flush_range(struct glmap *m, uint32_t off, uint32_t len) {
    if (off > m->len || len > m->len - off) {
        LOGE("gl: flush range %u+%u outside mapping of %u bytes", off, len, m->len);
        return;
    }
    memcpy((uint8_t *)m->host + off, (uint8_t *)g2h(m->guest) + off, len);
}

THUNK(glFlushMappedBufferRange) {
    GLenum target = ARG(0);
    struct glctx *s = gl_cur();
    struct glmap *m = find_map(s, bound_buffer(s, target));
    if (m) flush_range(m, (uint32_t)ARGIPTR(1), (uint32_t)ARGIPTR(2));
    glFlushMappedBufferRange(target, ARGIPTR(1), ARGIPTR(2));
}

THUNK(glFlushMappedBufferRangeAPPLE) {
    // APPLE_flush_buffer_range: offset is relative to the start of the buffer.
    GLenum target = ARG(0);
    struct glctx *s = gl_cur();
    struct glmap *m = find_map(s, bound_buffer(s, target));
    if (m) flush_range(m, (uint32_t)ARGIPTR(1) - m->off, (uint32_t)ARGIPTR(2));
    glFlushMappedBufferRangeAPPLE(target, ARGIPTR(1), ARGIPTR(2));
}

THUNK(glBufferParameteriAPPLE) {
    GLenum target = ARG(0), pname = ARG(1);
    GLint param = ARGI(2);
    if (pname == GL_BUFFER_FLUSHING_UNMAP_APPLE) {
        struct glctx *s = gl_cur();
        GLuint b = bound_buffer(s, target);
        if (b) {
            if (b >= s->buf_noflush_cap) {
                uint32_t n = s->buf_noflush_cap ? s->buf_noflush_cap : 256;
                while (n <= b) n *= 2;
                s->buf_noflush = realloc(s->buf_noflush, n);
                memset(s->buf_noflush + s->buf_noflush_cap, 0, n - s->buf_noflush_cap);
                s->buf_noflush_cap = n;
            }
            s->buf_noflush[b] = !param;
        }
    }
    glBufferParameteriAPPLE(target, pname, param);
}

THUNK(glUnmapBuffer) {
    GLenum target = ARG(0);
    struct glctx *s = gl_cur();
    GLuint b = bound_buffer(s, target);
    struct glmap *m = find_map(s, b);
    if (m) {
        if (m->access & GL_MAP_WRITE_BIT) {
            bool explicit_flush = m->range ? (m->access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0
                                           : (b < s->buf_noflush_cap && s->buf_noflush[b]);
            if (!explicit_flush) memcpy(m->host, g2h(m->guest), m->len);
        }
        gheap_free(m->guest);
        memset(m, 0, sizeof *m);
    } else {
        LOGI("gl: glUnmapBuffer(%#x) without a tracked mapping", target);
    }
    RET(glUnmapBuffer(target));
}

THUNK(glGetBufferPointerv) {
    // Only GL_BUFFER_MAP_POINTER exists: report the guest shadow.
    GLenum target = ARG(0);
    struct glctx *s = gl_cur();
    struct glmap *m = find_map(s, bound_buffer(s, target));
    if (ARG(2)) gwrite32(ARG(2), m ? m->guest : 0);
}

THUNK(glGetPointerv) {
    void *p = NULL;
    glGetPointerv(ARG(0), &p);
    // Pointers we passed in were g2h()-translated; offsets were passed raw.
    uint32_t v = host_ptr_in_guest(p) ? h2g(p) : (uint32_t)(uintptr_t)p;
    if (ARG(1)) gwrite32(ARG(1), v);
}

// --- strings ---------------------------------------------------------------------
static gaddr_t cache_string(GLenum name, GLuint index, const char *str) {
    struct glctx *s = gl_cur();
    for (uint32_t i = 0; i < s->nstrs; i++)
        if (s->strs[i].name == name && s->strs[i].index == index) return s->strs[i].g;
    if (!str) return 0;
    if (s->nstrs == s->strs_cap) {
        s->strs_cap = s->strs_cap ? s->strs_cap * 2 : 64;
        s->strs = realloc(s->strs, s->strs_cap * sizeof *s->strs);
    }
    gaddr_t g = gheap_strdup(str);
    s->strs[s->nstrs++] = (struct glstr){name, index, g};
    return g;
}

THUNK(glGetString) {
    GLenum name = ARG(0);
    struct glctx *s = gl_cur();
    for (uint32_t i = 0; i < s->nstrs; i++)
        if (s->strs[i].name == name && s->strs[i].index == ~0u) {
            RET(s->strs[i].g);
            return;
        }
    const char *str = (const char *)glGetString(name);
    if (!str && name == GL_EXTENSIONS) {
        // Core profiles have no extension string; older engines still parse it.
        GLint n = 0;
        glGetError();
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        size_t cap = 1, len = 0;
        for (GLint i = 0; i < n; i++) cap += strlen((const char *)glGetStringi(GL_EXTENSIONS, i)) + 1;
        char *buf = malloc(cap);
        buf[0] = 0;
        for (GLint i = 0; i < n; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
            size_t l = strlen(e);
            memcpy(buf + len, e, l);
            len += l;
            buf[len++] = ' ';
            buf[len] = 0;
        }
        LOGI("gl: synthesized GL_EXTENSIONS string from %d core-profile extensions", n);
        gaddr_t cached = cache_string(name, ~0u, buf);
        free(buf);
        RET(cached);
    }
    if (str) LOGD("gl: glGetString(%#x) = \"%s\"", name, str);
    RET(cache_string(name, ~0u, str));
}

THUNK(glGetStringi) {
    GLenum name = ARG(0);
    GLuint index = ARG(1);
    struct glctx *s = gl_cur();
    for (uint32_t i = 0; i < s->nstrs; i++)
        if (s->strs[i].name == name && s->strs[i].index == index) {
            RET(s->strs[i].g);
            return;
        }
    RET(cache_string(name, index, (const char *)glGetStringi(name, index)));
}

// --- arrays of guest pointers -------------------------------------------------------
// Builds a host array of n translated guest pointers (heap-allocated if large).
#define PTR_ARRAY(var, garr, n)                                                                    \
    const void *var##_stack[16];                                                                   \
    const void **var = (n) <= 16 ? var##_stack : malloc((size_t)(n) * sizeof(void *));            \
    for (GLsizei i_ = 0; i_ < (n); i_++) var[i_] = g2h(gread32((garr) + 4 * (uint32_t)i_))
#define PTR_ARRAY_FREE(var)                                                                        \
    if (var != var##_stack) free(var)

THUNK(glShaderSource) {
    GLuint shader = ARG(0);
    GLsizei count = ARGI(1);
    gaddr_t strings = ARG(2);
    const GLint *lengths = ARGP(3);
    if (count < 0 || !strings) {
        glShaderSource(shader, count, NULL, lengths);
        return;
    }
    PTR_ARRAY(src, strings, count);
    if (log_level >= 2) {
        for (GLsizei i = 0; i < count; i++) {
            int l = lengths && lengths[i] >= 0 ? lengths[i] : (int)strlen(src[i]);
            LOGD("gl: shader %u source[%d]:\n%.*s", shader, i, l, (const char *)src[i]);
        }
    }
    glShaderSource(shader, count, (const GLchar *const *)src, lengths);
    PTR_ARRAY_FREE(src);
}

THUNK(glTransformFeedbackVaryings) {
    GLsizei count = ARGI(1);
    if (count < 0) count = 0;
    PTR_ARRAY(names, ARG(2), count);
    glTransformFeedbackVaryings(ARG(0), ARGI(1), (const GLchar *const *)names, ARG(3));
    PTR_ARRAY_FREE(names);
}

THUNK(glGetUniformIndices) {
    GLsizei count = ARGI(1);
    if (count < 0) count = 0;
    PTR_ARRAY(names, ARG(2), count);
    glGetUniformIndices(ARG(0), ARGI(1), (const GLchar *const *)names, ARGP(3));
    PTR_ARRAY_FREE(names);
}

THUNK(glMultiDrawElements) {
    // indices: array of guest pointers, or of offsets with an element buffer.
    GLsizei n = ARGI(4);
    if (n < 0) n = 0;
    gaddr_t arr = ARG(3);
    bool offsets = gl_elem_binding(gl_cur()) != 0;
    const void *stack[16];
    const void **idx = n <= 16 ? stack : malloc((size_t)n * sizeof(void *));
    for (GLsizei i = 0; i < n; i++) {
        uint32_t v = gread32(arr + 4 * (uint32_t)i);
        idx[i] = offsets ? (const void *)(uintptr_t)v : g2h(v);
    }
    glMultiDrawElements(ARG(0), ARGP(1), ARG(2), (const GLvoid *const *)idx, ARGI(4));
    if (idx != stack) free(idx);
}

// --- sync objects --------------------------------------------------------------------
// GLsync is a host pointer; the guest gets a small reusable integer handle.
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static GLsync *syncs; // index = handle - 1; free slots are NULL
static uint32_t nsyncs, syncs_cap, sync_hint;

static uint32_t sync_wrap(GLsync s) {
    if (!s) return 0;
    pthread_mutex_lock(&sync_lock);
    uint32_t i;
    for (i = sync_hint; i < nsyncs && syncs[i]; i++) {
    }
    if (i == nsyncs) {
        if (nsyncs == syncs_cap) {
            syncs_cap = syncs_cap ? syncs_cap * 2 : 64;
            syncs = realloc(syncs, syncs_cap * sizeof *syncs);
        }
        nsyncs++;
    }
    syncs[i] = s;
    sync_hint = i + 1;
    pthread_mutex_unlock(&sync_lock);
    return i + 1;
}

static GLsync sync_get(uint32_t h) {
    pthread_mutex_lock(&sync_lock);
    GLsync s = h && h <= nsyncs ? syncs[h - 1] : NULL;
    pthread_mutex_unlock(&sync_lock);
    if (!s && h) LOGI("gl: invalid GLsync handle %u", h);
    return s;
}

THUNK(glFenceSync) { RET(sync_wrap(glFenceSync(ARG(0), ARG(1)))); }
THUNK(glIsSync) { RET(sync_get(ARG(0)) ? glIsSync(sync_get(ARG(0))) : GL_FALSE); }
THUNK(glClientWaitSync) { RET(glClientWaitSync(sync_get(ARG(0)), ARG(1), ARG64(2))); }
THUNK(glWaitSync) { glWaitSync(sync_get(ARG(0)), ARG(1), ARG64(2)); }
THUNK(glGetSynciv) { glGetSynciv(sync_get(ARG(0)), ARG(1), ARGI(2), ARGP(3), ARGP(4)); }
THUNK(glDeleteSync) {
    uint32_t h = ARG(0);
    GLsync s = sync_get(h);
    if (!s) return;
    glDeleteSync(s);
    pthread_mutex_lock(&sync_lock);
    syncs[h - 1] = NULL;
    if (h - 1 < sync_hint) sync_hint = h - 1;
    pthread_mutex_unlock(&sync_lock);
}
