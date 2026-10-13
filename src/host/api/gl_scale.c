// Render scale (DXH_RENDER_SCALE=2): the game keeps its logical resolution
// (e.g. 1920x1080: HUD, text and mouse at their designed size) while the 3D
// image is rendered at N times the pixels. Render targets of exactly the
// logical size (allocated without data) and the default framebuffer get N
// times the storage; viewport, scissor, blits, copies and readbacks are scaled
// for framebuffers that use them. Post-processing offsets the game computes
// for the logical size become finer, which mostly softens those effects.
#define GL_SILENCE_DEPRECATION 1
#include "gl_internal.h"

#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>

#include "gl_gen.h"

// ARB_fragment_program (legacy headers only)
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_MAX_PROGRAM_ENV_PARAMETERS_ARB 0x88B5
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B
#define GL_PROGRAM_ERROR_STRING_ARB 0x8874

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int S = -1;                // scale factor, 1 = off
static GLint logical_w, logical_h; // set from the surface backing size

static int scale(void) {
    if (S < 0) {
        const char *e = getenv("DXH_RENDER_SCALE");
        S = e && atoi(e) > 1 ? atoi(e) : 1;
        if (S > 1) LOGI("gl: render scale %d", S);
    }
    return S;
}

// --- shared (share group) state: scaled textures and renderbuffers by name -----------------
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *tex_scaled, *rb_scaled;
static uint32_t tex_cap, rb_cap;

static void mark(uint8_t **arr, uint32_t *cap, GLuint n, bool on) {
    pthread_mutex_lock(&names_lock);
    if (n >= *cap) {
        uint32_t c = *cap ? *cap : 1024;
        while (c <= n) c *= 2;
        *arr = realloc(*arr, c);
        memset(*arr + *cap, 0, c - *cap);
        *cap = c;
    }
    (*arr)[n] = on;
    pthread_mutex_unlock(&names_lock);
}
static bool marked(uint8_t *arr, uint32_t cap, GLuint n) { return n && n < cap && arr[n]; }

static GLuint binding(GLenum q) {
    GLint v = 0;
    glGetIntegerv(q, &v);
    return (GLuint)v;
}
static GLuint tex_binding(GLenum target) {
    switch (target) {
    case GL_TEXTURE_2D: return binding(GL_TEXTURE_BINDING_2D);
    case GL_TEXTURE_RECTANGLE: return binding(GL_TEXTURE_BINDING_RECTANGLE);
    case GL_TEXTURE_2D_MULTISAMPLE: return binding(GL_TEXTURE_BINDING_2D_MULTISAMPLE);
    default: return 0;
    }
}
static bool is_logical(GLsizei w, GLsizei h) { return logical_w && w == logical_w && h == logical_h; }

// --- per-context framebuffer state ----------------------------------------------------------
// Per FBO: which attachments are present and scaled. Feral's D3D layer keeps
// one FBO bound and swaps its attachments (color, depth, stencil) all the
// time, so the FBO's scale must follow the current color attachment (or the
// depth attachment when there is no color), not the last attachment call.
#define A_COLOR 1
#define A_COLOR_SCALED 2
#define A_DEPTH 4
#define A_DEPTH_SCALED 8
static uint8_t attach_bits(uint8_t b) {
    if (b & A_COLOR) return (b & A_COLOR_SCALED) ? 1 : 0;
    if (b & A_DEPTH) return (b & A_DEPTH_SCALED) ? 1 : 0;
    return 0;
}

static void fbo_grow(struct glctx *s, GLuint fbo) {
    if (fbo >= s->fbo_scaled_cap) {
        uint32_t c = s->fbo_scaled_cap ? s->fbo_scaled_cap : 256;
        while (c <= fbo) c *= 2;
        s->fbo_scaled = realloc(s->fbo_scaled, c);
        memset(s->fbo_scaled + s->fbo_scaled_cap, 0, c - s->fbo_scaled_cap);
        s->fbo_scaled_cap = c;
    }
}
static bool fbo_scaled(struct glctx *s, GLuint fbo) {
    if (!fbo) return logical_w != 0; // default framebuffer: scaled backing
    return fbo < s->fbo_scaled_cap && attach_bits(s->fbo_scaled[fbo]);
}
static bool draw_scaled(void) { return scale() > 1 && fbo_scaled(gl_cur(), gl_cur()->draw_fbo); }
static bool read_scaled(void) { return scale() > 1 && fbo_scaled(gl_cur(), gl_cur()->read_fbo); }

static GLuint *attach_fbo(GLenum target) {
    struct glctx *s = gl_cur();
    return target == GL_READ_FRAMEBUFFER ? &s->read_fbo : &s->draw_fbo;
}

// --- fragment.position in fragment programs -------------------------------------------------
// Screen-space passes (depth of field, object highlight, ...) compute texture
// coordinates from the pixel position (D3D VPOS -> fragment.position) times
// the game's 1 / logical size. On a scaled target that runs up to N and reads
// clamped edge texels (streaks). Fragment programs using it are rewritten to
// read fragment.position * program.env[fp_env_index()] instead, which holds
// 1/N while a scaled target is bound and 1 otherwise.
static GLuint fp_env_index(void) {
    static GLint n;
    if (!n) {
        glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_MAX_PROGRAM_ENV_PARAMETERS_ARB, &n);
        if (n <= 16) n = 256;
        LOGI("gl-scale: fragment.position scale in program.env[%d]", n - 1);
    }
    return (GLuint)n - 1;
}

static void fp_env(struct glctx *s) {
    int k = draw_scaled() ? S : 1;
    if (s->fp_env_k == k) return;
    float f = 1.0f / (float)k;
    const GLfloat v[4] = {f, f, 1.0f, 1.0f};
    glProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, fp_env_index(), v);
    s->fp_env_k = k;
}

static bool is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Returns a malloc'd rewritten program, or NULL when it does not use fragment.position.
static char *rewrite_fp(const char *src, size_t len, size_t *out_len) {
    static const char key[] = "fragment.position";
    const size_t kl = sizeof key - 1;
    bool found = false;
    for (size_t i = 0; i + kl <= len && !found; i++) found = !memcmp(src + i, key, kl);
    if (!found || len < 10 || memcmp(src, "!!ARBfp1.0", 10)) return NULL;
    // insertion point: after the header line and any leading OPTION statements
    size_t p = 10;
    for (;;) {
        size_t q = p;
        for (;;) { // skip blanks and # comments
            while (q < len && (src[q] == ' ' || src[q] == '\t' || src[q] == '\n' || src[q] == '\r')) q++;
            if (q < len && src[q] == '#') {
                while (q < len && src[q] != '\n') q++;
                continue;
            }
            break;
        }
        if (q + 6 <= len && !memcmp(src + q, "OPTION", 6) && (q + 6 == len || !is_ident(src[q + 6]))) {
            while (q < len && src[q] != ';') q++;
            p = q < len ? q + 1 : len;
            continue;
        }
        break;
    }
    char head[160];
    int hl = snprintf(head, sizeof head,
                      "\nPARAM dxh_rs = program.env[%u];\nTEMP dxh_fp;\nMUL dxh_fp, fragment.position, dxh_rs;\n",
                      fp_env_index());
    char *out = malloc(len * 2 + (size_t)hl + 64);
    size_t o = 0;
    memcpy(out, src, p), o = p;
    memcpy(out + o, head, (size_t)hl), o += (size_t)hl;
    for (size_t i = p; i < len;) {
        // "ATTRIB name = fragment.position;" -> "ALIAS name = dxh_fp;"
        if (i + 6 <= len && !memcmp(src + i, "ATTRIB", 6) && (i == 0 || !is_ident(src[i - 1])) &&
            (i + 6 < len && !is_ident(src[i + 6]))) {
            size_t e = i;
            while (e < len && src[e] != ';') e++;
            size_t f = i;
            bool pos = false;
            for (; f + kl <= e; f++)
                if (!memcmp(src + f, key, kl) && (f + kl == e || !is_ident(src[f + kl]) ) && src[f + kl] != '.') {
                    pos = true;
                    break;
                }
            if (pos) {
                memcpy(out + o, "ALIAS ", 6), o += 6;
                size_t n0 = i + 6;
                memcpy(out + o, src + n0, f - n0), o += f - n0; // " name = "
                memcpy(out + o, "dxh_fp", 6), o += 6;
                i = f + kl;
                continue;
            }
        }
        if (i + kl <= len && !memcmp(src + i, key, kl) && (i == 0 || !is_ident(src[i - 1]))) {
            memcpy(out + o, "dxh_fp", 6), o += 6;
            i += kl;
            continue;
        }
        out[o++] = src[i++];
    }
    *out_len = o;
    return out;
}

THUNK(glProgramStringARB) {
    GLenum target = ARG(0), format = ARG(1);
    GLsizei len = (GLsizei)ARG(2);
    const char *src = ARGP(3);
    if (scale() > 1 && target == GL_FRAGMENT_PROGRAM_ARB && format == GL_PROGRAM_FORMAT_ASCII_ARB && src && len > 0) {
        size_t nl;
        char *nw = rewrite_fp(src, (size_t)len, &nl);
        if (nw) {
            static int count;
            if (++count <= 3 || getenv("DXH_RENDER_SCALE_DEBUG"))
                LOGI("gl-scale: fragment program uses fragment.position, rewritten (#%d):\n%.*s", count, (int)nl, nw);
            glProgramStringARB(target, format, (GLsizei)nl, nw);
            // Checking the result waits for the compile (the multithreaded GL
            // engine compiles asynchronously while the game keeps loading), so
            // only with DXH_RENDER_SCALE_DEBUG.
            GLint err = -1;
            if (getenv("DXH_RENDER_SCALE_DEBUG")) glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &err);
            if (err != -1) {
                LOGE("gl-scale: rewritten program rejected at %d (%s); using the original", err,
                     (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB));
                glProgramStringARB(target, format, len, src);
            }
            free(nw);
            fp_env(gl_cur());
            return;
        }
    }
    glProgramStringARB(target, format, len, src);
}

// Viewport and scissor are context state, not framebuffer state: the game sets
// them and then switches between scaled and unscaled targets. Keep the logical
// values and re-apply them whenever the draw target's scale changes.
static void apply_rects(struct glctx *s, bool force) {
    bool sc_ = draw_scaled();
    if (s->fp_env_k) fp_env(s);
    if (!force && sc_ == s->applied_scaled) return;
    int k = sc_ ? S : 1;
    LOGT("gl-scale: fbo %u -> rects x%d (vp %dx%d)", s->draw_fbo, k, s->vp[2], s->vp[3]);
    if (s->vp_set) glViewport(s->vp[0] * k, s->vp[1] * k, s->vp[2] * k, s->vp[3] * k);
    if (s->sc_set) glScissor(s->sc[0] * k, s->sc[1] * k, s->sc[2] * k, s->sc[3] * k);
    s->applied_scaled = sc_;
}

// --- surface ------------------------------------------------------------------------------------
void gl_scale_backing(GLint *wh) {
    if (scale() <= 1) return;
    logical_w = wh[0];
    logical_h = wh[1];
    wh[0] *= S;
    wh[1] *= S;
    LOGI("gl: render scale: logical %dx%d, rendering %dx%d", logical_w, logical_h, wh[0], wh[1]);
}
void gl_scale_backing_back(GLint *wh) {
    if (scale() <= 1 || !logical_w) return;
    wh[0] /= S;
    wh[1] /= S;
}

// --- allocations ------------------------------------------------------------------------------
THUNK(glTexImage2D) {
    GLenum target = ARG(0);
    GLint level = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4);
    const void *pixels = GLPTR_UNPACK(ARG(8));
    if (scale() > 1 && (target == GL_TEXTURE_2D || target == GL_TEXTURE_RECTANGLE) && !pixels &&
        !binding(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
        GLuint t = tex_binding(target);
        if (level == 0 && is_logical(w, h)) {
            LOGI("gl-scale: texture %u %dx%d format %#x scaled", t, w, h, ARG(2));
            mark(&tex_scaled, &tex_cap, t, true);
        } else if (level == 0) {
            mark(&tex_scaled, &tex_cap, t, false);
        }
        if (marked(tex_scaled, tex_cap, t)) {
            w *= S;
            h *= S;
        }
    } else if (scale() > 1 && level == 0) {
        mark(&tex_scaled, &tex_cap, tex_binding(target), false); // image data: real size
    }
    glTexImage2D(target, level, (GLint)ARG(2), w, h, (GLint)ARG(5), ARG(6), ARG(7), pixels);
}

THUNK(glTexStorage2D) {
    GLenum target = ARG(0);
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4);
    if (scale() > 1) {
        GLuint t = tex_binding(target);
        bool on = is_logical(w, h);
        mark(&tex_scaled, &tex_cap, t, on);
        if (on) w *= S, h *= S;
    }
    glTexStorage2D(target, (GLsizei)ARG(1), ARG(2), w, h);
}

THUNK(glTexImage2DMultisample) {
    GLenum target = ARG(0);
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4);
    if (scale() > 1) {
        GLuint t = tex_binding(target);
        bool on = is_logical(w, h);
        mark(&tex_scaled, &tex_cap, t, on);
        if (on) w *= S, h *= S;
    }
    glTexImage2DMultisample(target, (GLsizei)ARG(1), ARG(2), w, h, (GLboolean)ARG(5));
}

static void rb_storage(GLsizei *w, GLsizei *h) {
    if (scale() <= 1) return;
    GLuint rb = binding(GL_RENDERBUFFER_BINDING);
    bool on = is_logical(*w, *h);
    mark(&rb_scaled, &rb_cap, rb, on);
    if (on) *w *= S, *h *= S;
}
THUNK(glRenderbufferStorage) {
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    rb_storage(&w, &h);
    glRenderbufferStorage(ARG(0), ARG(1), w, h);
}
THUNK(glRenderbufferStorageEXT) {
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    rb_storage(&w, &h);
    glRenderbufferStorageEXT(ARG(0), ARG(1), w, h);
}
THUNK(glRenderbufferStorageMultisample) {
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4);
    rb_storage(&w, &h);
    glRenderbufferStorageMultisample(ARG(0), (GLsizei)ARG(1), ARG(2), w, h);
}
THUNK(glRenderbufferStorageMultisampleEXT) {
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4);
    rb_storage(&w, &h);
    glRenderbufferStorageMultisampleEXT(ARG(0), (GLsizei)ARG(1), ARG(2), w, h);
}

// glTexSubImage2D (gl_special.c) into a scaled texture: the texture holds image
// data after all; give it its real size back.
void gl_scale_tex_upload(GLenum target) {
    if (scale() <= 1 || target != GL_TEXTURE_2D) return;
    GLuint t = tex_binding(target);
    if (!marked(tex_scaled, tex_cap, t)) return;
    GLint fmt = 0;
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    mark(&tex_scaled, &tex_cap, t, false);
    glTexImage2D(target, 0, fmt, logical_w, logical_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    LOGI("gl: render scale: texture %u gets image data, back to %dx%d", t, logical_w, logical_h);
}

// --- framebuffers -----------------------------------------------------------------------------
THUNK(glBindFramebuffer) {
    GLenum target = ARG(0);
    GLuint fb = ARG(1);
    struct glctx *s = gl_cur();
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) s->draw_fbo = fb;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s->read_fbo = fb;
    glBindFramebuffer(target, fb);
    if (scale() > 1 && target != GL_READ_FRAMEBUFFER) apply_rects(s, false);
}

// Diagnostics: a framebuffer with scaled and unscaled attachments is
// incomplete (its draws fail). Logged once per framebuffer.
static void check_mixed(GLenum target) {
    GLenum st = glCheckFramebufferStatus(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER);
    if (st == GL_FRAMEBUFFER_COMPLETE) return;
    static uint8_t logged[4096];
    GLuint fbo = *attach_fbo(target);
    if (fbo < sizeof logged && logged[fbo]) return;
    if (fbo < sizeof logged) logged[fbo] = 1;
    GLint w[2] = {0}, h[2] = {0}, type[2] = {0}, name[2] = {0};
    GLenum att[2] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT};
    GLenum t = target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
    for (int i = 0; i < 2; i++) {
        glGetFramebufferAttachmentParameteriv(t, att[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type[i]);
        glGetFramebufferAttachmentParameteriv(t, att[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name[i]);
    }
    LOGI("gl: render scale: framebuffer %u incomplete (%#x): color %s %d, depth %s %d", fbo, st,
         type[0] == GL_TEXTURE ? "tex" : type[0] == GL_RENDERBUFFER ? "rb" : "-", name[0],
         type[1] == GL_TEXTURE ? "tex" : type[1] == GL_RENDERBUFFER ? "rb" : "-", name[1]);
    (void)w;
    (void)h;
}

static void attached(GLenum target, GLenum attachment, GLuint obj, bool scaled_obj) {
    if (scale() <= 1) return;
    struct glctx *s = gl_cur();
    GLuint fbo = *attach_fbo(target);
    if (!fbo) return;
    fbo_grow(s, fbo);
    uint8_t b = s->fbo_scaled[fbo];
    if (attachment == GL_COLOR_ATTACHMENT0) {
        b &= ~(A_COLOR | A_COLOR_SCALED);
        if (obj) b |= A_COLOR | (scaled_obj ? A_COLOR_SCALED : 0);
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        b &= ~(A_DEPTH | A_DEPTH_SCALED);
        if (obj) b |= A_DEPTH | (scaled_obj ? A_DEPTH_SCALED : 0);
    } else {
        return; // other color attachments / stencil: same size as the ones above
    }
    s->fbo_scaled[fbo] = b;
    LOGT("gl-scale: fbo %u attach %#x obj %u scaled %d -> bits %#x (fbo scaled %d)", fbo, attachment, obj, scaled_obj,
         b, attach_bits(b));
    if (target != GL_READ_FRAMEBUFFER) apply_rects(s, false);
}
THUNK(glFramebufferTexture2D) {
    attached(ARG(0), ARG(1), ARG(3), marked(tex_scaled, tex_cap, ARG(3)));
    glFramebufferTexture2D(ARG(0), ARG(1), ARG(2), ARG(3), (GLint)ARG(4));
    if (scale() > 1 && getenv("DXH_RENDER_SCALE_DEBUG")) check_mixed(ARG(0));
}
THUNK(glFramebufferTexture) {
    attached(ARG(0), ARG(1), ARG(2), marked(tex_scaled, tex_cap, ARG(2)));
    glFramebufferTexture(ARG(0), ARG(1), ARG(2), (GLint)ARG(3));
    if (scale() > 1 && getenv("DXH_RENDER_SCALE_DEBUG")) check_mixed(ARG(0));
}
THUNK(glFramebufferTextureLayer) {
    attached(ARG(0), ARG(1), ARG(2), marked(tex_scaled, tex_cap, ARG(2)));
    glFramebufferTextureLayer(ARG(0), ARG(1), ARG(2), (GLint)ARG(3), (GLint)ARG(4));
    if (scale() > 1 && getenv("DXH_RENDER_SCALE_DEBUG")) check_mixed(ARG(0));
}
THUNK(glFramebufferRenderbuffer) {
    attached(ARG(0), ARG(1), ARG(3), marked(rb_scaled, rb_cap, ARG(3)));
    glFramebufferRenderbuffer(ARG(0), ARG(1), ARG(2), ARG(3));
    if (scale() > 1 && getenv("DXH_RENDER_SCALE_DEBUG")) check_mixed(ARG(0));
}

static void forget(uint8_t **arr, uint32_t *cap, GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n && names; i++)
        if (names[i] < *cap) mark(arr, cap, names[i], false);
}
THUNK(glDeleteTextures) {
    if (scale() > 1) forget(&tex_scaled, &tex_cap, ARGI(0), ARGP(1));
    glDeleteTextures(ARGI(0), ARGP(1));
}
THUNK(glDeleteRenderbuffers) {
    if (scale() > 1) forget(&rb_scaled, &rb_cap, ARGI(0), ARGP(1));
    glDeleteRenderbuffers(ARGI(0), ARGP(1));
}
THUNK(glDeleteRenderbuffersEXT) {
    if (scale() > 1) forget(&rb_scaled, &rb_cap, ARGI(0), ARGP(1));
    glDeleteRenderbuffersEXT(ARGI(0), ARGP(1));
}
THUNK(glDeleteFramebuffers) {
    struct glctx *s = gl_cur();
    const GLuint *names = ARGP(1);
    for (GLsizei i = 0; i < ARGI(0) && names; i++) {
        if (names[i] < s->fbo_scaled_cap) s->fbo_scaled[names[i]] = 0;
        if (s->draw_fbo == names[i]) s->draw_fbo = 0;
        if (s->read_fbo == names[i]) s->read_fbo = 0;
    }
    glDeleteFramebuffers(ARGI(0), names);
}

// --- coordinates ------------------------------------------------------------------------------
THUNK(glViewport) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (scale() > 1) {
        struct glctx *s = gl_cur();
        s->vp[0] = x, s->vp[1] = y, s->vp[2] = w, s->vp[3] = h;
        s->vp_set = true;
        s->applied_scaled = draw_scaled();
        if (s->fp_env_k) fp_env(s);
        LOGT("gl-scale: viewport %dx%d on fbo %u x%d", w, h, s->draw_fbo, s->applied_scaled ? S : 1);
        if (s->applied_scaled) x *= S, y *= S, w *= S, h *= S;
    }
    glViewport(x, y, w, h);
}
THUNK(glScissor) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    if (scale() > 1) {
        struct glctx *s = gl_cur();
        s->sc[0] = x, s->sc[1] = y, s->sc[2] = w, s->sc[3] = h;
        s->sc_set = true;
        bool k = draw_scaled();
        if (k != s->applied_scaled && s->vp_set) apply_rects(s, true); // keep both consistent
        if (k) x *= S, y *= S, w *= S, h *= S;
    }
    glScissor(x, y, w, h);
}
THUNK(glBlitFramebuffer) {
    GLint v[8];
    for (int i = 0; i < 8; i++) v[i] = (GLint)ARG(i);
    LOGT("gl-scale: blit read fbo %u x%d -> draw fbo %u x%d", gl_cur()->read_fbo, read_scaled() ? S : 1,
         gl_cur()->draw_fbo, draw_scaled() ? S : 1);
    if (read_scaled())
        for (int i = 0; i < 4; i++) v[i] *= S;
    if (draw_scaled())
        for (int i = 4; i < 8; i++) v[i] *= S;
    glBlitFramebuffer(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], ARG(8), ARG(9));
}
THUNK(glGetIntegerv) {
    GLenum pname = ARG(0);
    GLint *out = ARGP(1);
    glGetIntegerv(pname, out);
    if (!out || scale() <= 1) return;
    // The logical values the game set (the host's depend on which target was
    // bound when they were applied); Scaleform saves and restores the viewport.
    struct glctx *s = gl_cur();
    if (pname == GL_VIEWPORT && s->vp_set) memcpy(out, s->vp, sizeof s->vp);
    else if (pname == GL_SCISSOR_BOX && s->sc_set) memcpy(out, s->sc, sizeof s->sc);
    else if ((pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) && draw_scaled())
        for (int i = 0; i < 4; i++) out[i] /= S;
}

// Copy a scaled read-framebuffer region into a texture of logical size: blit
// (downscale) through a temporary framebuffer.
static void blit_into_texture(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w,
                              GLsizei h) {
    struct glctx *s = gl_cur();
    if (!s->scale_tmp_fbo) glGenFramebuffers(1, &s->scale_tmp_fbo);
    GLuint tex = tex_binding(target);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); // blits are scissored
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->scale_tmp_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, tex, level);
    glBlitFramebuffer(x * S, y * S, (x + w) * S, (y + h) * S, xoff, yoff, xoff + w, yoff + h, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->draw_fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

THUNK(glCopyTexSubImage2D) {
    GLenum target = ARG(0);
    GLint level = (GLint)ARG(1), xoff = (GLint)ARG(2), yoff = (GLint)ARG(3), x = (GLint)ARG(4), y = (GLint)ARG(5);
    GLsizei w = (GLsizei)ARG(6), h = (GLsizei)ARG(7);
    if (read_scaled()) {
        if (marked(tex_scaled, tex_cap, tex_binding(target))) {
            glCopyTexSubImage2D(target, level, xoff * S, yoff * S, x * S, y * S, w * S, h * S);
        } else {
            blit_into_texture(target, level, xoff, yoff, x, y, w, h);
        }
        return;
    }
    glCopyTexSubImage2D(target, level, xoff, yoff, x, y, w, h);
}
THUNK(glCopyTexImage2D) {
    GLenum target = ARG(0), ifmt = ARG(2);
    GLint level = (GLint)ARG(1), x = (GLint)ARG(3), y = (GLint)ARG(4);
    GLsizei w = (GLsizei)ARG(5), h = (GLsizei)ARG(6);
    if (read_scaled()) {
        GLuint t = tex_binding(target);
        if (level == 0 && is_logical(w, h)) {
            mark(&tex_scaled, &tex_cap, t, true);
            glCopyTexImage2D(target, level, ifmt, x * S, y * S, w * S, h * S, 0);
        } else {
            glTexImage2D(target, level, (GLint)ifmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            blit_into_texture(target, level, 0, 0, x, y, w, h);
        }
        return;
    }
    glCopyTexImage2D(target, level, ifmt, x, y, w, h, (GLint)ARG(7));
}

// Readback from a scaled framebuffer: read N times the pixels, keep every Nth.
THUNK(glReadPixels) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    GLenum format = ARG(4), type = ARG(5);
    void *pixels = GLPTR_PACK(ARG(6));
    int bpp = 0;
    if ((format == GL_RGBA || format == GL_BGRA) &&
        (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_INT_8_8_8_8))
        bpp = 4;
    else if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT)
        bpp = 4;
    if (!read_scaled() || !pixels || !bpp || binding(GL_PIXEL_PACK_BUFFER_BINDING)) {
        if (read_scaled()) LOGI("gl: render scale: unscaled readback (format %#x type %#x)", format, type);
        glReadPixels(x, y, w, h, format, type, pixels);
        return;
    }
    uint8_t *big = malloc((size_t)w * S * h * S * bpp);
    if (!big) return;
    GLint align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x * S, y * S, w * S, h * S, format, type, big);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    uint8_t *dst = pixels;
    for (GLsizei r = 0; r < h; r++)
        for (GLsizei c = 0; c < w; c++)
            memcpy(dst + ((size_t)r * w + c) * bpp, big + (((size_t)r * S) * w * S + (size_t)c * S) * bpp, bpp);
    free(big);
}
