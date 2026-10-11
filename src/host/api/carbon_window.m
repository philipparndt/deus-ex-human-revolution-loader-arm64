// Carbon Window Manager emulation on NSWindow.
//
// A WindowRef is a CBWindow; its NSWindow is a CBNSWindow whose content view is
// a flipped view (the Carbon content view, kHIViewWindowContentID). The window's
// root view covers the structure region. Window notifications are turned into
// kEventClassWindow events sent to the window's event target.
#import "carbon_internal.h"

#import <objc/runtime.h>

void cb_quit_app_loop(void);
void cb_run_modal(volatile bool *flag);
void cb_view_window_resized(CBWindow *w);

@interface CBNSWindow : NSWindow {
  @public
    CBWindow *cb; // not retained
}
@end

static NSMutableArray *all_windows; // CBWindow (retained while not disposed)

CBWindow *cb_window_for_ns(NSWindow *w) {
    if ([w isKindOfClass:[CBNSWindow class]]) return ((CBNSWindow *)w)->cb;
    return nil;
}

CBWindow *cb_key_window(void) {
    CBWindow *w = cb_window_for_ns([NSApp keyWindow]);
    if (!w) w = cb_window_for_ns([NSApp mainWindow]);
    return w;
}

CGRect cb_window_content_global(CBWindow *w) {
    NSRect r = [w->ns contentRectForFrameRect:[w->ns frame]];
    return cb_ns_rect_to_global(r);
}
CGRect cb_window_structure_global(CBWindow *w) { return cb_ns_rect_to_global([w->ns frame]); }

static void send_window_event(CBWindow *w, uint32_t kind, void (^fill)(CBEvent *e)) {
    if (!w || w->disposed) return;
    CBEvent *e = cb_event_new(kEventClassWindow, kind, cb_now());
    uint32_t h = cb_handle(w);
    cb_event_set(e, kEventParamDirectObject, typeWindowRef, &h, 4);
    if (fill) fill(e);
    [w retain];
    cb_send(e, w->target);
    [w release];
    [e release];
}

@implementation CBNSWindow
- (BOOL)canBecomeKeyWindow {
    return cb ? !(cb->attrs & kWindowNoActivatesAttribute) : YES;
}
- (BOOL)canBecomeMainWindow {
    return cb ? (cb->wclass == kDocumentWindowClass || cb->wclass == kPlainWindowClass ||
                 cb->wclass == kMovableModalWindowClass || cb->wclass == kModalWindowClass ||
                 cb->wclass == kAlertWindowClass || cb->wclass == kMovableAlertWindowClass ||
                 cb->wclass == kSimpleWindowClass)
              : YES;
}
- (void)performClose:(id)sender {
    if (!cb) {
        [super performClose:sender];
        return;
    }
    send_window_event(cb, kEventWindowClose, nil);
}
- (BOOL)windowShouldClose:(id)sender {
    return NO;
}
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen *)screen {
    // Carbon does not move windows behind the application's back (the game lays out
    // dialogs off-screen at 0,0 and repositions them afterwards).
    return frameRect;
}
- (void)noResponderFor:(SEL)eventSelector {
    // Unhandled keys in a game window should not beep.
}
- (void)setContentView:(NSView *)v {
    NSView *old = [self contentView];
    [super setContentView:v];
    if (!cb || !cb->content || v == old) return;
    CBView *c = cb->content;
    [c->ns release];
    c->ns = [v retain];
    c->container = v;
    for (CBView *s in c->subviews)
        if (s->ns && [s->ns superview] != v) {
            [s->ns removeFromSuperview];
            [v addSubview:s->ns];
            cb_view_set_frame(s, s->frame);
        }
}
@end

// --- creation ----------------------------------------------------------------------------------------

@implementation CBWindow
- (void)dealloc {
    LOGD("carbon: window %#x deallocated", handle);
    if (ns) {
        ((CBNSWindow *)ns)->cb = nil;
        [ns orderOut:nil];
        [ns close];
        [ns release];
    }
    root->window = nil;
    content->window = nil;
    [content release];
    [root release];
    [props release];
    [super dealloc];
}
@end

static NSWindowStyleMask style_for(uint32_t wclass, uint32_t attrs) {
    switch (wclass) {
    case kPlainWindowClass: case kOverlayWindowClass: case kSimpleWindowClass: case kHelpWindowClass:
    case kToolbarWindowClass: case kAltPlainWindowClass:
        return NSWindowStyleMaskBorderless;
    }
    NSWindowStyleMask m = NSWindowStyleMaskTitled;
    if (attrs & kWindowCloseBoxAttribute) m |= NSWindowStyleMaskClosable;
    if (attrs & kWindowCollapseBoxAttribute) m |= NSWindowStyleMaskMiniaturizable;
    if (attrs & kWindowResizableAttribute) m |= NSWindowStyleMaskResizable;
    if (wclass == kUtilityWindowClass || wclass == kFloatingWindowClass) m |= NSWindowStyleMaskUtilityWindow;
    return m;
}

static void apply_attrs(CBWindow *w) {
    if (!w->ns) return;
    NSWindowStyleMask m = style_for(w->wclass, w->attrs);
    if (m != NSWindowStyleMaskBorderless && ([w->ns styleMask] & NSWindowStyleMaskFullScreen))
        m |= NSWindowStyleMaskFullScreen;
    if ([w->ns styleMask] != m) [w->ns setStyleMask:m];
    [w->ns setHasShadow:!(w->attrs & kWindowNoShadowAttribute)];
    [w->ns setIgnoresMouseEvents:(w->attrs & kWindowIgnoreClicksAttribute) != 0];
    [w->ns setHidesOnDeactivate:(w->attrs & kWindowHideOnSuspendAttribute) != 0];
}

CBView *cb_window_make_views(CBWindow *w); // carbon_view.m

static CBWindow *window_new(uint32_t wclass, uint32_t attrs, CGRect content_global) {
    cb_app_init();
    CBWindow *w = [[CBWindow alloc] initWithKind:TK_WINDOW];
    w->wclass = wclass;
    w->attrs = attrs;
    w->props = [[NSMutableDictionary alloc] init];
    w->maxSize = CGSizeMake(FLT_MAX, FLT_MAX);
    if (content_global.size.width < 1) content_global.size.width = 1;
    if (content_global.size.height < 1) content_global.size.height = 1;
    NSRect cr = cb_global_rect_to_ns(content_global);
    CBNSWindow *nw = [[CBNSWindow alloc] initWithContentRect:cr styleMask:style_for(wclass, attrs)
                                                     backing:NSBackingStoreBuffered defer:NO];
    nw->cb = w;
    [nw setReleasedWhenClosed:NO];
    [nw setAcceptsMouseMovedEvents:YES];
    [nw setTabbingMode:NSWindowTabbingModeDisallowed];
    CBFlippedView *cv = [[CBFlippedView alloc] initWithFrame:NSMakeRect(0, 0, cr.size.width, cr.size.height)];
    [nw setContentView:cv];
    [cv release];
    switch (wclass) {
    case kFloatingWindowClass: case kUtilityWindowClass: [nw setLevel:NSFloatingWindowLevel]; break;
    case kHelpWindowClass: [nw setLevel:NSPopUpMenuWindowLevel]; break;
    case kOverlayWindowClass:
        [nw setOpaque:NO];
        [nw setBackgroundColor:[NSColor clearColor]];
        break;
    }
    w->ns = nw;
    apply_attrs(w);
    [nw setFrame:[nw frameRectForContentRect:cr] display:NO];
    LOGD("carbon: window ns content %g,%g %gx%g (screen h %g) -> frame %g,%g", cr.origin.x, cr.origin.y,
         cr.size.width, cr.size.height, cb_main_screen_height(), [nw frame].origin.x, [nw frame].origin.y);
    cb_window_make_views(w);
    if (!all_windows) all_windows = [[NSMutableArray alloc] init];
    [all_windows addObject:w];
    return w;
}

// CreateNewWindow(WindowClass, WindowAttributes, const Rect *contentBounds, WindowRef *out)
THUNK(CreateNewWindow) {
    CGRect rc = cb_get_qdrect(ARG(2));
    __block CBWindow *w;
    cb_on_main(^{
      w = window_new(ARG(0), ARG(1), rc);
    });
    LOGI("carbon: CreateNewWindow(class %u, attrs %#x, %g,%g %gx%g) = %#x", ARG(0), ARG(1), rc.origin.x,
         rc.origin.y, rc.size.width, rc.size.height, cb_handle(w));
    if (ARG(3)) gwrite32(ARG(3), cb_handle(w));
    RET(noErr);
}

static uint32_t attrs_from_bits(gaddr_t list) {
    uint32_t a = 0;
    if (!list) return 0;
    for (int i = 0; i < 64; i++) {
        int32_t b = (int32_t)gread32(list + 4 * i);
        if (!b) break;
        if (b >= 1 && b <= 32) a |= 1u << (b - 1);
    }
    return a;
}

// kHICoordSpaceScreenPixel equals kHICoordSpace72DPIGlobal: since 10.7 Carbon's
// scale factor (HIGetScaleFactor) is always 1.0, Retina backing is not exposed.
static CGRect space_rect_to_global(CBWindow *w, uint32_t space, CGRect r) { return r; }

// HIWindowCreate(WindowClass, const int *attributes, const WindowDefSpec *, HICoordinateSpace, const HIRect *bounds, WindowRef *out)
THUNK(HIWindowCreate) {
    CGRect rc = space_rect_to_global(nil, ARG(3), get_grect(ARG(4)));
    uint32_t attrs = attrs_from_bits(ARG(1));
    __block CBWindow *w;
    cb_on_main(^{
      w = window_new(ARG(0), attrs, rc);
    });
    LOGI("carbon: HIWindowCreate(class %u, attrs %#x, %g,%g %gx%g) = %#x", ARG(0), attrs, rc.origin.x, rc.origin.y,
         rc.size.width, rc.size.height, cb_handle(w));
    if (ARG(5)) gwrite32(ARG(5), cb_handle(w));
    RET(noErr);
}

void cb_window_dispose(CBWindow *w) {
    if (!w || w->disposed) return;
    send_window_event(w, kEventWindowClosed, nil);
    w->disposed = true;
    cb_on_main(^{
      [w->ns orderOut:nil];
    });
    [all_windows removeObjectIdenticalTo:w]; // releases the creation reference
}

#define WIN(i) CB_UNWRAP(CBWindow, ARG(i))

THUNK(DisposeWindow) {
    CBWindow *w = WIN(0);
    LOGD("carbon: DisposeWindow(%#x)", ARG(0));
    if (w) cb_on_main(^{
          cb_window_dispose(w);
        });
}

OSStatus cb_window_default(CBEvent *ev, CBTarget *t) {
    CBWindow *w = t->owner;
    if (!w) return eventNotHandledErr;
    if (ev->cls == kEventClassWindow && ev->kind == kEventWindowClose &&
        (t->standard || (w->attrs & kWindowStandardHandlerAttribute))) {
        cb_window_dispose(w);
        return noErr;
    }
    return eventNotHandledErr;
}

// --- visibility & ordering -----------------------------------------------------------------------------

// --- debugging aids -------------------------------------------------------------------------------
// DXH_CARBON_SHOT=<dir>: save a PNG of every shown window's content one second after
// ShowWindow. DXH_CARBON_CLICK=<title>[,<title>...]: press the first button with that
// title one second after the window is shown (automated testing of dialogs).
static NSButton *find_button(NSView *v, NSString *title) {
    if ([v isKindOfClass:[NSButton class]] && [[(NSButton *)v title] isEqualToString:title]) return (NSButton *)v;
    for (NSView *s in [v subviews]) {
        NSButton *b = find_button(s, title);
        if (b) return b;
    }
    return nil;
}

static void dump_views(NSView *v, int depth) {
    NSRect f = [v frame];
    NSString *t = [v isKindOfClass:[NSButton class]] ? [(NSButton *)v title]
                  : [v isKindOfClass:[NSTextField class]] ? [(NSTextField *)v stringValue] : @"";
    if (t.length > 30) t = [t substringToIndex:30];
    LOGI("carbon: %*s%s %g,%g %gx%g%s%s '%s'", depth * 2, "", class_getName([v class]), f.origin.x, f.origin.y,
         f.size.width, f.size.height, [v isHidden] ? " hidden" : "", [v isFlipped] ? " flipped" : "", t.UTF8String);
    for (NSView *s in [v subviews]) dump_views(s, depth + 1);
}

static void debug_after_show(CBWindow *w) {
    const char *shot = getenv("DXH_CARBON_SHOT"), *click = getenv("DXH_CARBON_CLICK");
    if (!shot && !click) return;
    [w retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      if (!w->disposed && shot) {
          NSView *v = [w->ns contentView];
          dump_views(v, 0);
          NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:[v bounds]];
          [v cacheDisplayInRect:[v bounds] toBitmapImageRep:rep];
          NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
          NSString *path = [NSString stringWithFormat:@"%s/win-%x.png", shot, cb_handle(w)];
          [png writeToFile:path atomically:YES];
          LOGI("carbon: saved window snapshot %s", path.UTF8String);
      }
      if (!w->disposed && click) {
          for (NSString *t in [[NSString stringWithUTF8String:click] componentsSeparatedByString:@","]) {
              NSButton *b = find_button([w->ns contentView], t);
              if (b) {
                  LOGI("carbon: auto-clicking \"%s\"", t.UTF8String);
                  [b performClick:nil];
                  break;
              }
          }
      }
      [w release];
    });
}

static void show(CBWindow *w) {
    send_window_event(w, kEventWindowShowing, nil);
    cb_on_main(^{
      [w->ns orderFront:nil];
      if (![NSApp keyWindow] && [w->ns canBecomeKeyWindow]) [w->ns makeKeyWindow];
    });
    w->visible = true;
    send_window_event(w, kEventWindowShown, nil);
    debug_after_show(w);
}

THUNK(ShowWindow) {
    CBWindow *w = WIN(0);
    LOGD("carbon: ShowWindow(%#x)", ARG(0));
    if (w) show(w);
}
THUNK(HideWindow) {
    CBWindow *w = WIN(0);
    if (!w) return;
    send_window_event(w, kEventWindowHiding, nil);
    cb_on_main(^{
      [w->ns orderOut:nil];
    });
    w->visible = false;
    send_window_event(w, kEventWindowHidden, nil);
}
THUNK(SelectWindow) {
    CBWindow *w = WIN(0);
    if (!w) return;
    cb_on_main(^{
      [NSApp activateIgnoringOtherApps:YES];
      [w->ns makeKeyAndOrderFront:nil];
    });
}
THUNK(BringToFront) {
    CBWindow *w = WIN(0);
    if (w) cb_on_main(^{
          [w->ns orderFront:nil];
        });
}
THUNK(SendBehind) {
    CBWindow *w = WIN(0), *b = WIN(1);
    if (!w) return;
    cb_on_main(^{
      if (b) [w->ns orderWindow:NSWindowBelow relativeTo:[b->ns windowNumber]];
      else [w->ns orderBack:nil];
    });
}
THUNK(IsWindowVisible) {
    CBWindow *w = WIN(0);
    RET(w && [w->ns isVisible]);
}
THUNK(IsWindowActive) {
    CBWindow *w = WIN(0);
    RET(w && ([w->ns isKeyWindow] || [w->ns isMainWindow]));
}
THUNK(IsWindowCollapsed) {
    CBWindow *w = WIN(0);
    RET(w && [w->ns isMiniaturized]);
}
THUNK(IsWindowCollapsable) {
    CBWindow *w = WIN(0);
    RET(w && (w->attrs & kWindowCollapseBoxAttribute));
}
THUNK(CollapseWindow) {
    CBWindow *w = WIN(0);
    if (!w) RET(paramErr);
    bool c = ARG(1) & 0xff;
    cb_on_main(^{
      if (c) [w->ns miniaturize:nil];
      else [w->ns deminiaturize:nil];
    });
    RET(noErr);
}

static bool class_matches(CBWindow *w, uint32_t wclass) { return wclass == kAllWindowClasses || w->wclass == wclass; }

static NSArray *ordered_cb_windows(void) {
    NSMutableArray *a = [NSMutableArray array];
    for (NSWindow *n in [NSApp orderedWindows]) {
        CBWindow *w = cb_window_for_ns(n);
        if (w && !w->disposed) [a addObject:w];
    }
    // windows not on screen follow
    for (CBWindow *w in all_windows)
        if (![a containsObject:w]) [a addObject:w];
    return a;
}

THUNK(GetFrontWindowOfClass) {
    __block gaddr_t res = 0;
    uint32_t wclass = ARG(0);
    bool vis = ARG(1) & 0xff;
    cb_on_main(^{
      for (CBWindow *w in ordered_cb_windows())
          if (class_matches(w, wclass) && (!vis || [w->ns isVisible])) {
              res = cb_handle(w);
              break;
          }
    });
    RET(res);
}

THUNK(GetNextWindow) {
    CBWindow *w = WIN(0);
    __block gaddr_t res = 0;
    cb_on_main(^{
      NSArray *a = ordered_cb_windows();
      NSUInteger i = [a indexOfObjectIdenticalTo:w];
      if (i != NSNotFound && i + 1 < a.count) res = cb_handle(a[i + 1]);
    });
    RET(res);
}

THUNK(ActiveNonFloatingWindow) {
    __block gaddr_t res = 0;
    cb_on_main(^{
      CBWindow *w = cb_window_for_ns([NSApp mainWindow]);
      if (!w) w = cb_window_for_ns([NSApp keyWindow]);
      if (w && w->wclass != kFloatingWindowClass && w->wclass != kUtilityWindowClass) res = cb_handle(w);
    });
    RET(res);
}

// --- attributes ---------------------------------------------------------------------------------------------

THUNK(GetWindowAttributes) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    if (ARG(1)) gwrite32(ARG(1), w->attrs);
    RET(noErr);
}
THUNK(GetWindowClass) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    if (ARG(1)) gwrite32(ARG(1), w->wclass);
    RET(noErr);
}
THUNK(ChangeWindowAttributes) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    w->attrs = (w->attrs | ARG(1)) & ~ARG(2);
    cb_on_main(^{
      apply_attrs(w);
    });
    RET(noErr);
}
THUNK(HIWindowChangeAttributes) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    w->attrs = (w->attrs | attrs_from_bits(ARG(1))) & ~attrs_from_bits(ARG(2));
    cb_on_main(^{
      apply_attrs(w);
    });
    RET(noErr);
}
THUNK(HIWindowTestAttribute) {
    CBWindow *w = WIN(0);
    int32_t b = (int32_t)ARG(1);
    RET(w && b >= 1 && b <= 32 && (w->attrs & (1u << (b - 1))));
}
THUNK(HIWindowChangeAvailability) { RET(noErr); }
THUNK(SetWindowTitleWithCFString) {
    CBWindow *w = WIN(0);
    NSString *s = (NSString *)ARGSTR(1);
    if (!w) RET(errInvalidWindowRef);
    cb_on_main(^{
      [w->ns setTitle:s ? s : @""];
    });
    RET(noErr);
}
THUNK(SetWindowAlpha) {
    CBWindow *w = WIN(0);
    float a = ARGF(1);
    if (!w) RET(errInvalidWindowRef);
    cb_on_main(^{
      [w->ns setAlphaValue:a];
    });
    RET(noErr);
}
static NSColor *rgb_color(gaddr_t c) {
    return [NSColor colorWithCalibratedRed:gread16(c) / 65535.0 green:gread16(c + 2) / 65535.0
                                      blue:gread16(c + 4) / 65535.0 alpha:1.0];
}
THUNK(SetWindowContentColor) {
    CBWindow *w = WIN(0);
    if (!w || !ARG(1)) RET(paramErr);
    NSColor *c = rgb_color(ARG(1));
    cb_on_main(^{
      [w->ns setBackgroundColor:c];
    });
    RET(noErr);
}
THUNK(SetThemeWindowBackground) {
    CBWindow *w = WIN(0);
    if (!w) RET(paramErr);
    cb_on_main(^{
      [w->ns setBackgroundColor:[NSColor windowBackgroundColor]];
    });
    RET(noErr);
}

// --- bounds ------------------------------------------------------------------------------------------------

static CGRect region_rect(CBWindow *w, uint32_t region) {
    switch (region) {
    case kWindowContentRgn: return cb_window_content_global(w);
    case kWindowTitleBarRgn: case kWindowTitleTextRgn: case kWindowDragRgn: {
        CGRect s = cb_window_structure_global(w), c = cb_window_content_global(w);
        return CGRectMake(s.origin.x, s.origin.y, s.size.width, c.origin.y - s.origin.y);
    }
    default: return cb_window_structure_global(w);
    }
}

static void put_space_rect(gaddr_t g, CBWindow *w, uint32_t space, CGRect r) {
    if (space == kHICoordSpaceWindow) {
        CGRect s = cb_window_structure_global(w);
        r.origin.x -= s.origin.x;
        r.origin.y -= s.origin.y;
    }
    put_grect(g, r);
}

// HIWindowGetBounds(window, region, space, HIRect *out)
THUNK(HIWindowGetBounds) {
    CBWindow *w = WIN(0);
    if (!w || !ARG(3)) RET(paramErr);
    __block CGRect rc;
    cb_on_main(^{
      rc = region_rect(w, ARG(1));
    });
    put_space_rect(ARG(3), w, ARG(2), rc);
    LOGD("carbon: HIWindowGetBounds(%#x, rgn %u, space %u) = %g,%g %gx%g", ARG(0), ARG(1), ARG(2), greadf(ARG(3)),
         greadf(ARG(3) + 4), greadf(ARG(3) + 8), greadf(ARG(3) + 12));
    RET(noErr);
}

static void set_window_global(CBWindow *w, uint32_t region, CGRect r) {
    cb_on_main(^{
      NSRect nr = cb_global_rect_to_ns(r);
      if (region == kWindowContentRgn) nr = [w->ns frameRectForContentRect:nr];
      [w->ns setFrame:nr display:YES];
    });
}

// HIWindowSetBounds(window, region, space, const HIRect *)
THUNK(HIWindowSetBounds) {
    CBWindow *w = WIN(0);
    if (!w || !ARG(3)) RET(paramErr);
    CGRect rc = get_grect(ARG(3));
    if (ARG(2) == kHICoordSpaceScreenPixel) rc = space_rect_to_global(w, ARG(2), rc);
    LOGD("carbon: HIWindowSetBounds(%#x, rgn %u, %g,%g %gx%g)", ARG(0), ARG(1), rc.origin.x, rc.origin.y,
         rc.size.width, rc.size.height);
    set_window_global(w, ARG(1), rc);
    RET(noErr);
}

// MoveWindow(window, short hGlobal, short vGlobal, Boolean front)
THUNK(MoveWindow) {
    CBWindow *w = WIN(0);
    if (!w) return;
    int16_t h = (int16_t)ARG(1), v = (int16_t)ARG(2);
    bool front = ARG(3) & 0xff;
    cb_on_main(^{
      CGRect c = cb_window_content_global(w);
      c.origin = CGPointMake(h, v);
      set_window_global(w, kWindowContentRgn, c);
      if (front) [w->ns orderFront:nil];
    });
}
THUNK(MoveWindowStructure) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    int16_t h = (int16_t)ARG(1), v = (int16_t)ARG(2);
    cb_on_main(^{
      CGRect c = cb_window_structure_global(w);
      c.origin = CGPointMake(h, v);
      set_window_global(w, kWindowStructureRgn, c);
    });
    RET(noErr);
}
// RepositionWindow(window, parentWindow, WindowPositionMethod)
THUNK(RepositionWindow) {
    CBWindow *w = WIN(0), *parent = WIN(1);
    uint32_t method = ARG(2);
    if (!w) RET(errInvalidWindowRef);
    cb_on_main(^{
      NSRect f = [w->ns frame];
      NSScreen *screen = [NSScreen mainScreen];
      // kWindowCenterOnParentWindowScreen (3), kWindowCascadeOnParentWindowScreen (6),
      // kWindowAlertPositionOnParentWindowScreen (9): the parent's screen
      if (parent && (method == 3 || method == 6 || method == 9) && [parent->ns screen]) screen = [parent->ns screen];
      NSRect area = [screen visibleFrame]; // below the menu bar, above the Dock
      NSPoint o;
      if (parent && parent != w && (method == 2 || method == 5 || method == 8)) {
          NSRect pf = [parent->ns frame]; // on the parent window itself
          o = NSMakePoint(NSMidX(pf) - f.size.width / 2, NSMidY(pf) - f.size.height / 2);
      } else if (method == 7 || method == 8 || method == 9) {
          // alert position: horizontally centered, upper third
          o = NSMakePoint(NSMidX(area) - f.size.width / 2, NSMaxY(area) - (area.size.height - f.size.height) / 3 - f.size.height);
      } else {
          o = NSMakePoint(NSMidX(area) - f.size.width / 2, NSMidY(area) - f.size.height / 2);
      }
      // keep the title bar reachable
      if (o.y + f.size.height > NSMaxY(area)) o.y = NSMaxY(area) - f.size.height;
      if (o.x < NSMinX(area)) o.x = NSMinX(area);
      [w->ns setFrameOrigin:o];
    });
    LOGD("carbon: RepositionWindow(%#x, parent %#x, method %u)", ARG(0), ARG(1), method);
    RET(noErr);
}

// HIWindowGetGreatestAreaDisplay(window, region, space, CGDirectDisplayID *out, HIRect *outRect)
THUNK(HIWindowGetGreatestAreaDisplay) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    __block CGDirectDisplayID best = CGMainDisplayID();
    uint32_t rgn = ARG(1);
    cb_on_main(^{
      CGRect rc = region_rect(w, rgn);
      double area = -1;
      uint32_t n = 0;
      CGDirectDisplayID ids[16];
      CGGetActiveDisplayList(16, ids, &n);
      for (uint32_t i = 0; i < n; i++) {
          CGRect in = CGRectIntersection(CGDisplayBounds(ids[i]), rc);
          double a = CGRectIsNull(in) ? 0 : in.size.width * in.size.height;
          if (a > area) {
              area = a;
              best = ids[i];
          }
      }
    });
    if (ARG(3)) gwrite32(ARG(3), best);
    if (ARG(4)) put_grect(ARG(4), CGDisplayBounds(best));
    RET(noErr);
}

// HIWindowFindAtLocation(const HIPoint *, space, startWindow, options, WindowRef *out, WindowPartCode *part, HIPoint *outLoc)
THUNK(HIWindowFindAtLocation) {
    CGPoint p = CGPointMake(greadf(ARG(0)), greadf(ARG(0) + 4));
    __block CBWindow *w = nil;
    cb_on_main(^{
      NSInteger num = [NSWindow windowNumberAtPoint:cb_global_to_ns(p) belowWindowWithWindowNumber:0];
      w = cb_window_for_ns([NSApp windowWithWindowNumber:num]);
    });
    if (ARG(4)) gwrite32(ARG(4), cb_handle(w));
    if (!w) {
        if (ARG(5)) gwrite16(ARG(5), inDesk);
        RET(errWindowNotFound);
    }
    CGRect c = cb_window_content_global(w), s = cb_window_structure_global(w);
    if (ARG(5)) gwrite16(ARG(5), CGRectContainsPoint(c, p) ? inContent : inDrag);
    if (ARG(6)) {
        gwritef(ARG(6), p.x - s.origin.x);
        gwritef(ARG(6) + 4, p.y - s.origin.y);
    }
    RET(noErr);
}

THUNK(HIWindowFlush) { RET(noErr); }

THUNK(InvalWindowRect) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    cb_on_main(^{
      [[w->ns contentView] setNeedsDisplay:YES];
    });
    RET(noErr);
}

THUNK(SetWindowResizeLimits) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    if (ARG(1)) w->minSize = CGSizeMake(greadf(ARG(1)), greadf(ARG(1) + 4));
    if (ARG(2)) w->maxSize = CGSizeMake(greadf(ARG(2)), greadf(ARG(2) + 4));
    cb_on_main(^{
      [w->ns setContentMinSize:w->minSize];
      [w->ns setContentMaxSize:w->maxSize];
    });
    RET(noErr);
}
THUNK(GetWindowResizeLimits) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    if (ARG(1)) put_gsize(ARG(1), w->minSize);
    if (ARG(2)) put_gsize(ARG(2), w->maxSize);
    RET(noErr);
}

THUNK(GetWindowEventTarget) {
    CBWindow *w = WIN(0);
    RET(w ? cb_handle(w->target) : 0);
}

// --- properties -----------------------------------------------------------------------------------------

static NSNumber *prop_key(uint32_t creator, uint32_t tag) { return @(((uint64_t)creator << 32) | tag); }

THUNK(SetWindowProperty) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    NSData *d = [NSData dataWithBytes:ARG(4) ? g2h(ARG(4)) : "" length:ARG(4) ? ARG(3) : 0];
    [w->props setObject:d forKey:prop_key(ARG(1), ARG(2))];
    RET(noErr);
}
THUNK(GetWindowProperty) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    NSData *d = [w->props objectForKey:prop_key(ARG(1), ARG(2))];
    if (!d) RET(errWindowPropertyNotFound);
    uint32_t n = (uint32_t)d.length;
    if (ARG(4)) gwrite32(ARG(4), n);
    if (ARG(5)) memcpy(g2h(ARG(5)), d.bytes, n < ARG(3) ? n : ARG(3));
    RET(noErr);
}
THUNK(GetWindowPropertySize) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    NSData *d = [w->props objectForKey:prop_key(ARG(1), ARG(2))];
    if (!d) RET(errWindowPropertyNotFound);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)d.length);
    RET(noErr);
}
THUNK(RemoveWindowProperty) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    [w->props removeObjectForKey:prop_key(ARG(1), ARG(2))];
    RET(noErr);
}

// --- ports ----------------------------------------------------------------------------------------------

static NSMapTable *port_map; // NSNumber(port) -> CBWindow (weak)

gaddr_t cb_port_window(gaddr_t port, CBWindow **out) {
    *out = port_map ? [port_map objectForKey:@(port)] : nil;
    return port;
}

THUNK(GetWindowPort) {
    CBWindow *w = WIN(0);
    if (!w) RET(0);
    if (!w->port) {
        w->port = gheap_calloc(1, 128);
        if (!port_map) port_map = [[NSMapTable strongToWeakObjectsMapTable] retain];
        [port_map setObject:w forKey:@(w->port)];
    }
    RET(w->port);
}
THUNK(SetPortWindowPort) {}

// --- groups ----------------------------------------------------------------------------------------------

@interface CBGroup : CBBase {
  @public
    int32_t level;
}
@end
@implementation CBGroup
@end

THUNK(CreateWindowGroup) {
    CBGroup *g = [[CBGroup alloc] init];
    g->level = kCGNormalWindowLevel;
    if (ARG(1)) gwrite32(ARG(1), cb_handle(g));
    RET(noErr);
}
THUNK(SetWindowGroup) {
    CBWindow *w = WIN(0);
    CBGroup *g = CB_UNWRAP(CBGroup, ARG(1));
    if (!w) RET(errInvalidWindowRef);
    w->group = ARG(1);
    if (g) cb_on_main(^{
          [w->ns setLevel:g->level];
        });
    RET(noErr);
}
THUNK(SetWindowGroupLevel) {
    CBGroup *g = CB_UNWRAP(CBGroup, ARG(0));
    if (!g) RET(paramErr);
    g->level = (int32_t)ARG(1);
    LOGD("carbon: SetWindowGroupLevel(%d)", g->level);
    cb_on_main(^{
      for (CBWindow *w in all_windows)
          if (w->group == ARG(0)) [w->ns setLevel:g->level];
    });
    RET(noErr);
}

// --- screen updates / system UI ---------------------------------------------------------------------------

THUNK(DisableScreenUpdates) { RET(noErr); }
THUNK(EnableScreenUpdates) { RET(noErr); }

THUNK(SetSystemUIMode) {
    uint32_t mode = ARG(0), opts = ARG(1);
    LOGI("carbon: SetSystemUIMode(%u, %#x)", mode, opts);
    __block OSStatus st = noErr;
    cb_on_main(^{
      NSApplicationPresentationOptions o = NSApplicationPresentationDefault;
      switch (mode) {
      case kUIModeContentSuppressed: o = NSApplicationPresentationAutoHideDock; break;
      case kUIModeContentHidden: o = NSApplicationPresentationHideDock; break;
      case kUIModeAllSuppressed:
          o = NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar;
          break;
      case kUIModeAllHidden: o = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar; break;
      }
      if (opts & kUIOptionDisableProcessSwitch) o |= NSApplicationPresentationDisableProcessSwitching;
      if (opts & kUIOptionDisableForceQuit) o |= NSApplicationPresentationDisableForceQuit;
      if (opts & kUIOptionDisableAppleMenu) o |= NSApplicationPresentationDisableAppleMenu;
      if (opts & kUIOptionDisableHide) o |= NSApplicationPresentationDisableHideApplication;
      if (opts & kUIOptionDisableSessionTerminate) o |= NSApplicationPresentationDisableSessionTermination;
      if ((o & NSApplicationPresentationDisableProcessSwitching) &&
          !(o & (NSApplicationPresentationHideDock | NSApplicationPresentationAutoHideDock)))
          o &= ~NSApplicationPresentationDisableProcessSwitching;
      @try {
          [NSApp setPresentationOptions:o];
      } @catch (NSException *x) {
          LOGI("carbon: setPresentationOptions failed: %s", x.reason.UTF8String);
          st = paramErr;
      }
    });
    RET(st);
}

// --- sheets & modality -----------------------------------------------------------------------------------

bool cb_alert_show_sheet(CBWindow *w, CBWindow *parent); // carbon_misc.m

THUNK(ShowSheetWindow) {
    CBWindow *s = WIN(0), *p = WIN(1);
    if (!s || !p) RET(paramErr);
    if (cb_alert_show_sheet(s, p)) RET(noErr);
    cb_on_main(^{
      [p->ns beginSheet:s->ns completionHandler:nil];
    });
    s->visible = true;
    RET(noErr);
}
THUNK(HideSheetWindow) {
    CBWindow *s = WIN(0);
    if (!s) RET(paramErr);
    cb_on_main(^{
      NSWindow *p = [s->ns sheetParent];
      if (p) [p endSheet:s->ns];
      else [s->ns orderOut:nil];
    });
    s->visible = false;
    RET(noErr);
}
THUNK(BeginAppModalStateForWindow) { RET(noErr); }
THUNK(EndAppModalStateForWindow) { RET(noErr); }

THUNK(RunAppModalLoopForWindow) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    LOGI("carbon: RunAppModalLoopForWindow(%#x)", ARG(0));
    [w retain];
    w->modal_quit = false;
    w->modal_running = true;
    cb_on_main(^{
      [NSApp activateIgnoringOtherApps:YES];
      [w->ns makeKeyAndOrderFront:nil];
    });
    cb_run_modal(&w->modal_quit);
    w->modal_running = false;
    [w release];
    LOGI("carbon: RunAppModalLoopForWindow returned");
    RET(noErr);
}
THUNK(QuitAppModalLoopForWindow) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    w->modal_quit = true;
    extern void cb_wake_main(void);
    cb_wake_main(); // the modal loop may be blocked waiting for events
    RET(noErr);
}

THUNK(SetWindowDefaultButton) {
    CBWindow *w = WIN(0);
    CBView *b = CB_UNWRAP(CBView, ARG(1));
    if (!w) RET(errInvalidWindowRef);
    w->defaultButton = b;
    cb_on_main(^{
      if ([b->ns isKindOfClass:[NSButton class]]) {
          [(NSButton *)b->ns setKeyEquivalent:@"\r"];
      }
    });
    RET(noErr);
}
THUNK(SetWindowCancelButton) {
    CBWindow *w = WIN(0);
    CBView *b = CB_UNWRAP(CBView, ARG(1));
    if (!w) RET(errInvalidWindowRef);
    w->cancelButton = b;
    cb_on_main(^{
      if ([b->ns isKindOfClass:[NSButton class]]) [(NSButton *)b->ns setKeyEquivalent:@"\e"];
    });
    RET(noErr);
}
THUNK(SetAutomaticControlDragTrackingEnabledForWindow) { RET(noErr); }

// --- NSWindow bridging --------------------------------------------------------------------------------------

// -[NSWindow initWithWindowRef:] returns the window's own NSWindow.
static id ns_initWithWindowRef(id self, SEL _cmd, void *ref) {
    // The ObjC bridge passes guest handles of opaque pointers unwrapped (the
    // CBWindow itself); host callers would pass the guest-visible pointer.
    CBWindow *w = host_ptr_in_guest(ref) ? CB_UNWRAP(CBWindow, h2g(ref)) : (__bridge CBWindow *)ref;
    LOGI("carbon: -[NSWindow initWithWindowRef:%p] -> %p", ref, w ? w->ns : nil);
    [self release];
    return w ? [w->ns retain] : nil;
}
static void *ns_windowRef(id self, SEL _cmd) {
    CBWindow *w = cb_window_for_ns(self);
    return w ? g2h(cb_handle(w)) : NULL;
}

// --- notifications --------------------------------------------------------------------------------------------

static void bounds_changed(CBWindow *w, uint32_t what) {
    cb_view_window_resized(w);
    CGRect cur = cb_window_content_global(w);
    send_window_event(w, kEventWindowBoundsChanged, ^(CBEvent *e) {
      cb_event_set_u32(e, kEventParamAttributes, typeUInt32, what);
      uint8_t q[8];
      int16_t v[4] = {(int16_t)cur.origin.y, (int16_t)cur.origin.x, (int16_t)(cur.origin.y + cur.size.height),
                      (int16_t)(cur.origin.x + cur.size.width)};
      memcpy(q, v, 8);
      cb_event_set(e, kEventParamCurrentBounds, typeQDRectangle, q, 8);
      cb_event_set(e, kEventParamPreviousBounds, typeQDRectangle, q, 8);
      cb_event_set(e, kEventParamOriginalBounds, typeQDRectangle, q, 8);
    });
}

void cb_window_module_init(void) {
    Class c = [NSWindow class];
    class_replaceMethod(c, @selector(initWithWindowRef:), (IMP)ns_initWithWindowRef, "@@:^v");
    class_replaceMethod(c, @selector(windowRef), (IMP)ns_windowRef, "^v@:");
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
      CBWindow *w = cb_window_for_ns(n.object);
      send_window_event(w, kEventWindowActivated, nil);
      send_window_event(w, kEventWindowFocusAcquired, nil);
    }];
    [nc addObserverForName:NSWindowDidResignKeyNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
      CBWindow *w = cb_window_for_ns(n.object);
      send_window_event(w, kEventWindowFocusRelinquish, nil);
      send_window_event(w, kEventWindowDeactivated, nil);
    }];
    [nc addObserverForName:NSWindowDidMoveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
      CBWindow *w = cb_window_for_ns(n.object);
      if (w) bounds_changed(w, kWindowBoundsChangeOriginChanged);
    }];
    [nc addObserverForName:NSWindowDidResizeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
      CBWindow *w = cb_window_for_ns(n.object);
      if (w) bounds_changed(w, kWindowBoundsChangeSizeChanged);
    }];
    [nc addObserverForName:NSWindowDidMiniaturizeNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  send_window_event(cb_window_for_ns(n.object), kEventWindowCollapsed, nil);
                }];
    [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  send_window_event(cb_window_for_ns(n.object), kEventWindowExpanded, nil);
                }];
}
