// Shared state for the OpenGL / CGL thunks (gl_gen.c, gl_special.c, gl_cgl.c).
//
// GL state that the thunks must know about is tracked per host CGL context:
//   - buffer bindings that turn pointer arguments into buffer *offsets*
//     (ARRAY_BUFFER for gl*Pointer, ELEMENT_ARRAY_BUFFER (per VAO) for
//     glDrawElements*, PIXEL_PACK/UNPACK for texture uploads / readbacks),
//   - guest shadow copies of mapped buffers (glMapBuffer*),
//   - cached guest copies of glGetString(i) results.
//
// The context is looked up from CGLGetCurrentContext(), so it works no matter
// who made it current (CGLSetCurrentContext thunk, NSOpenGLContext bridge, ...).
#pragma once
#include "common.h"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gltypes.h>
#include <pthread.h>

// Buffer binding points tracked by the thunks (index into glctx.bind).
enum {
    GLB_ARRAY,
    GLB_PIXEL_PACK,
    GLB_PIXEL_UNPACK,
    GLB_COPY_READ,
    GLB_COPY_WRITE,
    GLB_UNIFORM,
    GLB_TEXTURE,
    GLB_TRANSFORM_FEEDBACK,
    GLB_DRAW_INDIRECT,
    GLB_COUNT
};

struct glmap {
    GLuint buf;         // buffer name (0 = unused slot)
    void *host;         // real mapping returned by the driver
    gaddr_t guest;      // guest shadow
    uint32_t off, len;  // mapped range within the buffer
    GLbitfield access;  // GL_MAP_*_BIT style flags
    bool range;         // mapped with glMapBufferRange (vs. glMapBuffer)
};

struct glstr {
    GLenum name;
    GLuint index; // glGetStringi index, or ~0u for glGetString
    gaddr_t g;
};

struct glctx {
    CGLContextObj cgl;
    struct glctx *next;
    GLuint bind[GLB_COUNT];
    GLuint cur_vao;
    GLuint *vao_ebo; // element array binding per VAO name
    uint32_t vao_ebo_cap;
    uint8_t *buf_noflush; // per buffer name: GL_BUFFER_FLUSHING_UNMAP_APPLE == FALSE
    uint32_t buf_noflush_cap;
    struct glmap maps[16];
    struct glstr *strs;
    uint32_t nstrs, strs_cap;
};

struct glctx *gl_ctx_lookup(CGLContextObj c); // creates on first use
void gl_ctx_forget(CGLContextObj c);         // call when a context is destroyed

extern __thread CGLContextObj gl_tls_cgl;
extern __thread struct glctx *gl_tls_ctx;
extern __thread unsigned gl_tls_gen;
extern volatile unsigned gl_ctx_gen; // bumped whenever a context is forgotten

static inline struct glctx *gl_cur(void) {
    CGLContextObj c = CGLGetCurrentContext();
    if (__builtin_expect(c != gl_tls_cgl || !gl_tls_ctx || gl_tls_gen != gl_ctx_gen, 0)) {
        gl_tls_gen = gl_ctx_gen;
        gl_tls_ctx = gl_ctx_lookup(c);
        gl_tls_cgl = c;
    }
    return gl_tls_ctx;
}

static inline GLuint gl_elem_binding(struct glctx *s) {
    return s->cur_vao < s->vao_ebo_cap ? s->vao_ebo[s->cur_vao] : 0;
}

// Pointer-or-offset arguments: with a buffer bound to the relevant target the
// guest value is a byte offset and must be passed unchanged.
#define GLB_ELEM GLB_COUNT // pseudo index: element array binding of the current VAO
extern int gl_verify;      // DXH_GL_VERIFY=1: cross-check tracked bindings with the driver
void gl_verify_binding(int which, GLuint tracked);
static inline void *gl_ptr(int which, gaddr_t a) {
    struct glctx *s = gl_cur();
    GLuint b = which == GLB_ELEM ? gl_elem_binding(s) : s->bind[which];
    if (__builtin_expect(gl_verify, 0)) gl_verify_binding(which, b);
    return b ? (void *)(uintptr_t)a : g2h(a);
}
#define GLPTR_ARRAY(a) gl_ptr(GLB_ARRAY, (a))
#define GLPTR_ELEM(a) gl_ptr(GLB_ELEM, (a))
#define GLPTR_UNPACK(a) gl_ptr(GLB_PIXEL_UNPACK, (a))
#define GLPTR_PACK(a) gl_ptr(GLB_PIXEL_PACK, (a))

// GLintptr / GLsizeiptr are 32-bit signed in the guest.
#define ARGIPTR(i) ((intptr_t)(int32_t)ARG(i))
