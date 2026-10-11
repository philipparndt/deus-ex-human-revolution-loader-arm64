// OpenGL / CGL smoke test: offscreen core-profile context, FBO rendering with
// a GLSL program, VBO uploads via glBufferData and glMapBufferRange, indexed
// draw with an element-buffer offset, glReadPixels and string queries.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static void check_gl(const char *where) {
    GLenum e = glGetError();
    CHECK(e == GL_NO_ERROR, "GL error %#x at %s", e, where);
}

static const char *vs_src =
    "#version 150 core\n"
    "in vec2 pos;\n"
    "in vec4 col;\n"
    "out vec4 vcol;\n"
    "void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
// Split in two strings to exercise the pointer array in glShaderSource.
static const char *fs_src[2] = {
    "#version 150 core\n"
    "in vec4 vcol;\n",
    "uniform vec4 tint;\n"
    "out vec4 frag;\n"
    "void main() { frag = vcol * tint; }\n",
};

static GLuint compile(GLenum type, int n, const char *const *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof log, &len, log);
        printf("shader log (%d): %s\n", len, log);
    }
    CHECK(ok, "shader compile");
    GLint srclen = 0;
    glGetShaderiv(s, GL_SHADER_SOURCE_LENGTH, &srclen);
    char *back = malloc(srclen + 1);
    glGetShaderSource(s, srclen + 1, NULL, back);
    CHECK(strstr(back, "void main()") != NULL, "glGetShaderSource roundtrip");
    free(back);
    return s;
}

#define W 64
#define H 64

static unsigned pixel(const unsigned char *px, int x, int y) {
    const unsigned char *p = px + 4 * (y * W + x);
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

int main(void) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)54, // kCGLPFAFullScreen: removed, must be dropped
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pix, &npix);
    printf("CGLChoosePixelFormat: err %d pix %p npix %d\n", err, (void *)pix, npix);
    CHECK(err == kCGLNoError && pix, "choose pixel format");
    if (!pix) return 1;
    GLint prof = 0;
    CGLDescribePixelFormat(pix, 0, kCGLPFAOpenGLProfile, &prof);
    printf("pixel format profile %#x\n", prof);

    CGLContextObj ctx = NULL;
    err = CGLCreateContext(pix, NULL, &ctx);
    CHECK(err == kCGLNoError && ctx, "create context (err %d: %s)", err, CGLErrorString(err));
    CGLDestroyPixelFormat(pix);
    if (!ctx) return 1;
    CHECK(CGLSetCurrentContext(ctx) == kCGLNoError, "set current");
    CHECK(CGLGetCurrentContext() == ctx, "get current returns same handle");
    GLint swap = 0;
    CGLSetParameter(ctx, kCGLCPSwapInterval, &swap);
    CHECK(CGLLockContext(ctx) == kCGLNoError, "lock");
    CGLUnlockContext(ctx);
    CHECK(CGLGetShareGroup(ctx) != NULL, "share group");

    CGLRendererInfoObj ri = NULL;
    GLint nrend = 0;
    if (CGLQueryRendererInfo(0xffffffff, &ri, &nrend) == kCGLNoError) {
        for (GLint i = 0; i < nrend; i++) {
            GLint id = 0, mem = 0, accel = 0;
            CGLDescribeRenderer(ri, i, kCGLRPRendererID, &id);
            CGLDescribeRenderer(ri, i, kCGLRPVideoMemoryMegabytes, &mem);
            CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &accel);
            printf("renderer %d: id %#x vram %d MB accel %d\n", i, id, mem, accel);
        }
        CGLDestroyRendererInfo(ri);
    }
    CHECK(nrend > 0, "renderer count");

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\nGLSL %s\n", vendor, renderer, version, glsl);
    CHECK(version && version[0] >= '3', "core profile version");
    CHECK(glGetString(GL_VERSION) == (const GLubyte *)version, "glGetString pointer is stable");
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    CHECK(next > 0, "extension count");
    int found_any = 0;
    for (GLint i = 0; i < next; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
        if (e && strncmp(e, "GL_", 3) == 0) found_any++;
    }
    CHECK(found_any == next, "glGetStringi names");
    CHECK(glGetStringi(GL_EXTENSIONS, 0) == glGetStringi(GL_EXTENSIONS, 0), "glGetStringi stable");
    const char *ext = (const char *)glGetString(GL_EXTENSIONS); // synthesized in core profile
    CHECK(ext && strstr(ext, (const char *)glGetStringi(GL_EXTENSIONS, 0)), "synthesized GL_EXTENSIONS");
    printf("%d extensions, first: %s\n", next, glGetStringi(GL_EXTENSIONS, 0));
    check_gl("queries");

    // Framebuffer
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "fbo complete");
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check_gl("fbo");

    // Program
    GLuint vs = compile(GL_VERTEX_SHADER, 1, &vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, 2, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "col");
    glBindFragDataLocation(prog, 0, "frag");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        printf("link log: %s\n", log);
    }
    CHECK(linked, "link");
    glUseProgram(prog);
    GLint tint = glGetUniformLocation(prog, "tint");
    CHECK(tint >= 0, "uniform location");
    const GLfloat white[4] = {1, 1, 1, 1};
    glUniform4fv(tint, 1, white);
    check_gl("program");

    // Geometry: left half quad (red) via glBufferData, right half quad (green)
    // via glMapBufferRange; indices in an element buffer drawn with an offset.
    GLuint vao, vbo[2], ebo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, vbo);
    const GLfloat left[] = {
        // x, y, r, g, b, a
        -1, -1, 1, 0, 0, 1, 0, -1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, -1, 1, 1, 0, 0, 1,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof left, left, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof left, NULL, GL_DYNAMIC_DRAW);
    GLfloat *m = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof left, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    CHECK(m != NULL, "glMapBufferRange");
    if (m) {
        for (int v = 0; v < 4; v++) {
            m[v * 6 + 0] = left[v * 6 + 0] + 1.0f;
            m[v * 6 + 1] = left[v * 6 + 1];
            m[v * 6 + 2] = 0;
            m[v * 6 + 3] = 1;
            m[v * 6 + 4] = 0;
            m[v * 6 + 5] = 1;
        }
        CHECK(glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE, "glUnmapBuffer");
    }
    // Read back through a READ mapping.
    GLfloat *rd = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof left, GL_MAP_READ_BIT);
    CHECK(rd && rd[0] == 0.0f && rd[3] == 1.0f && rd[6] == 1.0f, "read mapping contents");
    if (rd) glUnmapBuffer(GL_ARRAY_BUFFER);
    // Partial write mapping with explicit flush: change the colour of vertex 0..3 alpha only.
    GLfloat *fl = glMapBufferRange(GL_ARRAY_BUFFER, 2 * sizeof(GLfloat), 4 * sizeof(GLfloat),
                                   GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    CHECK(fl && fl[1] == 1.0f, "write mapping keeps contents");
    if (fl) {
        fl[1] = 1.0f; // g stays 1
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 4 * sizeof(GLfloat));
        glUnmapBuffer(GL_ARRAY_BUFFER);
    }
    check_gl("buffers");

    const GLushort idx[] = {0xdead, 0xbeef, 0, 1, 2, 0, 2, 3};
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo[i]);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void *)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)(2 * sizeof(GLushort)));
    }
    check_gl("draw");

    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(sync != NULL, "fence");
    GLenum wr = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    CHECK(wr == GL_ALREADY_SIGNALED || wr == GL_CONDITION_SATISFIED, "client wait %#x", wr);
    glDeleteSync(sync);

    unsigned char *px = malloc(W * H * 4);
    memset(px, 0x55, W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check_gl("readpixels");
    unsigned l = pixel(px, W / 4, H / 2), r = pixel(px, 3 * W / 4, H / 2);
    printf("left %08x right %08x\n", l, r);
    CHECK(l == 0xff0000ff, "left pixel red, got %08x", l);
    CHECK(r == 0x00ff00ff, "right pixel green, got %08x", r);

    // Readback into a pixel pack buffer (offset argument), then map it.
    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, W * H * 4 + 16, NULL, GL_STREAM_READ);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, (void *)16);
    const unsigned char *pm = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    CHECK(pm != NULL, "map pack buffer");
    if (pm) {
        CHECK(memcmp(pm + 16, px, W * H * 4) == 0, "PBO readback matches");
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    check_gl("pbo");

    // Texture upload from client memory with glTexImage2D, read back via glGetTexImage? (core
    // has no glGetTexImage on GLES but desktop does).
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    unsigned char texel[4 * 4 * 4];
    for (int i = 0; i < 64; i++) texel[i] = (unsigned char)(i * 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    unsigned char texback[64] = {0};
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texback);
    CHECK(memcmp(texel, texback, 64) == 0, "texture roundtrip");
    check_gl("texture");

    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &pbo);
    glDeleteBuffers(2, vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteRenderbuffers(1, &rb);
    glDeleteFramebuffers(1, &fbo);
    glFinish();
    check_gl("cleanup");
    free(px);

    CGLSetCurrentContext(NULL);
    CHECK(CGLGetCurrentContext() == NULL, "no current context");
    CHECK(CGLDestroyContext(ctx) == kCGLNoError, "destroy context");

    printf("gl_basic: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
