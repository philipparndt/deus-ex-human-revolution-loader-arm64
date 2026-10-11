// HIView / Control Manager emulation on AppKit views.
//
// Every HIViewRef/ControlRef is a CBView. Its AppKit counterpart (ns) is placed
// in the parent's container view; Carbon frames use top-left origins, so
// container views are flipped where we create them (content view, user panes,
// group boxes, tabs). HICocoaViewCreate wraps an existing NSView (the game's
// OpenGL view) and makes it a real subview of the window content.
#import "carbon_internal.h"

static CGRect view_frame(CBView *v);

@implementation CBFlippedView
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) [self setAutoresizesSubviews:NO];
    return self;
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    // Group boxes / tabs: keep their decoration view filling us.
    if (cb && (cb->ctype == CT_GROUPBOX || cb->ctype == CT_TABS)) {
        for (NSView *s in [self subviews])
            if ([s isKindOfClass:[NSBox class]]) {
                NSRect b = [self bounds];
                [s setFrame:cb->ctype == CT_TABS ? NSMakeRect(0, 12, b.size.width, b.size.height - 12) : b];
            }
    }
}
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return cb && cb->ctype == CT_CUSTOM;
}
- (void)drawRect:(NSRect)dirty {
    if (!cb || cb->ctype != CT_CUSTOM) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CBEvent *e = cb_event_new(kEventClassControl, kEventControlDraw, cb_now());
    uint32_t h = cb_handle(cb), c = cfg(ctx);
    cb_event_set(e, kEventParamDirectObject, typeControlRef, &h, 4);
    cb_event_set(e, kEventParamCGContextRef, typeCGContextRef, &c, 4);
    cb_send_noprop(e, cb->target);
    [e release];
}
@end

// --- views ---------------------------------------------------------------------------------------------

@implementation CBView
- (void)cbAction:(id)sender {
    cb_view_sync_from_ns(self);
    [self retain];
    int16_t part = kControlButtonPart;
    switch (ctype) {
    case CT_SLIDER: part = kControlIndicatorPart; break;
    case CT_CHECK: case CT_DISCLOSURE: part = kControlCheckBoxPart; break;
    case CT_POPUP: part = kControlMenuPart; break;
    case CT_TABS: part = (int16_t)value; break;
    }
    if (action) {
        LOGD("carbon: control action proc %#x (part %d)", action, part);
        gcall2(action, cb_handle(self), (uint32_t)(int32_t)part);
    }
    NSEvent *cur = [NSApp currentEvent];
    bool dragging = ctype == CT_SLIDER && cur && [cur type] == NSEventTypeLeftMouseDragged;
    if (!dragging) {
        CBEvent *e = cb_event_new(kEventClassControl, kEventControlHit, cb_now());
        uint32_t h = cb_handle(self);
        cb_event_set(e, kEventParamDirectObject, typeControlRef, &h, 4);
        cb_event_set_u16(e, kEventParamControlPart, typeControlPartCode, (uint16_t)part);
        cb_event_set_u32(e, kEventParamKeyModifiers, typeUInt32, cb_current_modifiers());
        OSStatus st = cb_send(e, target);
        [e release];
        if (command && st != noErr) cb_send_command(command, kHICommandFromControl, 0, 0, h, target);
        CBEvent *vc = cb_event_new(kEventClassControl, kEventControlValueFieldChanged, cb_now());
        cb_event_set(vc, kEventParamDirectObject, typeControlRef, &h, 4);
        cb_send_noprop(vc, target);
        [vc release];
    }
    [self release];
}
- (void)dealloc {
    for (CBView *s in subviews) s->superview = nil;
    [subviews release];
    if (ns && [ns isKindOfClass:[CBFlippedView class]]) ((CBFlippedView *)ns)->cb = nil;
    if ([ns respondsToSelector:@selector(setTarget:)] && [(id)ns target] == self) [(id)ns setTarget:nil];
    [ns removeFromSuperview];
    [ns release];
    [title release];
    [props release];
    [menu release];
    [tabs release];
    [super dealloc];
}
@end

CBView *cb_view_new(int ctype, CBWindow *w) {
    CBView *v = [[CBView alloc] initWithKind:TK_VIEW];
    v->ctype = ctype;
    v->window = w;
    v->subviews = [[NSMutableArray alloc] init];
    v->props = [[NSMutableDictionary alloc] init];
    v->visible = v->enabled = v->active = true;
    v->maximum = 1;
    if (ctype == CT_CUSTOM || ctype == CT_USERPANE || ctype == CT_VIEW) {
        CBFlippedView *f = [[CBFlippedView alloc] initWithFrame:NSZeroRect];
        f->cb = v;
        v->ns = f;
        v->container = f;
    }
    return v;
}

CBView *cb_view_for_ns(NSView *n) {
    if ([n isKindOfClass:[CBFlippedView class]]) return ((CBFlippedView *)n)->cb;
    return nil;
}

static double title_height(CBWindow *w) {
    NSRect f = [w->ns frame], c = [w->ns contentRectForFrameRect:f];
    return f.size.height - c.size.height;
}

CBView *cb_window_make_views(CBWindow *w) {
    CBView *root = cb_view_new(CT_ROOT, w);
    NSView *cv = [w->ns contentView];
    root->container = cv;
    root->sig = 0;
    CBView *content = cb_view_new(CT_CONTENT, w);
    content->ns = [cv retain];
    content->container = cv;
    if ([cv isKindOfClass:[CBFlippedView class]]) ((CBFlippedView *)cv)->cb = content;
    content->sig = kHIViewWindowContentID.signature;
    content->vid = kHIViewWindowContentID.id;
    content->superview = root;
    [root->subviews addObject:content];
    w->root = root;
    w->content = content;
    root->frame = view_frame(root);
    content->frame = view_frame(content);
    return root;
}

// Frame of a view's AppKit container origin within the view (root children live in
// the content view, offset by the title bar).
static CGPoint container_offset(CBView *parent) {
    if (parent->ctype == CT_ROOT && parent->window) return CGPointMake(0, title_height(parent->window));
    return CGPointZero;
}

static NSRect ns_frame_for(CBView *v, CGRect rc) {
    CBView *p = v->superview;
    NSView *c = p ? p->container : nil;
    CGPoint off = p ? container_offset(p) : CGPointZero;
    rc.origin.x -= off.x + (p ? p->boundsOrigin.x : 0);
    rc.origin.y -= off.y + (p ? p->boundsOrigin.y : 0);
    if (c && ![c isFlipped]) rc.origin.y = [c bounds].size.height - rc.origin.y - rc.size.height;
    return NSRectFromCGRect(rc);
}

static CGRect frame_from_ns(CBView *v) {
    CBView *p = v->superview;
    NSView *c = p ? p->container : nil;
    NSRect f = [v->ns frame];
    CGRect rc = NSRectToCGRect(f);
    if (c && ![c isFlipped]) rc.origin.y = [c bounds].size.height - f.origin.y - f.size.height;
    CGPoint off = p ? container_offset(p) : CGPointZero;
    rc.origin.x += off.x + (p ? p->boundsOrigin.x : 0);
    rc.origin.y += off.y + (p ? p->boundsOrigin.y : 0);
    return rc;
}

static CGRect view_frame(CBView *v) {
    if (v->ctype == CT_ROOT && v->window) {
        NSRect f = [v->window->ns frame];
        return CGRectMake(0, 0, f.size.width, f.size.height);
    }
    if (v->ctype == CT_CONTENT && v->window) {
        NSRect f = [[v->window->ns contentView] frame];
        return CGRectMake(0, title_height(v->window), f.size.width, f.size.height);
    }
    if (v->ns && v->superview && [v->ns superview]) return frame_from_ns(v);
    return v->frame;
}


static void put_qd(uint8_t q[8], CGRect r) {
    int16_t v[4] = {(int16_t)lround(r.origin.y), (int16_t)lround(r.origin.x),
                    (int16_t)lround(r.origin.y + r.size.height), (int16_t)lround(r.origin.x + r.size.width)};
    memcpy(q, v, 8);
}

// kEventControlBoundsChanged to the view (the game's layout code listens for it).
static void send_bounds_changed(CBView *v, CGRect old, CGRect cur) {
    if (CGRectEqualToRect(old, cur)) return;
    uint32_t attrs = 0;
    if (!CGSizeEqualToSize(old.size, cur.size)) attrs |= kControlBoundsChangeSizeChanged;
    if (!CGPointEqualToPoint(old.origin, cur.origin)) attrs |= kControlBoundsChangePositionChanged;
    CBEvent *e = cb_event_new(kEventClassControl, kEventControlBoundsChanged, cb_now());
    uint32_t h = cb_handle(v);
    uint8_t qo[8], qc[8];
    put_qd(qo, old);
    put_qd(qc, cur);
    cb_event_set(e, kEventParamDirectObject, typeControlRef, &h, 4);
    cb_event_set_u32(e, kEventParamAttributes, typeUInt32, attrs);
    cb_event_set(e, kEventParamOriginalBounds, typeQDRectangle, qo, 8);
    cb_event_set(e, kEventParamPreviousBounds, typeQDRectangle, qo, 8);
    cb_event_set(e, kEventParamCurrentBounds, typeQDRectangle, qc, 8);
    [v retain];
    cb_send_noprop(e, v->target);
    [v release];
    [e release];
}

static void apply_layouts(CBView *parent, CGSize old, CGSize cur);

void cb_view_set_frame(CBView *v, CGRect rc) {
    __block CGRect old = v->frame;
    if (v->ctype == CT_ROOT || v->ctype == CT_CONTENT) return;
    if (v->ns && v->superview) {
        NSRect nf = ns_frame_for(v, rc);
        cb_on_main(^{
          if ([v->ns superview]) old = frame_from_ns(v);
          [v->ns setFrame:nf];
        });
    }
    v->frame = rc;
    if (!CGSizeEqualToSize(old.size, rc.size)) apply_layouts(v, old.size, rc.size);
    send_bounds_changed(v, old, rc);
}

// HILayoutInfo side bindings to the parent (toView == NULL), applied when the parent resizes.
static void apply_layouts(CBView *parent, CGSize old, CGSize cur) {
    double dw = cur.width - old.width, dh = cur.height - old.height;
    for (CBView *c in [[parent->subviews copy] autorelease]) {
        if (!c->has_layout) continue;
        const uint8_t *l = c->layout;
        uint16_t k[4];
        uint32_t tv[4];
        for (int i = 0; i < 4; i++) {
            memcpy(&tv[i], l + 4 + 10 * i, 4);
            memcpy(&k[i], l + 8 + 10 * i, 2);
        }
        bool T = k[0] && !tv[0], L = k[1] && !tv[1], B = k[2] && !tv[2], R = k[3] && !tv[3];
        CGRect f = view_frame(c);
        if (L && R) f.size.width += dw;
        else if (R) f.origin.x += dw;
        if (T && B) f.size.height += dh;
        else if (B) f.origin.y += dh;
        cb_view_set_frame(c, f);
    }
}

static void set_window_rec(CBView *v, CBWindow *w) {
    v->window = w;
    for (CBView *s in v->subviews) set_window_rec(s, w);
}

void cb_view_add_subview(CBView *parent, CBView *child) {
    if (!parent || !child || parent == child) return;
    [child retain];
    if (child->superview) cb_view_remove(child);
    child->superview = parent;
    [parent->subviews addObject:child];
    set_window_rec(child, parent->window);
    if (child->ns) {
        cb_on_main(^{
          if (parent->ctype == CT_SCROLLVIEW && [parent->ns isKindOfClass:[NSScrollView class]]) {
              [(NSScrollView *)parent->ns setDocumentView:child->ns];
              return;
          }
          NSView *c = parent->container;
          if (c) {
              [child->ns removeFromSuperview];
              [c addSubview:child->ns];
              [child->ns setFrame:ns_frame_for(child, child->frame)];
              [child->ns setHidden:!child->visible];
          }
        });
    }
    [child release];
}

void cb_view_remove(CBView *v) {
    CBView *p = v->superview;
    if (!p) return;
    v->frame = view_frame(v);
    [v retain];
    v->superview = nil;
    [p->subviews removeObjectIdenticalTo:v];
    if (v->ns) cb_on_main(^{
          [v->ns removeFromSuperview];
        });
    [v autorelease];
}

CGPoint cb_view_to_window(CBView *v, CGPoint p) {
    for (CBView *x = v; x && x->ctype != CT_ROOT; x = x->superview) {
        CGRect f = view_frame(x);
        p.x += f.origin.x - x->boundsOrigin.x;
        p.y += f.origin.y - x->boundsOrigin.y;
    }
    return p;
}
CGPoint cb_window_to_view(CBView *v, CGPoint p) {
    CGPoint o = cb_view_to_window(v, CGPointZero);
    return CGPointMake(p.x - o.x, p.y - o.y);
}

void cb_view_window_resized(CBWindow *w) {
    // AppKit autoresizing moved the AppKit views; tell the root and content views.
    if (!w->root || !w->content) return;
    CGRect r = view_frame(w->root), c = view_frame(w->content);
    CGRect ro = w->root->frame, co = w->content->frame;
    w->root->frame = r;
    w->content->frame = c;
    if (!CGSizeEqualToSize(co.size, c.size)) apply_layouts(w->content, co.size, c.size);
    send_bounds_changed(w->root, ro, r);
    send_bounds_changed(w->content, co, c);
}

static void apply_value(CBView *v);
// The tab bar (the box is inserted below it, so it is not the first subview).
static NSSegmentedControl *tabs_bar(NSView *n) {
    for (NSView *sv in [n subviews])
        if ([sv isKindOfClass:[NSSegmentedControl class]]) return (NSSegmentedControl *)sv;
    return nil;
}

static void apply_font_style(CBView *v, gaddr_t rec);
void cb_popup_set_menu(CBView *v, CBMenu *m); // carbon_menu.m

void cb_view_sync_from_ns(CBView *v) {
    NSView *n = v->ns;
    switch (v->ctype) {
    case CT_CHECK: case CT_RADIO: case CT_DISCLOSURE: {
        NSControlStateValue s = [(NSButton *)n state];
        v->value = s == NSControlStateValueOn ? 1 : s == NSControlStateValueMixed ? 2 : 0;
        break;
    }
    case CT_BEVEL:
        if ([(NSButton *)n state] == NSControlStateValueOn) v->value = 1;
        else v->value = 0;
        break;
    case CT_SLIDER: v->value = (int32_t)lround([(NSSlider *)n doubleValue]); break;
    case CT_POPUP: v->value = (int32_t)[(NSPopUpButton *)n indexOfSelectedItem] + 1; break;
    case CT_TABS: {
        NSSegmentedControl *sc = tabs_bar(n);
        if (sc) v->value = (int32_t)[sc selectedSegment] + 1;
        break;
    }
    }
}

static void apply_value(CBView *v) {
    NSView *n = v->ns;
    int32_t val = v->value;
    cb_on_main(^{
      switch (v->ctype) {
      case CT_CHECK: case CT_RADIO: case CT_DISCLOSURE: case CT_BEVEL:
          [(NSButton *)n setState:val == 1 ? NSControlStateValueOn
                                  : val == 2 ? NSControlStateValueMixed
                                             : NSControlStateValueOff];
          break;
      case CT_SLIDER:
          [(NSSlider *)n setMinValue:v->minimum];
          [(NSSlider *)n setMaxValue:v->maximum];
          [(NSSlider *)n setDoubleValue:val];
          break;
      case CT_PROGRESS:
          [(NSProgressIndicator *)n setMinValue:v->minimum];
          [(NSProgressIndicator *)n setMaxValue:v->maximum];
          [(NSProgressIndicator *)n setDoubleValue:val];
          break;
      case CT_POPUP:
          if (v->menu) cb_popup_set_menu(v, v->menu);
          if (val >= 1 && val <= [(NSPopUpButton *)n numberOfItems]) [(NSPopUpButton *)n selectItemAtIndex:val - 1];
          break;
      case CT_TABS: {
          NSSegmentedControl *sc = tabs_bar(n);
          if (sc && val >= 1 && val <= [sc segmentCount])
              [sc setSelectedSegment:val - 1];
          break;
      }
      }
    });
}

#define VIEW(i) CB_UNWRAP(CBView, ARG(i))
#define WIN(i) CB_UNWRAP(CBWindow, ARG(i))

// --- HIView API ---------------------------------------------------------------------------------------

THUNK(HIViewGetRoot) {
    CBWindow *w = WIN(0);
    RET(w ? cb_handle(w->root) : 0);
}
THUNK(GetRootControl) {
    CBWindow *w = WIN(0);
    if (ARG(1)) gwrite32(ARG(1), w ? cb_handle(w->content) : 0);
    RET(w ? noErr : errInvalidWindowRef);
}
THUNK(CreateRootControl) {
    CBWindow *w = WIN(0);
    if (ARG(1)) gwrite32(ARG(1), w ? cb_handle(w->content) : 0);
    RET(w ? noErr : errInvalidWindowRef);
}

static CBView *find_by_id(CBView *v, uint32_t sig, int32_t vid) {
    if (v->sig == sig && v->vid == vid) return v;
    for (CBView *s in v->subviews) {
        CBView *f = find_by_id(s, sig, vid);
        if (f) return f;
    }
    return nil;
}

// HIViewFindByID(start, HIViewID {OSType sig; SInt32 id} (by value), HIViewRef *out)
THUNK(HIViewFindByID) {
    CBView *v = VIEW(0);
    uint32_t sig = ARG(1);
    int32_t vid = (int32_t)ARG(2);
    if (!v) RET(paramErr);
    CBView *f = find_by_id(v, sig, vid);
    if (!f && sig == kHIViewWindowGrowBoxID.signature && vid == kHIViewWindowGrowBoxID.id && v->window) {
        CBView *root = v->window->root;
        f = cb_view_new(CT_VIEW, v->window);
        [f->ns release];
        f->ns = nil;
        f->container = nil;
        f->sig = sig;
        f->vid = vid;
        f->superview = root;
        [root->subviews addObject:f];
        [f release];
    }
    char b[8];
    LOGD("carbon: HIViewFindByID(%s, %d) = %#x", cb_fourcc(sig, b), vid, cb_handle(f));
    if (ARG(3)) gwrite32(ARG(3), cb_handle(f));
    RET(f ? noErr : errUnknownControl);
}
THUNK(HIViewSetID) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    v->sig = ARG(1);
    v->vid = (int32_t)ARG(2);
    RET(noErr);
}

THUNK(HIViewAddSubview) {
    CBView *p = VIEW(0), *c = VIEW(1);
    if (!p || !c) RET(paramErr);
    LOGD("carbon: HIViewAddSubview(%#x, %#x)", ARG(0), ARG(1));
    cb_view_add_subview(p, c);
    RET(noErr);
}
THUNK(HIViewRemoveFromSuperview) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    cb_view_remove(v);
    RET(noErr);
}
THUNK(HIViewGetSuperview) {
    CBView *v = VIEW(0);
    RET(v ? cb_handle(v->superview) : 0);
}
THUNK(HIViewGetWindow) {
    CBView *v = VIEW(0);
    RET(v ? cb_handle(v->window) : 0);
}
THUNK(GetControlOwner) {
    CBView *v = VIEW(0);
    RET(v ? cb_handle(v->window) : 0);
}

THUNK(HIViewGetFrame) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    __block CGRect f;
    cb_on_main(^{
      f = view_frame(v);
    });
    put_grect(ARG(1), f);
    RET(noErr);
}
THUNK(HIViewSetFrame) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    CGRect f = get_grect(ARG(1));
    LOGD("carbon: HIViewSetFrame(%#x, %g,%g %gx%g)", ARG(0), f.origin.x, f.origin.y, f.size.width, f.size.height);
    cb_view_set_frame(v, f);
    RET(noErr);
}
THUNK(HIViewGetBounds) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    __block CGRect f;
    cb_on_main(^{
      f = view_frame(v);
    });
    LOGD("carbon: HIViewGetBounds(%#x) = %gx%g", ARG(0), f.size.width, f.size.height);
    put_grect(ARG(1), CGRectMake(v->boundsOrigin.x, v->boundsOrigin.y, f.size.width, f.size.height));
    RET(noErr);
}
THUNK(HIViewSetBoundsOrigin) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    v->boundsOrigin = CGPointMake(ARGF(1), ARGF(2));
    for (CBView *s in v->subviews) cb_view_set_frame(s, s->frame);
    RET(noErr);
}
THUNK(HIViewSetVisible) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    v->visible = ARG(1) & 0xff;
    if (v->ns && v->ctype != CT_CONTENT) cb_on_main(^{
          [v->ns setHidden:!v->visible];
        });
    RET(noErr);
}
THUNK(HIViewIsLatentlyVisible) {
    CBView *v = VIEW(0);
    RET(v && v->visible);
}
static void needs_display(CBView *v) {
    if (v->ns) cb_on_main(^{
          [v->ns setNeedsDisplay:YES];
        });
}
THUNK(HIViewSetNeedsDisplay) {
    CBView *v = VIEW(0);
    if (v && (ARG(1) & 0xff)) needs_display(v);
    RET(noErr);
}
THUNK(HIViewSetNeedsDisplayInRegion) {
    CBView *v = VIEW(0);
    if (v && (ARG(2) & 0xff)) needs_display(v);
    RET(noErr);
}
THUNK(HIViewSetSubviewsNeedDisplayInShape) {
    CBView *v = VIEW(0);
    if (v && (ARG(2) & 0xff)) {
        needs_display(v);
        for (CBView *s in v->subviews) needs_display(s);
    }
    RET(noErr);
}
THUNK(HIViewRender) {
    CBView *v = VIEW(0);
    if (v && v->ns) cb_on_main(^{
          [v->ns displayIfNeeded];
        });
    RET(noErr);
}
THUNK(HIViewScrollRect) {
    CBView *v = VIEW(0);
    if (v) needs_display(v);
    RET(noErr);
}
THUNK(UpdateControls) {
    CBWindow *w = WIN(0);
    if (w) cb_on_main(^{
          [[w->ns contentView] setNeedsDisplay:YES];
        });
}

static CGPoint win_pt(CBView *v, CGPoint p, bool to_window) {
    if (!v) return p;
    return to_window ? cb_view_to_window(v, p) : cb_window_to_view(v, p);
}

// HIViewConvertPoint(HIPoint *io, HIViewRef src, HIViewRef dst) (NULL = window coordinates)
THUNK(HIViewConvertPoint) {
    CBView *s = VIEW(1), *d = VIEW(2);
    if (!ARG(0)) RET(paramErr);
    __block CGPoint p = CGPointMake(greadf(ARG(0)), greadf(ARG(0) + 4));
    cb_on_main(^{
      p = win_pt(d, win_pt(s, p, true), false);
    });
    gwritef(ARG(0), p.x);
    gwritef(ARG(0) + 4, p.y);
    RET(noErr);
}
THUNK(HIViewConvertRect) {
    CBView *s = VIEW(1), *d = VIEW(2);
    if (!ARG(0)) RET(paramErr);
    __block CGRect rc = get_grect(ARG(0));
    cb_on_main(^{
      rc.origin = win_pt(d, win_pt(s, rc.origin, true), false);
    });
    put_grect(ARG(0), rc);
    RET(noErr);
}

THUNK(HIViewGetLayoutInfo) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    uint32_t ver = gread32(ARG(1));
    if (v->has_layout) memcpy(g2h(ARG(1)) + 4, v->layout + 4, 80);
    else memset(g2h(ARG(1)) + 4, 0, 80);
    gwrite32(ARG(1), ver);
    RET(noErr);
}
THUNK(HIViewSetLayoutInfo) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    memcpy(v->layout, g2h(ARG(1)), 84);
    v->has_layout = true;
    RET(noErr);
}

// HIViewDrawCGImage(CGContextRef, const HIRect *bounds, CGImageRef): draws upright in a flipped context.
THUNK(HIViewDrawCGImage) {
    CGContextRef ctx = (CGContextRef)ARGCF(0);
    CGImageRef img = (CGImageRef)ARGCF(2);
    if (!ctx || !img || !ARG(1)) RET(paramErr);
    CGRect b = get_grect(ARG(1));
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, b.origin.x, b.origin.y + b.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, b.size.width, b.size.height), img);
    CGContextRestoreGState(ctx);
    RET(noErr);
}

THUNK(HIGrowBoxViewSetTransparent) { RET(noErr); }

// HICocoaViewCreate(NSView *, OptionBits, HIViewRef *out)
THUNK(HICocoaViewCreate) {
    id nsv = g2o(ARG(0));
    __block CBView *v;
    cb_on_main(^{
      v = cb_view_new(CT_COCOA, nil);
      if (nsv) {
          v->ns = [nsv retain];
          v->container = nsv;
          v->frame = NSRectToCGRect([nsv frame]);
      }
    });
    LOGI("carbon: HICocoaViewCreate(%#x %s) = %#x", ARG(0), nsv ? class_getName([nsv class]) : "nil", cb_handle(v));
    if (ARG(2)) gwrite32(ARG(2), cb_handle(v));
    RET(noErr);
}
THUNK(HICocoaViewGetView) {
    CBView *v = VIEW(0);
    RET(v && v->ctype == CT_COCOA ? o2g(v->ns) : 0);
}

// HIScrollViewCreate(OptionBits, HIViewRef *out)
THUNK(HIScrollViewCreate) {
    __block CBView *v;
    uint32_t opts = ARG(0);
    cb_on_main(^{
      v = cb_view_new(CT_SCROLLVIEW, nil);
      NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSZeroRect];
      [sv setHasVerticalScroller:(opts & kHIScrollViewOptionsVertScroll) != 0];
      [sv setHasHorizontalScroller:(opts & kHIScrollViewOptionsHorizScroll) != 0];
      [sv setAutohidesScrollers:(opts & kHIScrollViewOptionsAllowGrow) == 0];
      v->ns = sv;
      v->container = nil;
    });
    if (ARG(1)) gwrite32(ARG(1), cb_handle(v));
    RET(noErr);
}

// --- controls ---------------------------------------------------------------------------------------------

static NSString *nsstr(gaddr_t g) {
    NSString *s = g ? (NSString *)gcf_string(g) : nil;
    return s ? s : @"";
}

static CBView *make_control(gaddr_t win, gaddr_t rect, int ctype, NSView *n, NSString *ttl) {
    CBWindow *w = CB_UNWRAP(CBWindow, win);
    CBView *v = cb_view_new(ctype, w);
    if (n) {
        if (v->ns != n) {
            [v->ns release];
            v->ns = [n retain];
            if (!v->container) v->container = n;
        }
        if ([n isKindOfClass:[NSControl class]]) {
            [(NSControl *)n setTarget:v];
            [(NSControl *)n setAction:@selector(cbAction:)];
        }
    }
    v->title = [ttl copy];
    CGRect rc = rect ? cb_get_qdrect(rect) : CGRectMake(0, 0, 20, 20);
    LOGD("carbon: new control type %d \"%s\" at %g,%g %gx%g", ctype, ttl.UTF8String, rc.origin.x, rc.origin.y,
         rc.size.width, rc.size.height);
    v->frame = rc;
    if (w) cb_view_add_subview(w->content, v);
    else v->frame = rc;
    return v;
}

static OSStatus finish_control(struct guest_regs *r, int outArg, CBView *v, const char *what) {
    char rs[64] = "";
    LOGD("carbon: %s = %#x %s", what, cb_handle(v), rs);
    if (ARG(outArg)) gwrite32(ARG(outArg), cb_handle(v));
    return noErr;
}

static NSButton *button(NSString *t, NSButtonType type, NSBezelStyle bezel) {
    NSButton *b = [[[NSButton alloc] initWithFrame:NSZeroRect] autorelease];
    [b setButtonType:type];
    [b setBezelStyle:bezel];
    [b setTitle:t ? t : @""];
    return b;
}

// A view for an HIObject subclass of a built-in view class.
CBView *cb_view_new_of_class(NSString *base) {
    __block CBView *v;
    cb_on_main(^{
      if ([base isEqualToString:@"com.apple.HIBevelButton"]) {
          v = make_control(0, 0, CT_BEVEL, button(@"", NSButtonTypeMomentaryPushIn, NSBezelStyleRegularSquare), @"");
      } else if ([base isEqualToString:@"com.apple.HIPushButton"]) {
          v = make_control(0, 0, CT_PUSH, button(@"", NSButtonTypeMomentaryPushIn, NSBezelStyleRounded), @"");
      } else if ([base isEqualToString:@"com.apple.HICheckBox"]) {
          v = make_control(0, 0, CT_CHECK, button(@"", NSButtonTypeSwitch, NSBezelStyleRegularSquare), @"");
      } else {
          v = cb_view_new(CT_CUSTOM, nil);
      }
    });
    return v;
}

// CreatePushButtonControl(window, rect, title, ControlRef *out)
THUNK(CreatePushButtonControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      v = make_control(ARG(0), ARG(1), CT_PUSH, button(t, NSButtonTypeMomentaryPushIn, NSBezelStyleRounded), t);
    });
    RET(finish_control(r, 3, v, "CreatePushButtonControl"));
}
// CreatePushButtonWithIconControl(window, rect, title, ControlButtonContentInfo *, alignment, ControlRef *out)
THUNK(CreatePushButtonWithIconControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      v = make_control(ARG(0), ARG(1), CT_PUSH, button(t, NSButtonTypeMomentaryPushIn, NSBezelStyleRounded), t);
    });
    RET(finish_control(r, 5, v, "CreatePushButtonWithIconControl"));
}
// CreateCheckBoxControl(window, rect, title, SInt32 initialValue, Boolean autoToggle, ControlRef *out)
THUNK(CreateCheckBoxControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      v = make_control(ARG(0), ARG(1), CT_CHECK, button(t, NSButtonTypeSwitch, NSBezelStyleRegularSquare), t);
      v->value = (int32_t)ARG(3);
      v->maximum = 2;
      apply_value(v);
    });
    RET(finish_control(r, 5, v, "CreateCheckBoxControl"));
}
// CreateRoundButtonControl(window, rect, size, ControlButtonContentInfo *, ControlRef *out)
THUNK(CreateRoundButtonControl) {
    __block CBView *v;
    cb_on_main(^{
      v = make_control(ARG(0), ARG(1), CT_ROUND, button(@"", NSButtonTypeMomentaryPushIn, NSBezelStyleCircular), @"");
    });
    RET(finish_control(r, 4, v, "CreateRoundButtonControl"));
}
// CreateBevelButtonControl(window, rect, title, thickness, behavior, info, menuID, menuBehavior, menuPlacement, ControlRef *out)
THUNK(CreateBevelButtonControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      uint16_t behavior = (uint16_t)ARG(4);
      NSButtonType bt = (behavior & 0x3) == kControlBehaviorToggles   ? NSButtonTypePushOnPushOff
                        : (behavior & 0x3) == kControlBehaviorSticky ? NSButtonTypeOnOff
                                                                     : NSButtonTypeMomentaryPushIn;
      v = make_control(ARG(0), ARG(1), CT_BEVEL, button(t, bt, NSBezelStyleRegularSquare), t);
    });
    RET(finish_control(r, 9, v, "CreateBevelButtonControl"));
}
// CreateDisclosureTriangleControl(window, rect, orientation, title, initialValue, drawTitle, autoToggles, ControlRef *out)
THUNK(CreateDisclosureTriangleControl) {
    __block CBView *v;
    cb_on_main(^{
      NSButton *b = button(@"", NSButtonTypeOnOff, NSBezelStyleDisclosure);
      v = make_control(ARG(0), ARG(1), CT_DISCLOSURE, b, nsstr(ARG(3)));
      v->value = (int32_t)ARG(4);
      apply_value(v);
    });
    RET(finish_control(r, 7, v, "CreateDisclosureTriangleControl"));
}
// CreateStaticTextControl(window, rect, text, const ControlFontStyleRec *, ControlRef *out)
THUNK(CreateStaticTextControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      NSTextField *tf = [NSTextField wrappingLabelWithString:t];
      [tf setSelectable:NO];
      v = make_control(ARG(0), ARG(1), CT_STATICTEXT, tf, t);
      [tf setTarget:nil];
      [tf setAction:NULL];
    });
    RET(finish_control(r, 4, v, "CreateStaticTextControl"));
}
// CreateGroupBoxControl(window, rect, title, Boolean primary, ControlRef *out)
THUNK(CreateGroupBoxControl) {
    __block CBView *v;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      CBFlippedView *f = [[[CBFlippedView alloc] initWithFrame:NSZeroRect] autorelease];
      NSBox *box = [[[NSBox alloc] initWithFrame:NSZeroRect] autorelease];
      [box setTitle:t];
      [box setTitlePosition:t.length ? NSAtTop : NSNoTitle];
      [box setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
      [f addSubview:box];
      v = make_control(ARG(0), ARG(1), CT_GROUPBOX, f, t);
      f->cb = v;
      [box setFrame:[f bounds]];
    });
    RET(finish_control(r, 4, v, "CreateGroupBoxControl"));
}
// CreateUserPaneControl(window, rect, features, ControlRef *out)
THUNK(CreateUserPaneControl) {
    __block CBView *v;
    cb_on_main(^{
      v = make_control(ARG(0), ARG(1), CT_USERPANE, nil, @"");
    });
    RET(finish_control(r, 3, v, "CreateUserPaneControl"));
}
THUNK(CreateSeparatorControl) {
    __block CBView *v;
    cb_on_main(^{
      NSBox *b = [[[NSBox alloc] initWithFrame:NSZeroRect] autorelease];
      [b setBoxType:NSBoxSeparator];
      v = make_control(ARG(0), ARG(1), CT_SEPARATOR, b, @"");
    });
    RET(finish_control(r, 2, v, "CreateSeparatorControl"));
}
THUNK(CreateChasingArrowsControl) {
    __block CBView *v;
    cb_on_main(^{
      NSProgressIndicator *p = [[[NSProgressIndicator alloc] initWithFrame:NSZeroRect] autorelease];
      [p setStyle:NSProgressIndicatorStyleSpinning];
      [p setDisplayedWhenStopped:NO];
      [p startAnimation:nil];
      v = make_control(ARG(0), ARG(1), CT_CHASING, p, @"");
    });
    RET(finish_control(r, 2, v, "CreateChasingArrowsControl"));
}
// CreateProgressBarControl(window, rect, value, min, max, Boolean indeterminate, ControlRef *out)
THUNK(CreateProgressBarControl) {
    __block CBView *v;
    cb_on_main(^{
      NSProgressIndicator *p = [[[NSProgressIndicator alloc] initWithFrame:NSZeroRect] autorelease];
      [p setStyle:NSProgressIndicatorStyleBar];
      [p setIndeterminate:(ARG(5) & 0xff) != 0];
      if (ARG(5) & 0xff) [p startAnimation:nil];
      v = make_control(ARG(0), ARG(1), CT_PROGRESS, p, @"");
      v->value = (int32_t)ARG(2);
      v->minimum = (int32_t)ARG(3);
      v->maximum = (int32_t)ARG(4);
      apply_value(v);
    });
    RET(finish_control(r, 6, v, "CreateProgressBarControl"));
}
// CreateSliderControl(window, rect, value, min, max, orientation, UInt16 numTickMarks, Boolean live, ControlActionUPP, ControlRef *out)
THUNK(CreateSliderControl) {
    __block CBView *v;
    cb_on_main(^{
      NSSlider *s = [[[NSSlider alloc] initWithFrame:NSZeroRect] autorelease];
      uint16_t ticks = (uint16_t)ARG(6);
      if (ticks) {
          [s setNumberOfTickMarks:ticks];
          [s setAllowsTickMarkValuesOnly:YES];
      }
      [s setContinuous:(ARG(7) & 0xff) != 0];
      v = make_control(ARG(0), ARG(1), CT_SLIDER, s, @"");
      v->value = (int32_t)ARG(2);
      v->minimum = (int32_t)ARG(3);
      v->maximum = (int32_t)ARG(4);
      v->action = ARG(8);
      CGRect f = v->frame;
      if (f.size.height > f.size.width) [s setVertical:YES];
      apply_value(v);
    });
    RET(finish_control(r, 9, v, "CreateSliderControl"));
}
// CreatePopupButtonControl(window, rect, title, MenuID, Boolean variableWidth, SInt16 titleWidth, SInt16 just, Style, ControlRef *out)
THUNK(CreatePopupButtonControl) {
    __block CBView *v;
    cb_on_main(^{
      NSPopUpButton *p = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO] autorelease];
      v = make_control(ARG(0), ARG(1), CT_POPUP, p, nsstr(ARG(2)));
      v->minimum = 1;
      v->maximum = 0;
    });
    RET(finish_control(r, 8, v, "CreatePopupButtonControl"));
}

static void tabs_rebuild(CBView *v) {
    NSSegmentedControl *sc = tabs_bar(v->ns);
    if (!sc) return;
    [sc setSegmentCount:(NSInteger)v->tabs.count];
    for (NSUInteger i = 0; i < v->tabs.count; i++) {
        [sc setLabel:v->tabs[i] forSegment:(NSInteger)i];
        [sc setWidth:0 forSegment:(NSInteger)i];
    }
    [sc sizeToFit];
    NSRect b = [v->ns bounds];
    NSRect f = [sc frame];
    [sc setFrameOrigin:NSMakePoint(floor((b.size.width - f.size.width) / 2), 0)];
    v->maximum = (int32_t)v->tabs.count;
    if (v->value >= 1 && v->value <= (int32_t)v->tabs.count) [sc setSelectedSegment:v->value - 1];
}

// Carbon's tab count is the control maximum; titles may arrive (tabi) before it grows.
static void tabs_resize(CBView *v, int32_t n) {
    if (v->ctype != CT_TABS || n < 0 || n > 64) return;
    while ((int32_t)v->tabs.count < n) [v->tabs addObject:@""];
    while ((int32_t)v->tabs.count > n) [v->tabs removeLastObject];
    cb_on_main(^{
      tabs_rebuild(v);
    });
}

// CreateEditUnicodeTextControl(window, rect, CFStringRef text, Boolean isPassword, const ControlFontStyleRec *, ControlRef *out)
// (looked up at run time by the game through CFBundleGetFunctionPointerForName)
THUNK(CreateEditUnicodeTextControl) {
    __block CBView *v;
    bool pw = ARG(3) & 0xff;
    cb_on_main(^{
      NSString *t = nsstr(ARG(2));
      NSTextField *tf = pw ? [[[NSSecureTextField alloc] initWithFrame:NSZeroRect] autorelease]
                           : [[[NSTextField alloc] initWithFrame:NSZeroRect] autorelease];
      [tf setStringValue:t];
      [tf setEditable:YES];
      [tf setSelectable:YES];
      [tf setBezeled:YES];
      [[tf cell] setScrollable:YES];
      [[tf cell] setUsesSingleLineMode:YES];
      v = make_control(ARG(0), ARG(1), CT_EDITTEXT, tf, t);
      [tf setTarget:nil];
      [tf setAction:NULL];
    });
    if (ARG(4)) apply_font_style(v, ARG(4));
    RET(finish_control(r, 5, v, "CreateEditUnicodeTextControl"));
}

// CreateTabsControl(window, rect, size, direction, UInt16 numTabs, const ControlTabEntry *, ControlRef *out)
THUNK(CreateTabsControl) {
    __block CBView *v;
    cb_on_main(^{
      CBFlippedView *f = [[[CBFlippedView alloc] initWithFrame:NSZeroRect] autorelease];
      NSBox *box = [[[NSBox alloc] initWithFrame:NSZeroRect] autorelease];
      [box setTitlePosition:NSNoTitle];
      [box setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
      NSSegmentedControl *sc = [[[NSSegmentedControl alloc] initWithFrame:NSZeroRect] autorelease];
      [sc setSegmentStyle:NSSegmentStyleRounded];
      [sc setTrackingMode:NSSegmentSwitchTrackingSelectOne];
      [sc setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin]; // stays centered
      [f addSubview:sc];
      [f addSubview:box positioned:NSWindowBelow relativeTo:sc];
      v = make_control(ARG(0), ARG(1), CT_TABS, f, @"");
      f->cb = v;
      [sc setTarget:v];
      [sc setAction:@selector(cbAction:)];
      v->tabs = [[NSMutableArray alloc] init];
      uint16_t n = (uint16_t)ARG(4);
      for (uint16_t i = 0; i < n; i++) {
          // ControlTabEntry {ControlButtonContentInfo *icon; CFStringRef name; Boolean enabled} (pack 2: 10 bytes)
          gaddr_t e = ARG(5) + 10u * i;
          [v->tabs addObject:nsstr(gread32(e + 4))];
      }
      v->value = n ? 1 : 0;
      v->minimum = 1;
      tabs_rebuild(v);
      NSRect b = [f bounds];
      [box setFrame:NSMakeRect(0, 12, b.size.width, b.size.height - 12)];
    });
    RET(finish_control(r, 6, v, "CreateTabsControl"));
}

// --- control state ---------------------------------------------------------------------------------------

THUNK(GetControl32BitValue) {
    CBView *v = VIEW(0);
    if (!v) RET(0);
    cb_on_main(^{
      if (v->ns) cb_view_sync_from_ns(v);
    });
    RET((uint32_t)v->value);
}
THUNK(SetControl32BitValue) {
    CBView *v = VIEW(0);
    if (!v) return;
    v->value = (int32_t)ARG(1);
    apply_value(v);
}
THUNK(GetControl32BitMinimum) {
    CBView *v = VIEW(0);
    RET(v ? (uint32_t)v->minimum : 0);
}
THUNK(SetControl32BitMinimum) {
    CBView *v = VIEW(0);
    if (!v) return;
    v->minimum = (int32_t)ARG(1);
    apply_value(v);
}
THUNK(GetControl32BitMaximum) {
    CBView *v = VIEW(0);
    RET(v ? (uint32_t)v->maximum : 0);
}
THUNK(SetControl32BitMaximum) {
    CBView *v = VIEW(0);
    if (!v) return;
    v->maximum = (int32_t)ARG(1);
    if (v->ctype == CT_TABS) tabs_resize(v, v->maximum);
    apply_value(v);
}

static void set_title(CBView *v, NSString *t) {
    [v->title release];
    v->title = [t copy];
    cb_on_main(^{
      NSView *n = v->ns;
      if (v->ctype == CT_GROUPBOX) {
          NSBox *b = [[n subviews] firstObject];
          [b setTitle:t];
          [b setTitlePosition:t.length ? NSAtTop : NSNoTitle];
      } else if ([n isKindOfClass:[NSTextField class]]) {
          [(NSTextField *)n setStringValue:t];
      } else if ([n isKindOfClass:[NSButton class]]) {
          [(NSButton *)n setTitle:t];
      }
    });
}

THUNK(SetControlTitleWithCFString) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    set_title(v, nsstr(ARG(1)));
    RET(noErr);
}
THUNK(CopyControlTitleAsCFString) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    if (ARG(1)) gwrite32(ARG(1), cfg((CFTypeRef)[(v->title ? v->title : @"") copy]));
    RET(noErr);
}

static void set_enabled(CBView *v, bool en) {
    v->enabled = en;
    cb_on_main(^{
      if ([v->ns isKindOfClass:[NSControl class]]) [(NSControl *)v->ns setEnabled:en];
      if (v->ctype == CT_TABS) [tabs_bar(v->ns) setEnabled:en];
    });
    for (CBView *s in v->subviews) {
        // latent enabling: children follow their parent
        cb_on_main(^{
          if ([s->ns isKindOfClass:[NSControl class]]) [(NSControl *)s->ns setEnabled:en && s->enabled];
        });
    }
}
THUNK(EnableControl) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    set_enabled(v, true);
    RET(noErr);
}
THUNK(DisableControl) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    set_enabled(v, false);
    RET(noErr);
}
THUNK(IsControlEnabled) {
    CBView *v = VIEW(0);
    RET(v && v->enabled);
}
THUNK(IsControlActive) {
    CBView *v = VIEW(0);
    RET(v && v->active);
}
THUNK(IsControlHilited) {
    CBView *v = VIEW(0);
    RET(v && v->hilite);
}

THUNK(GetControlBounds) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(ARG(1));
    __block CGRect f;
    cb_on_main(^{
      f = view_frame(v);
    });
    LOGD("carbon: GetControlBounds(%#x) = %g,%g %gx%g", ARG(0), f.origin.x, f.origin.y, f.size.width, f.size.height);
    cb_put_qdrect(ARG(1), f);
    RET(ARG(1));
}
THUNK(SetControlBounds) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) return;
    CGRect f = cb_get_qdrect(ARG(1));
    LOGD("carbon: SetControlBounds(%#x, %g,%g %gx%g)", ARG(0), f.origin.x, f.origin.y, f.size.width, f.size.height);
    cb_view_set_frame(v, f);
}
// GetBestControlRect(control, Rect *out, SInt16 *baseline)
THUNK(GetBestControlRect) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    __block CGRect f;
    cb_on_main(^{
      f = view_frame(v);
      if ([v->ns isKindOfClass:[NSControl class]]) {
          NSSize s = [(NSControl *)v->ns sizeThatFits:NSMakeSize(f.size.width, 10000)];
          if ([v->ns isKindOfClass:[NSTextField class]]) {
              NSTextField *tf = (NSTextField *)v->ns;
              [tf setPreferredMaxLayoutWidth:f.size.width];
              s = [tf intrinsicContentSize];
              if (s.width > f.size.width || s.width <= 0) s.width = f.size.width;
          }
          f.size = NSSizeToCGSize(s);
      }
    });
    LOGD("carbon: GetBestControlRect(%#x) = %g,%g %gx%g", ARG(0), f.origin.x, f.origin.y, f.size.width, f.size.height);
    if (ARG(1)) cb_put_qdrect(ARG(1), f);
    if (ARG(2)) gwrite16(ARG(2), (uint16_t)(int16_t)(-(int16_t)lround(f.size.height * 0.25)));
    RET(noErr);
}

THUNK(SetControlCommandID) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    v->command = ARG(1);
    RET(noErr);
}
THUNK(SetControlAction) {
    CBView *v = VIEW(0);
    if (v) v->action = ARG(1);
}
THUNK(GetControlAction) {
    CBView *v = VIEW(0);
    RET(v ? v->action : 0);
}
THUNK(GetControlEventTarget) {
    CBView *v = VIEW(0);
    RET(v ? cb_handle(v->target) : 0);
}
THUNK(CountSubControls) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    if (ARG(1)) gwrite16(ARG(1), (uint16_t)v->subviews.count);
    RET(noErr);
}
THUNK(GetIndexedSubControl) {
    CBView *v = VIEW(0);
    uint16_t i = (uint16_t)ARG(1);
    if (!v || i < 1 || i > v->subviews.count) RET(errControlIsNotEmbedder);
    if (ARG(2)) gwrite32(ARG(2), cb_handle(v->subviews[i - 1]));
    RET(noErr);
}

THUNK(GetKeyboardFocus) {
    CBWindow *w = WIN(0);
    if (!w) RET(errInvalidWindowRef);
    if (ARG(1)) gwrite32(ARG(1), cb_handle(w->focus));
    RET(noErr);
}
THUNK(SetKeyboardFocus) {
    CBWindow *w = WIN(0);
    CBView *v = VIEW(1);
    if (!w) RET(errInvalidWindowRef);
    w->focus = (int16_t)ARG(2) == kControlFocusNoPart ? nil : v;
    cb_on_main(^{
      if (w->focus && w->focus->ns) [w->ns makeFirstResponder:w->focus->ns];
    });
    RET(noErr);
}

// ControlFontStyleRec (pack 2): flags, font, size, style, mode, just (SInt16 each), RGBColor fore, back
static void apply_font_style(CBView *v, gaddr_t rec) {
    __block int16_t style = (int16_t)gread16(rec + 6);
    int16_t flags = (int16_t)gread16(rec), size = (int16_t)gread16(rec + 4),
            just = (int16_t)gread16(rec + 10), font = (int16_t)gread16(rec + 2);
    cb_on_main(^{
      if (![v->ns isKindOfClass:[NSControl class]]) return;
      NSControl *c = (NSControl *)v->ns;
      CGFloat sz = [[c font] pointSize];
      if ((flags & kControlUseSizeMask) && size > 0) sz = size;
      if ((flags & kControlUseFontMask) && font == kControlFontSmallSystemFont) sz = [NSFont smallSystemFontSize];
      if ((flags & kControlUseFontMask) && font == kControlFontSmallBoldSystemFont)
          style |= bold, sz = [NSFont smallSystemFontSize];
      if ((flags & kControlUseFontMask) && font == kControlFontBigSystemFont) sz = [NSFont systemFontSize];
      NSFont *f = ((flags & kControlUseFaceMask) && (style & bold)) || font == kControlFontSmallBoldSystemFont
                      ? [NSFont boldSystemFontOfSize:sz]
                      : [NSFont systemFontOfSize:sz];
      [c setFont:f];
      if (flags & kControlUseJustMask)
          [c setAlignment:just == teJustCenter    ? NSTextAlignmentCenter
                          : just == teFlushRight ? NSTextAlignmentRight
                                                 : NSTextAlignmentLeft];
      if ((flags & kControlUseForeColorMask) && [c isKindOfClass:[NSTextField class]]) {
          gaddr_t fc = rec + 12;
          [(NSTextField *)c setTextColor:[NSColor colorWithCalibratedRed:gread16(fc) / 65535.0
                                                                   green:gread16(fc + 2) / 65535.0
                                                                    blue:gread16(fc + 4) / 65535.0
                                                                   alpha:1]];
      }
    });
}

THUNK(SetControlFontStyle) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    if (ARG(1)) apply_font_style(v, ARG(1));
    RET(noErr);
}
THUNK(SetControlDragTrackingEnabled) { RET(noErr); }

// --- control data --------------------------------------------------------------------------------------------

static NSNumber *data_key(uint32_t part, uint32_t tag) { return @(((uint64_t)(part & 0xffff) << 32) | tag); }

void cb_popup_set_menu(CBView *v, CBMenu *m); // carbon_menu.m

// SetControlData(control, part, tag, Size, const void *data)
THUNK(SetControlData) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    int16_t part = (int16_t)ARG(1);
    uint32_t tag = ARG(2), size = ARG(3);
    gaddr_t d = ARG(4);
    char b[8];
    LOGD("carbon: SetControlData(%#x, part %d, %s, %u)", ARG(0), part, cb_fourcc(tag, b), size);
    if (v->ctype == CT_EDITTEXT) {
        switch (tag) {
        case kControlEditTextSelectionTag: {
            int16_t a = (int16_t)gread16(d), b2 = (int16_t)gread16(d + 2);
            cb_on_main(^{
              NSText *ed = [[v->ns window] fieldEditor:NO forObject:v->ns];
              NSUInteger len = [[(NSTextField *)v->ns stringValue] length];
              NSUInteger s0 = a < 0 ? 0 : MIN((NSUInteger)a, len), s1 = b2 < 0 ? len : MIN((NSUInteger)b2, len);
              if (ed && [[v->ns window] firstResponder] == ed) [ed setSelectedRange:NSMakeRange(s0, s1 > s0 ? s1 - s0 : 0)];
            });
            RET(noErr);
        }
        case kControlEditTextLockedTag: {
            bool locked = gread8(d) != 0;
            cb_on_main(^{
              [(NSTextField *)v->ns setEditable:!locked];
            });
            RET(noErr);
        }
        case kControlEditTextPasswordCFStringTag:
            if (size >= 4) set_title(v, nsstr(gread32(d)));
            RET(noErr);
        case kControlEditTextSingleLineTag:
            RET(noErr);
        }
    }
    switch (tag) {
    case kControlStaticTextCFStringTag: // also kControlEditTextCFStringTag
        if (size >= 4) set_title(v, nsstr(gread32(d)));
        RET(noErr);
    case kControlStaticTextTextTag: {
        NSString *s = [[[NSString alloc] initWithBytes:g2h(d) length:size encoding:NSMacOSRomanStringEncoding]
            autorelease];
        set_title(v, s ? s : @"");
        RET(noErr);
    }
    case kControlFontStyleTag:
        apply_font_style(v, d);
        RET(noErr);
    case kControlTabInfoTag:
        if (v->ctype == CT_TABS && part >= 1 && part <= 64 && size >= 8) {
            if (part > (int16_t)v->tabs.count) tabs_resize(v, part);
            // ControlTabInfoRecV1 {SInt16 version; SInt16 iconSuiteID; CFStringRef name}
            uint16_t ver = gread16(d);
            NSString *s = ver == kControlTabInfoVersionOne ? nsstr(gread32(d + 4))
                                                           : [[[NSString alloc] initWithBytes:g2h(d + 5)
                                                                                       length:gread8(d + 4)
                                                                                     encoding:NSMacOSRomanStringEncoding]
                                                                 autorelease];
            v->tabs[part - 1] = s ? s : @"";
            cb_on_main(^{
              tabs_rebuild(v);
            });
        }
        RET(noErr);
    case kControlTabEnabledFlagTag:
        if (v->ctype == CT_TABS && part >= 1) {
            bool en = gread8(d) != 0;
            cb_on_main(^{
              NSSegmentedControl *sc = tabs_bar(v->ns);
              if (sc && part <= [sc segmentCount]) [sc setEnabled:en forSegment:part - 1];
            });
        }
        RET(noErr);
    case kControlPopupButtonMenuRefTag: case kControlPopupButtonOwnedMenuRefTag:
        if (v->ctype == CT_POPUP && size >= 4) {
            CBMenu *m = CB_UNWRAP(CBMenu, gread32(d));
            cb_on_main(^{
              cb_popup_set_menu(v, m);
            });
        }
        RET(noErr);
    case kControlPushButtonDefaultTag:
        if ([v->ns isKindOfClass:[NSButton class]] && gread8(d))
            cb_on_main(^{
              [(NSButton *)v->ns setKeyEquivalent:@"\r"];
            });
        RET(noErr);
    case kControlPushButtonCancelTag:
        if ([v->ns isKindOfClass:[NSButton class]] && gread8(d))
            cb_on_main(^{
              [(NSButton *)v->ns setKeyEquivalent:@"\e"];
            });
        RET(noErr);
    case kControlProgressBarIndeterminateTag:
        if (v->ctype == CT_PROGRESS) {
            bool ind = gread8(d) != 0;
            cb_on_main(^{
              [(NSProgressIndicator *)v->ns setIndeterminate:ind];
              if (ind) [(NSProgressIndicator *)v->ns startAnimation:nil];
            });
        }
        RET(noErr);
    case kControlProgressBarAnimatingTag: // == kControlChasingArrowsAnimatingTag
        if (v->ctype == CT_PROGRESS || v->ctype == CT_CHASING) {
            bool an = gread8(d) != 0;
            cb_on_main(^{
              if (an) [(NSProgressIndicator *)v->ns startAnimation:nil];
              else [(NSProgressIndicator *)v->ns stopAnimation:nil];
            });
        }
        break;
    }
    // Unknown tags: remember the bytes so GetControlData returns them.
    NSData *nd = [NSData dataWithBytes:d ? g2h(d) : "" length:d ? size : 0];
    [v->props setObject:nd forKey:data_key((uint32_t)part, tag)];
    RET(noErr);
}

static NSString *edit_text(CBView *v) {
    if (v->ctype == CT_EDITTEXT && [v->ns isKindOfClass:[NSTextField class]]) return [(NSTextField *)v->ns stringValue];
    return v->title ? v->title : @"";
}

static NSData *control_data(CBView *v, int16_t part, uint32_t tag) {
    if (v->ctype == CT_EDITTEXT) {
        switch (tag) {
        case kControlEditTextCFStringTag: case kControlEditTextPasswordCFStringTag: {
            uint32_t h = cfg((CFTypeRef)[edit_text(v) copy]);
            return [NSData dataWithBytes:&h length:4];
        }
        case kControlEditTextTextTag: case kControlEditTextPasswordTag: {
            NSData *d = [edit_text(v) dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES];
            return d ? d : [NSData data];
        }
        case kControlEditTextSelectionTag: {
            NSText *ed = [[v->ns window] fieldEditor:NO forObject:v->ns];
            NSRange rg = ed && [[v->ns window] firstResponder] == ed ? [ed selectedRange] : NSMakeRange(0, 0);
            int16_t sel[2] = {(int16_t)rg.location, (int16_t)(rg.location + rg.length)};
            return [NSData dataWithBytes:sel length:4];
        }
        }
    }
    switch (tag) {
    case kControlStaticTextCFStringTag: {
        uint32_t h = cfg((CFTypeRef)[(v->title ? v->title : @"") copy]);
        return [NSData dataWithBytes:&h length:4];
    }
    case kControlStaticTextTextTag: {
        NSData *d = [(v->title ? v->title : @"") dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES];
        return d ? d : [NSData data];
    }
    case kControlTabInfoTag:
        if (v->ctype == CT_TABS && part >= 1 && part <= (int16_t)v->tabs.count) {
            uint8_t rec[8] = {0};
            uint16_t ver = kControlTabInfoVersionOne;
            memcpy(rec, &ver, 2);
            uint32_t h = cfg((CFTypeRef)[v->tabs[part - 1] copy]);
            memcpy(rec + 4, &h, 4);
            return [NSData dataWithBytes:rec length:8];
        }
        return nil;
    case kControlPopupButtonMenuRefTag: case kControlPopupButtonOwnedMenuRefTag: {
        uint32_t h = cb_handle(v->menu);
        return [NSData dataWithBytes:&h length:4];
    }
    }
    return [v->props objectForKey:data_key((uint32_t)part, tag)];
}

// GetControlData(control, part, tag, Size bufferSize, void *buffer, Size *actualSize)
THUNK(GetControlData) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    __block NSData *d;
    cb_on_main(^{
      d = [control_data(v, (int16_t)ARG(1), ARG(2)) retain];
    });
    [d autorelease];
    char b[8];
    if (!d) {
        LOGD("carbon: GetControlData(%s): not supported", cb_fourcc(ARG(2), b));
        RET(errDataNotSupported);
    }
    uint32_t n = (uint32_t)d.length;
    if (ARG(5)) gwrite32(ARG(5), n);
    if (ARG(4)) memcpy(g2h(ARG(4)), d.bytes, n < ARG(3) ? n : ARG(3));
    RET(noErr);
}
THUNK(GetControlDataSize) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    NSData *d = control_data(v, (int16_t)ARG(1), ARG(2));
    if (!d) RET(errDataNotSupported);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)d.length);
    RET(noErr);
}

// --- control properties ----------------------------------------------------------------------------------

static NSNumber *prop_key(uint32_t c, uint32_t t) { return @(((uint64_t)c << 32) | t | (1ULL << 63)); }

THUNK(SetControlProperty) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    NSData *d = [NSData dataWithBytes:ARG(4) ? g2h(ARG(4)) : "" length:ARG(4) ? ARG(3) : 0];
    [v->props setObject:d forKey:prop_key(ARG(1), ARG(2))];
    RET(noErr);
}
THUNK(GetControlProperty) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    NSData *d = [v->props objectForKey:prop_key(ARG(1), ARG(2))];
    if (!d) RET(controlPropertyNotFoundErr);
    uint32_t n = (uint32_t)d.length;
    if (ARG(4)) gwrite32(ARG(4), n);
    if (ARG(5)) memcpy(g2h(ARG(5)), d.bytes, n < ARG(3) ? n : ARG(3));
    RET(noErr);
}
THUNK(GetControlPropertySize) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    NSData *d = [v->props objectForKey:prop_key(ARG(1), ARG(2))];
    if (!d) RET(controlPropertyNotFoundErr);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)d.length);
    RET(noErr);
}
THUNK(RemoveControlProperty) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    [v->props removeObjectForKey:prop_key(ARG(1), ARG(2))];
    RET(noErr);
}

// --- help tags -------------------------------------------------------------------------------------------------

// HMHelpContentRec (pack 2): SInt32 version; Rect absHotRect; HMTagDisplaySide tagSide (SInt16);
// HMHelpContent content[2] at 14, each {HMContentType contentType; union (256 bytes)}
THUNK(HMSetControlHelpContent) {
    CBView *v = VIEW(0);
    if (!v) RET(paramErr);
    v->help = 0;
    if (ARG(1)) {
        gaddr_t c = ARG(1) + 14;
        uint32_t type = gread32(c);
        NSString *tip = nil;
        if (type == kHMCFStringContent || type == kHMCFStringLocalizedContent) tip = nsstr(gread32(c + 4));
        else if (type == kHMPascalStrContent)
            tip = [[[NSString alloc] initWithBytes:g2h(c + 5) length:gread8(c + 4)
                                          encoding:NSMacOSRomanStringEncoding] autorelease];
        cb_on_main(^{
          [v->ns setToolTip:tip.length ? tip : nil];
        });
        [v->props setObject:[NSData dataWithBytes:g2h(ARG(1)) length:534] forKey:@"help"];
    }
    RET(noErr);
}
THUNK(HMGetControlHelpContent) {
    CBView *v = VIEW(0);
    if (!v || !ARG(1)) RET(paramErr);
    NSData *d = [v->props objectForKey:@"help"];
    if (d) memcpy(g2h(ARG(1)), d.bytes, 534);
    else {
        memset(g2h(ARG(1)) + 4, 0, 530);
        gwrite32(ARG(1) + 14, kHMNoContent);
        gwrite32(ARG(1) + 14 + 260, kHMNoContent);
    }
    RET(noErr);
}

OSStatus cb_view_default(CBEvent *ev, CBTarget *t) { return eventNotHandledErr; }
