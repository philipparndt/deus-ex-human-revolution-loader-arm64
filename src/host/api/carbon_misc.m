// Carbon odds and ends: standard alerts/sheets (NSAlert), theme cursors and text
// metrics, Scrap Manager (NSPasteboard), TSM documents, AESend, printing stubs,
// dock tile, Text Input Sources / Keyboard Layouts and data constants.
#import "carbon_internal.h"
#include "cs_ae.h"


// --- alerts ------------------------------------------------------------------------------------------------

// A DialogRef from CreateStandardAlert/Sheet. It is also its own WindowRef
// (GetDialogWindow returns the same handle).
@interface CBAlert : CBWindow {
  @public
    NSAlert *alert;
    int items[3];     // NSAlert button index -> DialogItemIndex
    int nbuttons;
    CBTarget *notify; // sheets: command target (retained)
}
@end
@implementation CBAlert
- (void)dealloc {
    [alert release];
    [notify release];
    ns = nil; // owned by the NSAlert
    [super dealloc];
}
@end

static NSString *nsstr(gaddr_t g) {
    NSString *s = g ? (NSString *)gcf_string(g) : nil;
    return s ? s : @"";
}

// AlertStdCFStringAlertParamRec (pack 2): version 0, movable 4, helpButton 5, defaultText 6,
// cancelText 10, otherText 14, defaultButton 18, cancelButton 20, position 22, flags 24.
static CBAlert *make_alert(uint32_t type, gaddr_t err, gaddr_t expl, gaddr_t param) {
    CBAlert *a = [[CBAlert alloc] initWithKind:TK_WINDOW];
    a->alert = [[NSAlert alloc] init];
    a->wclass = kMovableAlertWindowClass;
    [a->alert setMessageText:nsstr(err)];
    if (expl) [a->alert setInformativeText:nsstr(expl)];
    [a->alert setAlertStyle:type == kAlertStopAlert ? NSAlertStyleCritical
                            : type == kAlertCautionAlert ? NSAlertStyleWarning
                                                         : NSAlertStyleInformational];
    gaddr_t def = (gaddr_t)kAlertDefaultOKText, can = 0, oth = 0;
    if (param) {
        def = gread32(param + 6);
        can = gread32(param + 10);
        oth = gread32(param + 14);
    }
    struct {
        gaddr_t text;
        NSString *fallback;
        int item;
    } b[3] = {{def, @"OK", kAlertStdAlertOKButton},
              {can, @"Cancel", kAlertStdAlertCancelButton},
              {oth, @"Don't Save", kAlertStdAlertOtherButton}};
    for (int i = 0; i < 3; i++) {
        if (!b[i].text) continue;
        NSString *t = b[i].text == (gaddr_t)-1 ? b[i].fallback : nsstr(b[i].text);
        NSButton *btn = [a->alert addButtonWithTitle:t];
        if (b[i].item == kAlertStdAlertCancelButton) [btn setKeyEquivalent:@"\e"];
        a->items[a->nbuttons++] = b[i].item;
    }
    if (!a->nbuttons) {
        [a->alert addButtonWithTitle:@"OK"];
        a->items[a->nbuttons++] = kAlertStdAlertOKButton;
    }
    a->ns = [a->alert window];
    return a;
}

// CreateStandardAlert(AlertType, CFStringRef error, CFStringRef explanation, const param *, DialogRef *out)
THUNK(CreateStandardAlert) {
    __block CBAlert *a;
    cb_on_main(^{
      cb_app_init();
      a = make_alert(ARG(0), ARG(1), ARG(2), ARG(3));
    });
    LOGI("carbon: CreateStandardAlert(\"%s\")", [[a->alert messageText] UTF8String]);
    if (ARG(4)) gwrite32(ARG(4), cb_handle(a));
    RET(noErr);
}
// CreateStandardSheet(AlertType, error, explanation, param, EventTargetRef notifyTarget, DialogRef *out)
THUNK(CreateStandardSheet) {
    __block CBAlert *a;
    cb_on_main(^{
      cb_app_init();
      a = make_alert(ARG(0), ARG(1), ARG(2), ARG(3));
    });
    a->notify = [CB_UNWRAP(CBTarget, ARG(4)) retain];
    LOGI("carbon: CreateStandardSheet(\"%s\")", [[a->alert messageText] UTF8String]);
    if (ARG(5)) gwrite32(ARG(5), cb_handle(a));
    RET(noErr);
}
THUNK(GetDialogWindow) { RET(ARG(0)); }

// RunStandardAlert(DialogRef, ModalFilterUPP, DialogItemIndex *outItemHit)
THUNK(RunStandardAlert) {
    CBAlert *a = CB_UNWRAP(CBAlert, ARG(0));
    if (!a) RET(paramErr);
    __block NSModalResponse res;
    cb_on_main(^{
      [NSApp activateIgnoringOtherApps:YES];
      res = [a->alert runModal];
    });
    int idx = (int)(res - NSAlertFirstButtonReturn);
    int item = idx >= 0 && idx < a->nbuttons ? a->items[idx] : kAlertStdAlertCancelButton;
    LOGI("carbon: RunStandardAlert -> item %d", item);
    if (ARG(2)) gwrite16(ARG(2), (uint16_t)item);
    [a release]; // the alert is disposed after running
    RET(noErr);
}

// Called by ShowSheetWindow for sheets created by CreateStandardSheet.
bool cb_alert_show_sheet(CBWindow *w, CBWindow *parent) {
    if (![w isKindOfClass:[CBAlert class]]) return false;
    CBAlert *a = (CBAlert *)w;
    [a retain];
    cb_on_main(^{
      [a->alert beginSheetModalForWindow:parent->ns completionHandler:^(NSModalResponse res) {
        int idx = (int)(res - NSAlertFirstButtonReturn);
        int item = idx >= 0 && idx < a->nbuttons ? a->items[idx] : kAlertStdAlertCancelButton;
        uint32_t cmd = item == kAlertStdAlertOKButton       ? kHICommandOK
                       : item == kAlertStdAlertCancelButton ? kHICommandCancel
                                                            : kHICommandOther;
        if (a->notify) cb_send_command(cmd, 0, 0, 0, 0, a->notify);
        [a release];
      }];
    });
    return true;
}

// GetStandardAlertDefaultParams(AlertStdCFStringAlertParamPtr, UInt32 version)
THUNK(GetStandardAlertDefaultParams) {
    gaddr_t p = ARG(0);
    if (!p) RET(paramErr);
    memset(g2h(p), 0, 28);
    gwrite32(p, ARG(1));
    gwrite8(p + 4, 1);
    gwrite32(p + 6, (uint32_t)kAlertDefaultOKText);
    gwrite16(p + 18, kAlertStdAlertOKButton);
    RET(noErr);
}

// GetColor(Point where, ConstStr255Param prompt, const RGBColor *in, RGBColor *out)
THUNK(GetColor) {
    if (ARG(2) && ARG(3)) memcpy(g2h(ARG(3)), g2h(ARG(2)), 6);
    RET(0);
}

// --- theme -----------------------------------------------------------------------------------------------------

THUNK(SetThemeCursor) {
    uint32_t c = ARG(0);
    cb_on_main(^{
      NSCursor *cur;
      switch (c) {
      case kThemeCopyArrowCursor: cur = [NSCursor dragCopyCursor]; break;
      case kThemeAliasArrowCursor: cur = [NSCursor dragLinkCursor]; break;
      case kThemeContextualMenuArrowCursor: cur = [NSCursor contextualMenuCursor]; break;
      case kThemeIBeamCursor: cur = [NSCursor IBeamCursor]; break;
      case kThemeCrossCursor: case kThemePlusCursor: cur = [NSCursor crosshairCursor]; break;
      case kThemeClosedHandCursor: cur = [NSCursor closedHandCursor]; break;
      case kThemeOpenHandCursor: cur = [NSCursor openHandCursor]; break;
      case kThemePointingHandCursor: cur = [NSCursor pointingHandCursor]; break;
      case kThemeResizeLeftCursor: cur = [NSCursor resizeLeftCursor]; break;
      case kThemeResizeRightCursor: cur = [NSCursor resizeRightCursor]; break;
      case kThemeResizeLeftRightCursor: cur = [NSCursor resizeLeftRightCursor]; break;
      case kThemeNotAllowedCursor: cur = [NSCursor operationNotAllowedCursor]; break;
      case kThemeResizeUpCursor: cur = [NSCursor resizeUpCursor]; break;
      case kThemeResizeDownCursor: cur = [NSCursor resizeDownCursor]; break;
      case kThemeResizeUpDownCursor: cur = [NSCursor resizeUpDownCursor]; break;
      case kThemePoofCursor: cur = [NSCursor disappearingItemCursor]; break;
      default: cur = [NSCursor arrowCursor]; break;
      }
      [cur set];
    });
    RET(noErr);
}

// i386 HIThemeTextInfo (pack 2): version 0, state 4, fontID 8 (UInt16), horizontalFlushness 10,
// verticalFlushness 14, options 18, truncationPosition 22, truncationMaxLines 26,
// truncationHappened 30, filler 31, font 32 (version 1).
static void get_text_info(gaddr_t g, HIThemeTextInfo *ti) {
    memset(ti, 0, sizeof *ti);
    if (!g) {
        ti->fontID = kThemeSystemFont;
        ti->horizontalFlushness = kHIThemeTextHorizontalFlushLeft;
        ti->verticalFlushness = kHIThemeTextVerticalFlushTop;
        return;
    }
    ti->version = 0;
    ti->state = gread32(g + 4);
    ti->fontID = gread16(g + 8);
    ti->horizontalFlushness = gread32(g + 10);
    ti->verticalFlushness = gread32(g + 14);
    ti->options = gread32(g + 18);
    ti->truncationPosition = gread32(g + 22);
    ti->truncationMaxLines = gread32(g + 26);
    if (gread32(g) >= 1 && gread32(g + 32)) {
        ti->version = 1;
        ti->font = (CTFontRef)gcf(gread32(g + 32));
    }
}

// HIThemeGetTextDimensions(CFTypeRef string, CGFloat width, HIThemeTextInfo *, CGFloat *w, CGFloat *h, CGFloat *baseline)
THUNK(HIThemeGetTextDimensions) {
    CFTypeRef s = ARGCF(0);
    HIThemeTextInfo ti;
    get_text_info(ARG(2), &ti);
    CGFloat w = 0, h = 0, b = 0;
    OSStatus st = s ? HIThemeGetTextDimensions(s, ARGF(1), &ti, &w, &h, &b) : paramErr;
    if (ARG(3)) gwritef(ARG(3), w);
    if (ARG(4)) gwritef(ARG(4), h);
    if (ARG(5)) gwritef(ARG(5), b);
    RET(st);
}
THUNK(HIThemeGetUIFontType) { RET(HIThemeGetUIFontType((ThemeFontID)ARG(0))); }

// GetThemeTextDimensions(CFStringRef, ThemeFontID, ThemeDrawState, Boolean wrap, Point *ioBounds, SInt16 *outBaseline)
THUNK(GetThemeTextDimensions) {
    CFStringRef s = ARGSTR(0);
    HIThemeTextInfo ti;
    get_text_info(0, &ti);
    ti.fontID = (ThemeFontID)ARG(1);
    ti.state = ARG(2);
    bool wrap = ARG(3) & 0xff;
    CGFloat width = 0;
    if (wrap && ARG(4)) width = (int16_t)gread16(ARG(4) + 2);
    CGFloat w = 0, h = 0, b = 0;
    OSStatus st = s ? HIThemeGetTextDimensions(s, width, &ti, &w, &h, &b) : paramErr;
    if (ARG(4)) {
        gwrite16(ARG(4), (uint16_t)(int16_t)ceil(h));
        gwrite16(ARG(4) + 2, (uint16_t)(int16_t)ceil(w));
    }
    if (ARG(5)) gwrite16(ARG(5), (uint16_t)(int16_t)lround(b));
    RET(st);
}

// DrawThemeTextBox(CFStringRef, ThemeFontID, ThemeDrawState, Boolean wrap, const Rect *box, SInt16 just, CGContextRef)
THUNK(DrawThemeTextBox) {
    CFStringRef s = ARGSTR(0);
    CGContextRef ctx = (CGContextRef)ARGCF(6);
    if (!s || !ARG(4)) RET(paramErr);
    HIThemeTextInfo ti;
    get_text_info(0, &ti);
    ti.fontID = (ThemeFontID)ARG(1);
    ti.state = ARG(2);
    int16_t just = (int16_t)ARG(5);
    ti.horizontalFlushness = just == teJustCenter  ? kHIThemeTextHorizontalFlushCenter
                             : just == teFlushRight ? kHIThemeTextHorizontalFlushRight
                                                    : kHIThemeTextHorizontalFlushLeft;
    if (!(ARG(3) & 0xff)) {
        ti.truncationPosition = kHIThemeTextTruncationEnd;
        ti.truncationMaxLines = 1;
    }
    CGRect box = cb_get_qdrect(ARG(4));
    if (!ctx) ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) RET(paramErr);
    RET(HIThemeDrawTextBox(s, &box, &ti, ctx, kHIThemeOrientationNormal));
}

THUNK(GetTheme) { RET(noErr); }

// --- scrap ------------------------------------------------------------------------------------------------------

@interface CBScrap : CBBase {
  @public
    NSPasteboard *pb;
}
@end
@implementation CBScrap
@end

static NSMutableDictionary *scraps;

static NSString *flavor_type(uint32_t f) {
    switch (f) {
    case kScrapFlavorTypeText: return NSPasteboardTypeString;
    case kScrapFlavorTypeUnicode: return NSPasteboardTypeString;
    case kScrapFlavorTypePicture: return NSPasteboardTypePDF;
    }
    CFStringRef s = UTCreateStringForOSType(f);
    NSString *t = [NSString stringWithFormat:@"com.apple.scrap.%@", (NSString *)s];
    CFRelease(s);
    return t;
}

// GetScrapByName(CFStringRef name, OptionBits options, ScrapRef *out)
THUNK(GetScrapByName) {
    NSString *name = (NSString *)ARGSTR(0);
    uint32_t opts = ARG(1);
    if (!scraps) scraps = [[NSMutableDictionary alloc] init];
    CBScrap *s = scraps[name ? name : @""];
    if (!s) {
        s = [[[CBScrap alloc] init] autorelease];
        if (!name || [name isEqualToString:(NSString *)kScrapClipboardScrap])
            s->pb = [[NSPasteboard generalPasteboard] retain];
        else if ([name isEqualToString:(NSString *)kScrapFindScrap])
            s->pb = [[NSPasteboard pasteboardWithName:NSPasteboardNameFind] retain];
        else s->pb = [[NSPasteboard pasteboardWithName:name] retain];
        scraps[name ? name : @""] = s;
    }
    if (opts & kScrapClearNamedScrap) [s->pb clearContents];
    if (ARG(2)) gwrite32(ARG(2), cb_handle(s));
    RET(noErr);
}
THUNK(ClearScrap) {
    CBScrap *s = ARG(0) ? CB_UNWRAP(CBScrap, gread32(ARG(0))) : nil;
    if (!s) RET(paramErr);
    [s->pb clearContents];
    RET(noErr);
}
// PutScrapFlavor(scrap, flavorType, flags, Size, const void *data)
THUNK(PutScrapFlavor) {
    CBScrap *s = CB_UNWRAP(CBScrap, ARG(0));
    if (!s) RET(paramErr);
    uint32_t f = ARG(1), n = ARG(3);
    const void *d = ARG(4) ? g2h(ARG(4)) : NULL;
    if (!d) RET(noErr); // promise
    if (f == kScrapFlavorTypeText) {
        NSString *t = [[[NSString alloc] initWithBytes:d length:n encoding:NSMacOSRomanStringEncoding] autorelease];
        [s->pb setString:t ? t : @"" forType:NSPasteboardTypeString];
    } else if (f == kScrapFlavorTypeUnicode) {
        NSString *t = [NSString stringWithCharacters:d length:n / 2];
        [s->pb setString:t forType:NSPasteboardTypeString];
    } else {
        NSString *t = flavor_type(f);
        [s->pb addTypes:@[ t ] owner:nil];
        [s->pb setData:[NSData dataWithBytes:d length:n] forType:t];
    }
    RET(noErr);
}

static NSData *scrap_data(CBScrap *s, uint32_t f) {
    if (f == kScrapFlavorTypeText) {
        NSString *t = [s->pb stringForType:NSPasteboardTypeString];
        return [t dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES];
    }
    if (f == kScrapFlavorTypeUnicode) {
        NSString *t = [s->pb stringForType:NSPasteboardTypeString];
        if (!t) return nil;
        NSMutableData *d = [NSMutableData dataWithLength:t.length * 2];
        [t getCharacters:d.mutableBytes range:NSMakeRange(0, t.length)];
        return d;
    }
    return [s->pb dataForType:flavor_type(f)];
}

THUNK(GetScrapFlavorSize) {
    CBScrap *s = CB_UNWRAP(CBScrap, ARG(0));
    if (!s) RET(paramErr);
    NSData *d = scrap_data(s, ARG(1));
    if (!d) RET(noTypeErr);
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)d.length);
    RET(noErr);
}
// GetScrapFlavorData(scrap, flavorType, Size *byteCount (in/out), void *dest)
THUNK(GetScrapFlavorData) {
    CBScrap *s = CB_UNWRAP(CBScrap, ARG(0));
    if (!s) RET(paramErr);
    NSData *d = scrap_data(s, ARG(1));
    if (!d) RET(noTypeErr);
    uint32_t cap = ARG(2) ? gread32(ARG(2)) : 0, n = (uint32_t)d.length;
    if (n > cap) n = cap;
    if (ARG(3)) memcpy(g2h(ARG(3)), d.bytes, n);
    if (ARG(2)) gwrite32(ARG(2), n);
    RET(noErr);
}
THUNK(SetScrapPromiseKeeper) { RET(noErr); }

// --- TSM / AE / printing ----------------------------------------------------------------------------------------

THUNK(NewTSMDocument) {
    if (ARG(2)) gwrite32(ARG(2), gheap_calloc(1, 16));
    RET(noErr);
}
THUNK(DeleteTSMDocument) {
    if (ARG(0)) gheap_free(ARG(0));
    RET(noErr);
}

// AESend(const AppleEvent *, AppleEvent *reply, AESendMode, AESendPriority, SInt32 timeout, idleProc, filterProc)
THUNK(AESend) {
    AEDesc ev;
    aedesc_from_guest(&ev, ARG(0));
    LOGI("carbon: AESend(type %#x, mode %#x)", ev.descriptorType, ARG(2));
    AppleEvent reply = {typeNull, NULL};
    OSErr err = ev.dataHandle ? AESendMessage(&ev, &reply, (AESendMode)ARG(2), (long)(int32_t)ARG(4)) : paramErr;
    if (ARG(1)) aedesc_to_guest(ARG(1), &reply);
    RET((uint32_t)(int32_t)err);
}

THUNK(PMSessionUseSheets) { RET(noErr); }
// PMSessionPrintDialog(session, settings, pageFormat, Boolean *accepted)
THUNK(PMSessionPrintDialog) {
    if (ARG(3)) gwrite8(ARG(3), 0);
    RET(noErr);
}

// --- dock tile ----------------------------------------------------------------------------------------------------

THUNK(SetApplicationDockTileImage) {
    CGImageRef img = (CGImageRef)ARGCF(0);
    if (!img) RET(paramErr);
    cb_on_main(^{
      NSImage *i = [[NSImage alloc] initWithCGImage:img size:NSZeroSize];
      [NSApp setApplicationIconImage:i];
      [i release];
    });
    RET(noErr);
}
THUNK(RestoreApplicationDockTileImage) {
    cb_on_main(^{
      [NSApp setApplicationIconImage:nil];
    });
    RET(noErr);
}

// --- text input sources / keyboard layouts -------------------------------------------------------------------------

// Modern HIToolbox asserts that input sources are queried on the main queue.
static TISInputSourceRef tis_on_main(TISInputSourceRef (*fn)(void)) {
    __block TISInputSourceRef s;
    cb_on_main(^{
      s = fn();
    });
    return s;
}
static void *tis_prop(TISInputSourceRef src, CFStringRef key) {
    __block void *v;
    cb_on_main(^{
      v = TISGetInputSourceProperty(src, key);
    });
    return v;
}
THUNK(TISCopyCurrentKeyboardInputSource) { RET(cfg(tis_on_main(TISCopyCurrentKeyboardInputSource))); }
THUNK(TISCopyCurrentKeyboardLayoutInputSource) { RET(cfg(tis_on_main(TISCopyCurrentKeyboardLayoutInputSource))); }
THUNK(TISCopyCurrentASCIICapableKeyboardInputSource) {
    RET(cfg(tis_on_main(TISCopyCurrentASCIICapableKeyboardInputSource)));
}
THUNK(TISCopyCurrentASCIICapableKeyboardLayoutInputSource) {
    RET(cfg(tis_on_main(TISCopyCurrentASCIICapableKeyboardLayoutInputSource)));
}
THUNK(TISSelectInputSource) {
    TISInputSourceRef src = (TISInputSourceRef)ARGCF(0);
    __block OSStatus st;
    cb_on_main(^{
      st = TISSelectInputSource(src);
    });
    RET(st);
}

static char uchr_key;

// The 'uchr' data must be guest-accessible (the guest passes the bytes to
// UCKeyTranslate): copy it into a CFData allocated from the guest heap.
static CFDataRef guest_uchr(TISInputSourceRef src) {
    CFDataRef g = cf_attached(src, &uchr_key);
    if (g) return g;
    CFDataRef d = tis_prop(src, kTISPropertyUnicodeKeyLayoutData);
    if (!d) return NULL;
    g = CFDataCreate(cf_guest_heap_allocator(), CFDataGetBytePtr(d), CFDataGetLength(d));
    cf_attach(src, &uchr_key, (void *)g, (void (*)(void *))CFRelease);
    return g;
}

// TISGetInputSourceProperty(TISInputSourceRef, CFStringRef key) -> void * (not retained)
THUNK(TISGetInputSourceProperty) {
    TISInputSourceRef src = (TISInputSourceRef)ARGCF(0);
    CFStringRef key = ARGSTR(1);
    if (!src || !key) RET(0);
    if (CFEqual(key, kTISPropertyUnicodeKeyLayoutData)) RET(cfg(guest_uchr(src)));
    RET(cfg(tis_prop(src, key)));
}

THUNK(KLGetCurrentKeyboardLayout) {
    static TISInputSourceRef cur;
    TISInputSourceRef s = tis_on_main(TISCopyCurrentKeyboardLayoutInputSource);
    if (cur) CFRelease(cur);
    cur = s;
    if (ARG(0)) gwrite32(ARG(0), cfg(s));
    RET(s ? noErr : paramErr);
}

// KLGetKeyboardLayoutProperty(KeyboardLayoutRef, tag, const void **outValue)
THUNK(KLGetKeyboardLayoutProperty) {
    TISInputSourceRef src = (TISInputSourceRef)ARGCF(0);
    if (!src || !ARG(2)) RET(paramErr);
    uint32_t v = 0;
    switch (ARG(1)) {
    case kKLuchrData: {
        CFDataRef d = guest_uchr(src);
        if (!d) RET(paramErr);
        v = h2g(CFDataGetBytePtr(d));
        break;
    }
    case kKLKCHRData: RET(paramErr);
    case kKLKind: v = kKLuchrKind; break;
    case kKLIdentifier: v = 0; break;
    case kKLLocalizedName: case kKLName: v = cfg(tis_prop(src, kTISPropertyLocalizedName)); break;
    case kKLLanguageCode: {
        CFArrayRef a = tis_prop(src, kTISPropertyInputSourceLanguages);
        v = a && CFArrayGetCount(a) ? cfg(CFArrayGetValueAtIndex(a, 0)) : 0;
        break;
    }
    default: RET(paramErr);
    }
    gwrite32(ARG(2), v);
    RET(noErr);
}

// --- data constants ----------------------------------------------------------------------------------------------

#define CB_CFCONST(sym)                                                                            \
    static void init_cb_##sym(gaddr_t a) { gwrite32(a, cfg((CFTypeRef)(sym))); }                \
    GDATA(sym, 4, init_cb_##sym)

CB_CFCONST(kTISNotifySelectedKeyboardInputSourceChanged);
CB_CFCONST(kTISPropertyInputSourceIsASCIICapable);
CB_CFCONST(kTISPropertyUnicodeKeyLayoutData);

static void init_content_id(gaddr_t a) {
    gwrite32(a, kHIViewWindowContentID.signature);
    gwrite32(a + 4, (uint32_t)kHIViewWindowContentID.id);
}
static void init_growbox_id(gaddr_t a) {
    gwrite32(a, kHIViewWindowGrowBoxID.signature);
    gwrite32(a + 4, (uint32_t)kHIViewWindowGrowBoxID.id);
}
GDATA(kHIViewWindowContentID, 8, init_content_id);
GDATA(kHIViewWindowGrowBoxID, 8, init_growbox_id);
