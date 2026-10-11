// CoreText fonts and lines. CGGlyph (uint16) and UniChar arrays have the same
// layout on both ABIs; CGSize/CGRect outputs are converted to i386 floats.
#include "cg_internal.h"

#include <CoreText/CoreText.h>
#include <stdlib.h>

#define FONT(i) ((CTFontRef)ARGCF(i))

static void init_ct_fgcolor(gaddr_t a) { gwrite32(a, cfg(kCTForegroundColorFromContextAttributeName)); }
GDATA(kCTForegroundColorFromContextAttributeName, 4, init_ct_fgcolor);
static void init_ct_font(gaddr_t a) { gwrite32(a, cfg(kCTFontAttributeName)); }
GDATA(kCTFontAttributeName, 4, init_ct_font);
static void init_ct_fgc(gaddr_t a) { gwrite32(a, cfg(kCTForegroundColorAttributeName)); }
GDATA(kCTForegroundColorAttributeName, 4, init_ct_fgc);

// CTFontCreateWithName(CFStringRef name, CGFloat size, const CGAffineTransform *matrix)
THUNK(CTFontCreateWithName) {
    CGAffineTransform m;
    if (ARG(2)) m = get_gxform(ARG(2));
    RET_CF(CTFontCreateWithName(ARGSTR(0), ARGF(1), ARG(2) ? &m : NULL));
}
// CTFontCreateUIFontForLanguage(CTFontUIFontType, CGFloat size, CFStringRef language)
THUNK(CTFontCreateUIFontForLanguage) {
    RET_CF(CTFontCreateUIFontForLanguage((CTFontUIFontType)ARG(0), ARGF(1), ARGSTR(2)));
}
// CTFontCreateCopyWithSymbolicTraits(font, size, const CGAffineTransform *, value, mask)
THUNK(CTFontCreateCopyWithSymbolicTraits) {
    CGAffineTransform m;
    if (ARG(2)) m = get_gxform(ARG(2));
    RET_CF(CTFontCreateCopyWithSymbolicTraits(FONT(0), ARGF(1), ARG(2) ? &m : NULL, ARG(3), ARG(4)));
}
THUNK(CTFontCopyFullName) { RET_CF(CTFontCopyFullName(FONT(0))); }
THUNK(CTFontCopyPostScriptName) { RET_CF(CTFontCopyPostScriptName(FONT(0))); }
THUNK(CTFontCopyFamilyName) { RET_CF(CTFontCopyFamilyName(FONT(0))); }
// CGFontRef CTFontCopyGraphicsFont(font, CTFontDescriptorRef *attributes)
THUNK(CTFontCopyGraphicsFont) {
    CTFontDescriptorRef d = NULL;
    CGFontRef f = CTFontCopyGraphicsFont(FONT(0), ARG(1) ? &d : NULL);
    if (ARG(1)) gwrite32(ARG(1), cfg(d));
    RET_CF(f);
}
THUNK(CTFontGetSize) { RETD(CTFontGetSize(FONT(0))); }
THUNK(CTFontGetAscent) { RETD(CTFontGetAscent(FONT(0))); }
THUNK(CTFontGetDescent) { RETD(CTFontGetDescent(FONT(0))); }
THUNK(CTFontGetLeading) { RETD(CTFontGetLeading(FONT(0))); }
THUNK(CTFontGetUnitsPerEm) { RET(CTFontGetUnitsPerEm(FONT(0))); }
THUNK(CTFontGetSymbolicTraits) { RET(CTFontGetSymbolicTraits(FONT(0))); }
// CGRect CTFontGetBoundingBox(font): hidden pointer at ARG(0).
THUNK(CTFontGetBoundingBox) { RET_RECT(CTFontGetBoundingBox(FONT(1))); }

// bool CTFontGetGlyphsForCharacters(font, const UniChar *, CGGlyph *, CFIndex count)
THUNK(CTFontGetGlyphsForCharacters) {
    RET_BOOL(CTFontGetGlyphsForCharacters(FONT(0), ARGP(1), ARGP(2), ARGIDX(3)));
}

// double CTFontGetAdvancesForGlyphs(font, orientation, const CGGlyph *, CGSize *advances, CFIndex count)
THUNK(CTFontGetAdvancesForGlyphs) {
    CFIndex n = ARGIDX(4);
    CGSize *adv = ARG(3) && n > 0 ? calloc((size_t)n, sizeof(CGSize)) : NULL;
    double total = CTFontGetAdvancesForGlyphs(FONT(0), (CTFontOrientation)ARG(1), ARGP(2), adv, n);
    if (adv) {
        for (CFIndex i = 0; i < n; i++) put_gsize(ARG(3) + 8 * (uint32_t)i, adv[i]);
        free(adv);
    }
    RETD(total);
}

// CGRect CTFontGetBoundingRectsForGlyphs(font, orientation, const CGGlyph *, CGRect *rects, CFIndex count)
// (hidden struct-return pointer at ARG(0))
THUNK(CTFontGetBoundingRectsForGlyphs) {
    CFIndex n = ARGIDX(5);
    CGRect *rc = ARG(4) && n > 0 ? calloc((size_t)n, sizeof(CGRect)) : NULL;
    CGRect all = CTFontGetBoundingRectsForGlyphs(FONT(1), (CTFontOrientation)ARG(2), ARGP(3), rc, n);
    if (rc) {
        for (CFIndex i = 0; i < n; i++) put_grect(ARG(4) + 16 * (uint32_t)i, rc[i]);
        free(rc);
    }
    RET_RECT(all);
}

THUNK(CTLineCreateWithAttributedString) { RET_CF(CTLineCreateWithAttributedString((CFAttributedStringRef)ARGCF(0))); }
THUNK(CTLineDraw) { CTLineDraw((CTLineRef)ARGCF(0), (CGContextRef)ARGCF(1)); }
// CGRect CTLineGetImageBounds(line, context): hidden pointer at ARG(0).
THUNK(CTLineGetImageBounds) { RET_RECT(CTLineGetImageBounds((CTLineRef)ARGCF(1), (CGContextRef)ARGCF(2))); }
// double CTLineGetTypographicBounds(line, CGFloat *ascent, CGFloat *descent, CGFloat *leading)
THUNK(CTLineGetTypographicBounds) {
    CGFloat a = 0, d = 0, l = 0;
    double w = CTLineGetTypographicBounds((CTLineRef)ARGCF(0), &a, &d, &l);
    if (ARG(1)) gwritef(ARG(1), a);
    if (ARG(2)) gwritef(ARG(2), d);
    if (ARG(3)) gwritef(ARG(3), l);
    RETD(w);
}
