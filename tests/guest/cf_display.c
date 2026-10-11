// CoreGraphics display / geometry / bitmap context test (i386 guest).
#include <ApplicationServices/ApplicationServices.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(CGFloat) == 4, "CGFloat is float on i386");
_Static_assert(sizeof(CGRect) == 16, "CGRect");
_Static_assert(sizeof(CGAffineTransform) == 24, "CGAffineTransform");
_Static_assert(sizeof(ProcessInfoRec) == 60, "ProcessInfoRec");
_Static_assert(offsetof(ProcessInfoRec, processLauncher) == 40, "launcher");
_Static_assert(offsetof(ProcessInfoRec, processAppSpec) == 56, "appSpec");
_Static_assert(sizeof(CGFunctionCallbacks) == 12, "CGFunctionCallbacks");

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static int eval_calls;
static void eval(void *info, const CGFloat *in, CGFloat *out) {
    eval_calls++;
    out[0] = in[0];
    out[1] = 0;
    out[2] = 1 - in[0];
    out[3] = 1;
}

int main(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    CGRect b = CGDisplayBounds(d);
    printf("main display %u bounds %g,%g %gx%g\n", d, b.origin.x, b.origin.y, b.size.width, b.size.height);
    CHECK(d != 0 && b.size.width > 0 && b.size.height > 0);
    printf("pixels %zux%zu\n", CGDisplayPixelsWide(d), CGDisplayPixelsHigh(d));
    CHECK(CGDisplayPixelsWide(d) > 0);
    CGSize mm = CGDisplayScreenSize(d);
    printf("screen size %gx%g mm, GL mask %#x\n", mm.width, mm.height, CGDisplayIDToOpenGLDisplayMask(d));

    uint32_t n = 0;
    CGDirectDisplayID ids[8];
    CHECK(CGGetActiveDisplayList(8, ids, &n) == kCGErrorSuccess && n >= 1);
    printf("%u active displays\n", n);

    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    CHECK(cur != NULL);
    printf("current mode %zux%zu @%g Hz flags %#x\n", CGDisplayModeGetWidth(cur), CGDisplayModeGetHeight(cur),
           CGDisplayModeGetRefreshRate(cur), CGDisplayModeGetIOFlags(cur));
    CFStringRef enc = CGDisplayModeCopyPixelEncoding(cur);
    char ebuf[64] = "";
    CFStringGetCString(enc, ebuf, sizeof ebuf, kCFStringEncodingUTF8);
    CHECK(strcmp(ebuf, IO32BitDirectPixels) == 0);
    CFRelease(enc);
    CGDisplayModeRelease(cur);

    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, NULL);
    CHECK(modes && CFArrayGetCount(modes) > 0);
    printf("%ld modes:", (long)CFArrayGetCount(modes));
    for (CFIndex i = 0; i < CFArrayGetCount(modes) && i < 8; i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        printf(" %zux%zu@%g", CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m), CGDisplayModeGetRefreshRate(m));
    }
    printf("\n");
    CFRelease(modes);

    // Geometry.
    CGRect r = CGRectMake(1.5f, 2.25f, 10.5f, 20.f);
    CHECK(CGRectGetMinX(r) == 1.5f && CGRectGetMaxX(r) == 12.f && CGRectGetMidY(r) == 12.25f);
    CHECK(CGRectGetWidth(r) == 10.5f && CGRectGetHeight(r) == 20.f);
    CGRect ri = CGRectIntegral(r);
    printf("integral %g %g %g %g\n", ri.origin.x, ri.origin.y, ri.size.width, ri.size.height);
    CHECK(ri.origin.x == 1 && ri.origin.y == 2 && ri.size.width == 11 && ri.size.height == 21);
    CHECK(CGRectContainsRect(ri, r) && !CGRectContainsRect(r, ri));
    CHECK(CGRectEqualToRect(r, r) && !CGRectEqualToRect(r, ri));
    CHECK(CGRectEqualToRect(CGRectZero, CGRectMake(0, 0, 0, 0)) && CGSizeZero.width == 0);
    CGAffineTransform t = CGAffineTransformMakeScale(2, 3);
    CHECK(t.a == 2 && t.d == 3 && t.b == 0 && t.tx == 0);
    CHECK(CGAffineTransformIdentity.a == 1 && CGAffineTransformIdentity.d == 1 && CGAffineTransformIdentity.c == 0);

    // Bitmap context with guest-allocated buffer.
    enum { W = 16, H = 8 };
    static uint32_t px[W * H];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CHECK(cs != NULL);
    CGContextRef c = CGBitmapContextCreate(px, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CHECK(c != NULL);
    CGContextSetRGBFillColor(c, 1, 0, 0, 1);
    CGContextFillRect(c, CGRectMake(0, 0, W, H));
    CGContextSetRGBFillColor(c, 0, 0, 1, 1);
    CGContextFillRect(c, CGRectMake(0, 0, W / 2, H)); // left half blue
    const uint8_t *p = (const uint8_t *)px;
    printf("pixel[0] %02x%02x%02x%02x pixel[last] %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3], p[4 * (W * H - 1)],
           p[4 * (W * H - 1) + 1], p[4 * (W * H - 1) + 2], p[4 * (W * H - 1) + 3]);
    CHECK(p[0] == 0 && p[2] == 0xff && p[3] == 0xff);
    CHECK(p[4 * (W * H - 1)] == 0xff && p[4 * (W * H - 1) + 2] == 0);
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, 1, 1);
    CGContextSetLineWidth(c, 1);
    CGContextSetRGBStrokeColor(c, 0, 1, 0, 1);
    CGFloat dash[2] = {2, 1};
    CGContextSetLineDash(c, 0, dash, 2);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, 0, 0);
    CGContextAddLineToPoint(c, 10, 5);
    CGContextStrokePath(c);
    CGContextRestoreGState(c);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CHECK(img && CGImageGetWidth(img) == W && CGImageGetHeight(img) == H);
    CGImageRelease(img);

    // Context with NULL data: buffer allocated by the host layer in guest memory.
    CGContextRef c2 = CGBitmapContextCreate(NULL, 5, 3, 8, 0, cs, kCGImageAlphaPremultipliedFirst);
    CHECK(c2 != NULL);
    CGContextSetRGBFillColor(c2, 0, 1, 0, 1);
    CGContextFillRect(c2, CGRectMake(0, 0, 5, 3));
    CGImageRef img2 = CGBitmapContextCreateImage(c2);
    CHECK(img2 && CGImageGetWidth(img2) == 5);

    // Draw image back into the first context, and a shading via a guest CGFunction.
    CGContextDrawImage(c, CGRectMake(0, 0, 5, 3), img2);
    CHECK(p[4 * W * (H - 1) + 1] == 0xff && p[4 * W * (H - 1)] == 0); // bottom row = last in memory
    CGImageRelease(img2);
    CGContextRelease(c2);

    CGFloat domain[2] = {0, 1}, range[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    CGFunctionCallbacks cb = {0, eval, NULL};
    CGFunctionRef fn = CGFunctionCreate(NULL, 1, domain, 4, range, &cb);
    CHECK(fn != NULL);
    CGShadingRef sh = CGShadingCreateAxial(cs, CGPointMake(0, 0), CGPointMake(W, 0), fn, false, false);
    CHECK(sh != NULL);
    CGContextDrawShading(c, sh);
    printf("shading evaluate calls: %d, pixel[0] %02x%02x%02x\n", eval_calls, p[0], p[1], p[2]);
    CHECK(eval_calls > 0);
    CGShadingRelease(sh);
    CGFunctionRelease(fn);

    CGContextRelease(c);
    CGColorSpaceRelease(cs);

    ProcessSerialNumber psn;
    CHECK(GetCurrentProcess(&psn) == noErr);
    CFStringRef pname = NULL;
    CHECK(CopyProcessName(&psn, &pname) == noErr && pname);
    char nb[256] = "";
    if (pname) CFStringGetCString(pname, nb, sizeof nb, kCFStringEncodingUTF8);
    printf("process name: %s, shielding level %d\n", nb, CGShieldingWindowLevel());
    ProcessInfoRec info;
    Str31 nm;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processName = nm;
    CHECK(GetProcessInformation(&psn, &info) == noErr);
    printf("process info: name %.*s type %.4s\n", nm[0], nm + 1, (char *)&info.processType);

    printf(fails ? "cf_display: %d FAILURES\n" : "cf_display: all OK\n", fails);
    return fails != 0;
}
