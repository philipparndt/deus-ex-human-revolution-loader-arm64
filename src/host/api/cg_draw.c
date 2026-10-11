// CoreGraphics drawing: contexts, bitmap contexts, color spaces, images, data
// providers, paths, shadings, functions, geometry helpers, ImageIO.
//
// All CG objects are CF types and use cf_bridge handles. Bitmap context pixel
// buffers always live in guest memory (the host draws into them directly).
#include "cg_internal.h"

#include <ImageIO/ImageIO.h>
#include <pthread.h>
#include <stdlib.h>

#define CTX(i) ((CGContextRef)ARGCF(i))

// --- data symbols ------------------------------------------------------------------------

static void init_rect_zero(gaddr_t a) { put_grect(a, CGRectZero); }
static void init_size_zero(gaddr_t a) { put_gsize(a, CGSizeZero); }
static void init_point_zero(gaddr_t a) { gwritef(a, 0), gwritef(a + 4, 0); }
static void init_rect_null(gaddr_t a) { put_grect(a, CGRectNull); }
static void init_rect_infinite(gaddr_t a) { put_grect(a, CGRectInfinite); }
static void init_identity(gaddr_t a) { put_gxform(a, CGAffineTransformIdentity); }
GDATA(CGRectZero, 16, init_rect_zero);
GDATA(CGRectNull, 16, init_rect_null);
GDATA(CGRectInfinite, 16, init_rect_infinite);
GDATA(CGSizeZero, 8, init_size_zero);
GDATA(CGPointZero, 8, init_point_zero);
GDATA(CGAffineTransformIdentity, 24, init_identity);

#define CF_CONST(sym)                                                                              \
    static void init_##sym(gaddr_t a) { gwrite32(a, cfg(sym)); }                                   \
    GDATA(sym, 4, init_##sym)

CF_CONST(kCGImageDestinationLossyCompressionQuality);
CF_CONST(kCGImagePropertyTIFFCompression);
CF_CONST(kCGImagePropertyTIFFCopyright);
CF_CONST(kCGImagePropertyTIFFDateTime);
CF_CONST(kCGImagePropertyTIFFDictionary);
CF_CONST(kCGImagePropertyTIFFDocumentName);
CF_CONST(kCGImagePropertyTIFFImageDescription);
CF_CONST(kCGImagePropertyTIFFSoftware);
CF_CONST(kCGImagePropertyPixelWidth);
CF_CONST(kCGImagePropertyPixelHeight);
CF_CONST(kCGImageSourceShouldCache);
CF_CONST(kCGColorSpaceGenericRGB);
CF_CONST(kCGColorSpaceSRGB);
CF_CONST(kCGColorSpaceGenericGray);

// --- geometry (pure float math, i386 CGFloat = float) ------------------------------------

THUNK(CGRectGetMinX) { RETD(CGRectGetMinX(ARGRECT(0))); }
THUNK(CGRectGetMidX) { RETD(CGRectGetMidX(ARGRECT(0))); }
THUNK(CGRectGetMaxX) { RETD(CGRectGetMaxX(ARGRECT(0))); }
THUNK(CGRectGetMinY) { RETD(CGRectGetMinY(ARGRECT(0))); }
THUNK(CGRectGetMidY) { RETD(CGRectGetMidY(ARGRECT(0))); }
THUNK(CGRectGetMaxY) { RETD(CGRectGetMaxY(ARGRECT(0))); }
THUNK(CGRectGetWidth) { RETD(CGRectGetWidth(ARGRECT(0))); }
THUNK(CGRectGetHeight) { RETD(CGRectGetHeight(ARGRECT(0))); }
THUNK(CGRectEqualToRect) { RET_BOOL(CGRectEqualToRect(ARGRECT(0), ARGRECT(4))); }
THUNK(CGRectContainsRect) { RET_BOOL(CGRectContainsRect(ARGRECT(0), ARGRECT(4))); }
THUNK(CGRectContainsPoint) { RET_BOOL(CGRectContainsPoint(ARGRECT(0), ARGPOINT(4))); }
THUNK(CGRectIntersectsRect) { RET_BOOL(CGRectIntersectsRect(ARGRECT(0), ARGRECT(4))); }
THUNK(CGRectIsEmpty) { RET_BOOL(CGRectIsEmpty(ARGRECT(0))); }
THUNK(CGRectIsNull) { RET_BOOL(CGRectIsNull(ARGRECT(0))); }
// Struct returns: hidden pointer at ARG(0), rect argument at slots 1..4.
THUNK(CGRectIntegral) { RET_RECT(CGRectIntegral(ARGRECT(1))); }
THUNK(CGRectStandardize) { RET_RECT(CGRectStandardize(ARGRECT(1))); }
THUNK(CGRectInset) { RET_RECT(CGRectInset(ARGRECT(1), ARGF(5), ARGF(6))); }
THUNK(CGRectOffset) { RET_RECT(CGRectOffset(ARGRECT(1), ARGF(5), ARGF(6))); }
THUNK(CGRectUnion) { RET_RECT(CGRectUnion(ARGRECT(1), ARGRECT(5))); }
THUNK(CGRectIntersection) { RET_RECT(CGRectIntersection(ARGRECT(1), ARGRECT(5))); }

THUNK(CGAffineTransformMakeScale) { RET_XFORM(CGAffineTransformMakeScale(ARGF(1), ARGF(2))); }
THUNK(CGAffineTransformMakeTranslation) { RET_XFORM(CGAffineTransformMakeTranslation(ARGF(1), ARGF(2))); }
THUNK(CGAffineTransformMakeRotation) { RET_XFORM(CGAffineTransformMakeRotation(ARGF(1))); }
THUNK(CGAffineTransformMake) {
    RET_XFORM(CGAffineTransformMake(ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGF(6)));
}
THUNK(CGAffineTransformScale) { RET_XFORM(CGAffineTransformScale(ARGXFORM(1), ARGF(7), ARGF(8))); }
THUNK(CGAffineTransformTranslate) { RET_XFORM(CGAffineTransformTranslate(ARGXFORM(1), ARGF(7), ARGF(8))); }
THUNK(CGAffineTransformRotate) { RET_XFORM(CGAffineTransformRotate(ARGXFORM(1), ARGF(7))); }
THUNK(CGAffineTransformConcat) { RET_XFORM(CGAffineTransformConcat(ARGXFORM(1), ARGXFORM(7))); }
THUNK(CGAffineTransformInvert) { RET_XFORM(CGAffineTransformInvert(ARGXFORM(1))); }
THUNK(CGAffineTransformIsIdentity) { RET_BOOL(CGAffineTransformIsIdentity(ARGXFORM(0))); }

// --- color spaces --------------------------------------------------------------------------

THUNK(CGColorSpaceCreateDeviceRGB) { RET_CF(CGColorSpaceCreateDeviceRGB()); }
THUNK(CGColorSpaceCreateDeviceGray) { RET_CF(CGColorSpaceCreateDeviceGray()); }
THUNK(CGColorSpaceCreateWithName) { RET_CF(CGColorSpaceCreateWithName(ARGSTR(0))); }
THUNK(CGColorSpaceGetNumberOfComponents) { RET(CGColorSpaceGetNumberOfComponents((CGColorSpaceRef)ARGCF(0))); }
THUNK(CGColorSpaceRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}
THUNK(CGColorSpaceRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}

// --- data providers ---------------------------------------------------------------------------
// CGDataProviderCreateWithData(void *info, const void *data, size_t size,
//                              void (*release)(void *info, const void *data, size_t size))

struct gprov {
    gaddr_t info, data, release;
    uint32_t size;
};

static void prov_release(void *info, const void *data, size_t size) {
    struct gprov *p = info;
    if (p->release) gcall3(p->release, p->info, p->data, p->size);
    free(p);
}

THUNK(CGDataProviderCreateWithData) {
    struct gprov *p = calloc(1, sizeof *p);
    p->info = ARG(0);
    p->data = ARG(1);
    p->size = ARG(2);
    p->release = ARG(3);
    RET_CF(CGDataProviderCreateWithData(p, ARGP(1), ARG(2), prov_release));
}
THUNK(CGDataProviderCreateWithCFData) { RET_CF(CGDataProviderCreateWithCFData((CFDataRef)ARGCF(0))); }
THUNK(CGDataProviderCreateWithURL) { RET_CF(CGDataProviderCreateWithURL((CFURLRef)ARGCF(0))); }
THUNK(CGDataProviderCopyData) { RET_CF(CGDataProviderCopyData((CGDataProviderRef)ARGCF(0))); }
THUNK(CGDataProviderRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}
THUNK(CGDataProviderRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}

// --- images ------------------------------------------------------------------------------------

// Read an optional guest CGFloat[n] (decode arrays, domains, ranges).
static CGFloat *read_floats(gaddr_t g, size_t n, CGFloat *buf, size_t cap) {
    if (!g || n > cap) return NULL;
    for (size_t i = 0; i < n; i++) buf[i] = greadf(g + 4 * (uint32_t)i);
    return buf;
}

// CGImageCreate(width, height, bpc, bpp, bytesPerRow, space, bitmapInfo, provider,
//               const CGFloat *decode, bool shouldInterpolate, CGColorRenderingIntent)
THUNK(CGImageCreate) {
    CGColorSpaceRef cs = (CGColorSpaceRef)ARGCF(5);
    CGFloat dec[64];
    size_t ncomp = cs ? CGColorSpaceGetNumberOfComponents(cs) : 1;
    const CGFloat *decode = read_floats(ARG(8), 2 * ncomp, dec, 64);
    RET_CF(CGImageCreate(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), cs, ARG(6), (CGDataProviderRef)ARGCF(7), decode,
                         ARG(9) & 0xff, (CGColorRenderingIntent)ARG(10)));
}
THUNK(CGImageCreateWithImageInRect) { RET_CF(CGImageCreateWithImageInRect((CGImageRef)ARGCF(0), ARGRECT(1))); }
THUNK(CGImageCreateCopy) { RET_CF(CGImageCreateCopy((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetDataProvider) { RET_CF(CGImageGetDataProvider((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetWidth) { RET(CGImageGetWidth((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetHeight) { RET(CGImageGetHeight((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetBitsPerComponent) { RET(CGImageGetBitsPerComponent((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetBitsPerPixel) { RET(CGImageGetBitsPerPixel((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetBytesPerRow) { RET(CGImageGetBytesPerRow((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetBitmapInfo) { RET(CGImageGetBitmapInfo((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetAlphaInfo) { RET(CGImageGetAlphaInfo((CGImageRef)ARGCF(0))); }
THUNK(CGImageGetColorSpace) { RET_CF(CGImageGetColorSpace((CGImageRef)ARGCF(0))); }
THUNK(CGImageRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}
THUNK(CGImageRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}

// --- ImageIO ------------------------------------------------------------------------------------

THUNK(CGImageSourceCreateWithData) {
    RET_CF(CGImageSourceCreateWithData((CFDataRef)ARGCF(0), (CFDictionaryRef)ARGCF(1)));
}
THUNK(CGImageSourceCreateWithURL) {
    RET_CF(CGImageSourceCreateWithURL((CFURLRef)ARGCF(0), (CFDictionaryRef)ARGCF(1)));
}
THUNK(CGImageSourceGetCount) { RET(CGImageSourceGetCount((CGImageSourceRef)ARGCF(0))); }
THUNK(CGImageSourceCreateImageAtIndex) {
    RET_CF(CGImageSourceCreateImageAtIndex((CGImageSourceRef)ARGCF(0), ARG(1), (CFDictionaryRef)ARGCF(2)));
}
THUNK(CGImageSourceCopyPropertiesAtIndex) {
    RET_CF(CGImageSourceCopyPropertiesAtIndex((CGImageSourceRef)ARGCF(0), ARG(1), (CFDictionaryRef)ARGCF(2)));
}
THUNK(CGImageDestinationCreateWithURL) {
    RET_CF(CGImageDestinationCreateWithURL((CFURLRef)ARGCF(0), ARGSTR(1), ARG(2), (CFDictionaryRef)ARGCF(3)));
}
THUNK(CGImageDestinationCreateWithData) {
    RET_CF(CGImageDestinationCreateWithData((CFMutableDataRef)ARGCF(0), ARGSTR(1), ARG(2), (CFDictionaryRef)ARGCF(3)));
}
THUNK(CGImageDestinationAddImage) {
    CGImageDestinationAddImage((CGImageDestinationRef)ARGCF(0), (CGImageRef)ARGCF(1), (CFDictionaryRef)ARGCF(2));
}
THUNK(CGImageDestinationSetProperties) {
    CGImageDestinationSetProperties((CGImageDestinationRef)ARGCF(0), (CFDictionaryRef)ARGCF(1));
}
THUNK(CGImageDestinationFinalize) { RET_BOOL(CGImageDestinationFinalize((CGImageDestinationRef)ARGCF(0))); }

// --- bitmap contexts -----------------------------------------------------------------------------

struct gbitmap {
    gaddr_t data;          // guest buffer
    gaddr_t release, info; // guest release callback (CGBitmapContextCreateWithData)
    bool owned;            // we allocated the buffer
};

static void bitmap_release(void *info, void *data) {
    struct gbitmap *b = info;
    if (b->release) gcall2(b->release, b->info, b->data);
    if (b->owned) gheap_free(b->data);
    free(b);
}

static CGContextRef create_bitmap(gaddr_t data, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs,
                                  uint32_t info, gaddr_t release, gaddr_t rinfo) {
    struct gbitmap *b = calloc(1, sizeof *b);
    b->release = release;
    b->info = rinfo;
    if (!data && w && h) {
        // Allocate the backing store in guest memory so CGBitmapContextGetData
        // hands out a guest-visible pointer.
        size_t ncomp = cs ? CGColorSpaceGetNumberOfComponents(cs) : 0;
        uint32_t alpha = info & kCGBitmapAlphaInfoMask;
        if (alpha != kCGImageAlphaNone) ncomp++;
        if (!bpr) bpr = ((w * ncomp * bpc / 8) + 31) & ~(size_t)31;
        data = gheap_calloc(1, (uint32_t)(bpr * h));
        if (!data) {
            free(b);
            return NULL;
        }
        b->owned = true;
    }
    b->data = data;
    CGContextRef c = CGBitmapContextCreateWithData(g2h(data), w, h, bpc, bpr, cs, info, bitmap_release, b);
    if (!c) {
        if (b->owned) gheap_free(data);
        free(b);
    }
    return c;
}

// CGBitmapContextCreate(data, width, height, bpc, bytesPerRow, space, bitmapInfo)
THUNK(CGBitmapContextCreate) {
    RET_CF(create_bitmap(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (CGColorSpaceRef)ARGCF(5), ARG(6), 0, 0));
}
THUNK(CGBitmapContextCreateWithData) {
    RET_CF(create_bitmap(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (CGColorSpaceRef)ARGCF(5), ARG(6), ARG(7), ARG(8)));
}
THUNK(CGBitmapContextGetData) {
    void *p = CGBitmapContextGetData(CTX(0));
    RET(p && host_ptr_in_guest(p) ? h2g(p) : 0);
}
THUNK(CGBitmapContextGetWidth) { RET(CGBitmapContextGetWidth(CTX(0))); }
THUNK(CGBitmapContextGetHeight) { RET(CGBitmapContextGetHeight(CTX(0))); }
THUNK(CGBitmapContextGetBytesPerRow) { RET(CGBitmapContextGetBytesPerRow(CTX(0))); }
THUNK(CGBitmapContextGetBitsPerPixel) { RET(CGBitmapContextGetBitsPerPixel(CTX(0))); }
THUNK(CGBitmapContextGetBitmapInfo) { RET(CGBitmapContextGetBitmapInfo(CTX(0))); }
THUNK(CGBitmapContextCreateImage) { RET_CF(CGBitmapContextCreateImage(CTX(0))); }

// --- context state & drawing ----------------------------------------------------------------------

THUNK(CGContextRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}
THUNK(CGContextRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}
THUNK(CGContextSaveGState) { CGContextSaveGState(CTX(0)); }
THUNK(CGContextRestoreGState) { CGContextRestoreGState(CTX(0)); }
THUNK(CGContextFlush) { CGContextFlush(CTX(0)); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
THUNK(CGContextSynchronize) { CGContextSynchronize(CTX(0)); }
#pragma clang diagnostic pop

THUNK(CGContextScaleCTM) { CGContextScaleCTM(CTX(0), ARGF(1), ARGF(2)); }
THUNK(CGContextTranslateCTM) { CGContextTranslateCTM(CTX(0), ARGF(1), ARGF(2)); }
THUNK(CGContextRotateCTM) { CGContextRotateCTM(CTX(0), ARGF(1)); }
THUNK(CGContextConcatCTM) { CGContextConcatCTM(CTX(0), ARGXFORM(1)); }
THUNK(CGContextGetCTM) { RET_XFORM(CGContextGetCTM(CTX(1))); }

THUNK(CGContextSetAlpha) { CGContextSetAlpha(CTX(0), ARGF(1)); }
THUNK(CGContextSetBlendMode) { CGContextSetBlendMode(CTX(0), (CGBlendMode)ARGI(1)); }
THUNK(CGContextSetInterpolationQuality) { CGContextSetInterpolationQuality(CTX(0), (CGInterpolationQuality)ARGI(1)); }
THUNK(CGContextSetShouldAntialias) { CGContextSetShouldAntialias(CTX(0), ARG(1) & 0xff); }
THUNK(CGContextSetAllowsAntialiasing) { CGContextSetAllowsAntialiasing(CTX(0), ARG(1) & 0xff); }
THUNK(CGContextSetShouldSmoothFonts) { CGContextSetShouldSmoothFonts(CTX(0), ARG(1) & 0xff); }
THUNK(CGContextSetLineWidth) { CGContextSetLineWidth(CTX(0), ARGF(1)); }
THUNK(CGContextSetLineCap) { CGContextSetLineCap(CTX(0), (CGLineCap)ARGI(1)); }
THUNK(CGContextSetLineJoin) { CGContextSetLineJoin(CTX(0), (CGLineJoin)ARGI(1)); }
// CGContextSetLineDash(c, CGFloat phase, const CGFloat *lengths, size_t count)
THUNK(CGContextSetLineDash) {
    uint32_t n = ARG(3);
    CGFloat *len = n ? calloc(n, sizeof(CGFloat)) : NULL;
    for (uint32_t i = 0; i < n; i++) len[i] = greadf(ARG(2) + 4 * i);
    CGContextSetLineDash(CTX(0), ARGF(1), len, n);
    free(len);
}
THUNK(CGContextSetRGBFillColor) { CGContextSetRGBFillColor(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
THUNK(CGContextSetRGBStrokeColor) { CGContextSetRGBStrokeColor(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
THUNK(CGContextSetGrayFillColor) { CGContextSetGrayFillColor(CTX(0), ARGF(1), ARGF(2)); }
THUNK(CGContextSetGrayStrokeColor) { CGContextSetGrayStrokeColor(CTX(0), ARGF(1), ARGF(2)); }

THUNK(CGContextBeginPath) { CGContextBeginPath(CTX(0)); }
THUNK(CGContextClosePath) { CGContextClosePath(CTX(0)); }
THUNK(CGContextMoveToPoint) { CGContextMoveToPoint(CTX(0), ARGF(1), ARGF(2)); }
THUNK(CGContextAddLineToPoint) { CGContextAddLineToPoint(CTX(0), ARGF(1), ARGF(2)); }
THUNK(CGContextAddArcToPoint) { CGContextAddArcToPoint(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5)); }
THUNK(CGContextAddArc) { CGContextAddArc(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGI(6)); }
THUNK(CGContextAddRect) { CGContextAddRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextAddEllipseInRect) { CGContextAddEllipseInRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextAddPath) { CGContextAddPath(CTX(0), (CGPathRef)ARGCF(1)); }
THUNK(CGContextFillPath) { CGContextFillPath(CTX(0)); }
THUNK(CGContextEOFillPath) { CGContextEOFillPath(CTX(0)); }
THUNK(CGContextStrokePath) { CGContextStrokePath(CTX(0)); }
THUNK(CGContextFillRect) { CGContextFillRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextFillEllipseInRect) { CGContextFillEllipseInRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextStrokeRect) { CGContextStrokeRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextStrokeRectWithWidth) { CGContextStrokeRectWithWidth(CTX(0), ARGRECT(1), ARGF(5)); }
THUNK(CGContextClearRect) { CGContextClearRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextClip) { CGContextClip(CTX(0)); }
THUNK(CGContextEOClip) { CGContextEOClip(CTX(0)); }
THUNK(CGContextClipToRect) { CGContextClipToRect(CTX(0), ARGRECT(1)); }
THUNK(CGContextClipToMask) { CGContextClipToMask(CTX(0), ARGRECT(1), (CGImageRef)ARGCF(5)); }
THUNK(CGContextDrawImage) { CGContextDrawImage(CTX(0), ARGRECT(1), (CGImageRef)ARGCF(5)); }
THUNK(CGContextDrawShading) { CGContextDrawShading(CTX(0), (CGShadingRef)ARGCF(1)); }

// Text (deprecated CG text API is still present for x86_64).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
THUNK(CGContextSelectFont) { CGContextSelectFont(CTX(0), ARGS(1), ARGF(2), (CGTextEncoding)ARG(3)); }
THUNK(CGContextShowTextAtPoint) { CGContextShowTextAtPoint(CTX(0), ARGF(1), ARGF(2), ARGS(3), ARG(4)); }
THUNK(CGContextShowText) { CGContextShowText(CTX(0), ARGS(1), ARG(2)); }
#pragma clang diagnostic pop
THUNK(CGContextSetFont) { CGContextSetFont(CTX(0), (CGFontRef)ARGCF(1)); }
THUNK(CGContextSetFontSize) { CGContextSetFontSize(CTX(0), ARGF(1)); }
THUNK(CGContextSetCharacterSpacing) { CGContextSetCharacterSpacing(CTX(0), ARGF(1)); }
THUNK(CGContextSetTextDrawingMode) { CGContextSetTextDrawingMode(CTX(0), (CGTextDrawingMode)ARGI(1)); }
THUNK(CGContextSetTextMatrix) { CGContextSetTextMatrix(CTX(0), ARGXFORM(1)); }
THUNK(CGContextGetTextMatrix) { RET_XFORM(CGContextGetTextMatrix(CTX(1))); }
THUNK(CGContextSetTextPosition) { CGContextSetTextPosition(CTX(0), ARGF(1), ARGF(2)); }
// CGPoint (8 bytes) returned in edx:eax.
THUNK(CGContextGetTextPosition) {
    CGPoint p = CGContextGetTextPosition(CTX(0));
    RET_POINT(p);
}
THUNK(CGFontRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}
THUNK(CGFontRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}

// --- paths ------------------------------------------------------------------------------------------
// Transform arguments are const CGAffineTransform * (guest floats) or NULL.

#define ARGMAT(i, var) CGAffineTransform var##_s, *var = ARG(i) ? (var##_s = get_gxform(ARG(i)), &var##_s) : NULL

THUNK(CGPathCreateMutable) { RET_CF(CGPathCreateMutable()); }
THUNK(CGPathMoveToPoint) {
    ARGMAT(1, m);
    CGPathMoveToPoint((CGMutablePathRef)ARGCF(0), m, ARGF(2), ARGF(3));
}
THUNK(CGPathAddLineToPoint) {
    ARGMAT(1, m);
    CGPathAddLineToPoint((CGMutablePathRef)ARGCF(0), m, ARGF(2), ARGF(3));
}
THUNK(CGPathAddArcToPoint) {
    ARGMAT(1, m);
    CGPathAddArcToPoint((CGMutablePathRef)ARGCF(0), m, ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGF(6));
}
THUNK(CGPathAddRect) {
    ARGMAT(1, m);
    CGPathAddRect((CGMutablePathRef)ARGCF(0), m, ARGRECT(2));
}
THUNK(CGPathCloseSubpath) { CGPathCloseSubpath((CGMutablePathRef)ARGCF(0)); }
THUNK(CGPathRetain) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRetain(c);
    RET(ARG(0));
}
THUNK(CGPathRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}

// --- functions & shadings -------------------------------------------------------------------------------
// CGFunctionCallbacks (i386): {unsigned version; evaluate(void *info, const CGFloat *in, CGFloat *out);
//                              releaseInfo(void *info)} -- 12 bytes.

struct gfunc {
    gaddr_t info, evaluate, release;
    size_t nin, nout;
    gaddr_t scratch; // guest float[nin + nout]
    pthread_mutex_t lock;
};

static void gfunc_evaluate(void *info, const CGFloat *in, CGFloat *out) {
    struct gfunc *f = info;
    pthread_mutex_lock(&f->lock);
    for (size_t i = 0; i < f->nin; i++) gwritef(f->scratch + 4 * (uint32_t)i, in[i]);
    for (size_t i = 0; i < f->nout; i++) gwritef(f->scratch + 4 * (uint32_t)(f->nin + i), 0);
    gcall3(f->evaluate, f->info, f->scratch, f->scratch + 4 * (uint32_t)f->nin);
    for (size_t i = 0; i < f->nout; i++) out[i] = greadf(f->scratch + 4 * (uint32_t)(f->nin + i));
    pthread_mutex_unlock(&f->lock);
}

static void gfunc_release(void *info) {
    struct gfunc *f = info;
    if (f->release) gcall1(f->release, f->info);
    gheap_free(f->scratch);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

// CGFunctionCreate(info, domainDim, const CGFloat *domain, rangeDim, const CGFloat *range,
//                  const CGFunctionCallbacks *callbacks)
THUNK(CGFunctionCreate) {
    gaddr_t cb = ARG(5);
    if (!cb) {
        RET(0);
        return;
    }
    size_t nin = ARG(1), nout = ARG(3);
    CGFloat dom[64], rng[64];
    const CGFloat *domain = read_floats(ARG(2), 2 * nin, dom, 64);
    const CGFloat *range = read_floats(ARG(4), 2 * nout, rng, 64);
    struct gfunc *f = calloc(1, sizeof *f);
    f->info = ARG(0);
    f->evaluate = gread32(cb + 4);
    f->release = gread32(cb + 8);
    f->nin = nin;
    f->nout = nout;
    f->scratch = gheap_calloc(1, (uint32_t)(4 * (nin + nout) + 4));
    pthread_mutex_init(&f->lock, NULL);
    CGFunctionCallbacks hcb = {0, f->evaluate ? gfunc_evaluate : NULL, gfunc_release};
    CGFunctionRef fn = CGFunctionCreate(f, nin, domain, nout, range, &hcb);
    if (!fn) gfunc_release(f);
    RET_CF(fn);
}
THUNK(CGFunctionRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}

// CGShadingCreateAxial(space, CGPoint start, CGPoint end, function, bool extStart, bool extEnd)
THUNK(CGShadingCreateAxial) {
    RET_CF(CGShadingCreateAxial((CGColorSpaceRef)ARGCF(0), ARGPOINT(1), ARGPOINT(3), (CGFunctionRef)ARGCF(5),
                                ARG(6) & 0xff, ARG(7) & 0xff));
}
// CGShadingCreateRadial(space, start, startRadius, end, endRadius, function, extStart, extEnd)
THUNK(CGShadingCreateRadial) {
    RET_CF(CGShadingCreateRadial((CGColorSpaceRef)ARGCF(0), ARGPOINT(1), ARGF(3), ARGPOINT(4), ARGF(6),
                                 (CGFunctionRef)ARGCF(7), ARG(8) & 0xff, ARG(9) & 0xff));
}
THUNK(CGShadingRelease) {
    CFTypeRef c = ARGCF(0);
    if (c) CFRelease(c);
}
