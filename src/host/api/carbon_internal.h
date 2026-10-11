// Carbon (HIToolbox) emulation on top of AppKit — shared definitions.
//
// HIToolbox does not exist for 64-bit processes, so every Carbon object the
// guest sees is a host Objective-C object (manual retain/release) wrapped in a
// hobj handle:
//
//   CBTarget   EventTargetRef (owned by the application, the dispatcher, each
//              window, view/control, menu and HIObject)
//   CBHandler  EventHandlerRef
//   CBEvent    EventRef (class/kind/time + typed parameters stored in i386 layout)
//   CBObject   HIObjectRef base; CBWindow (WindowRef), CBView (HIViewRef/ControlRef),
//              CBMenu (MenuRef) derive from it
//   CBTimer    EventLoopTimerRef (CFRunLoopTimer firing a guest callback)
//
// Because the objects are NSObjects, guest CFRetain/CFRelease on WindowRef,
// MenuRef, HIObjectRef, EventRef work as well.
//
// i386 layouts: Carbon structs use mac68k alignment (#pragma pack(2)); HIPoint,
// HISize and HIRect are float based; QuickDraw Rect is {SInt16 top, left, bottom,
// right}, Point {SInt16 v, h}. Carbon global coordinates have their origin at the
// top-left of the main display with y growing downwards.
#pragma once
#import "objc_internal.h"
#include "cg_internal.h"

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// --- small helpers -----------------------------------------------------------------------------
#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
const char *cb_fourcc(uint32_t v, char buf[8]);

// Unwrap a guest handle into a host object of the given class (nil if not).
id cb_unwrap(gaddr_t g, Class cls);
#define CB_UNWRAP(cls, g) ((cls *)cb_unwrap((g), [cls class]))
gaddr_t cb_handle(id o); // hobj_wrap(o) (0 for nil)

// Run AppKit work on the main thread (synchronously).
void cb_on_main(void (^blk)(void));

// QuickDraw Rect <-> CGRect (Carbon coordinates).
static inline CGRect cb_get_qdrect(gaddr_t g) {
    int16_t t = (int16_t)gread16(g), l = (int16_t)gread16(g + 2), b = (int16_t)gread16(g + 4),
            rr = (int16_t)gread16(g + 6);
    return CGRectMake(l, t, rr - l, b - t);
}
static inline void cb_put_qdrect(gaddr_t g, CGRect rc) {
    gwrite16(g, (uint16_t)(int16_t)lround(rc.origin.y));
    gwrite16(g + 2, (uint16_t)(int16_t)lround(rc.origin.x));
    gwrite16(g + 4, (uint16_t)(int16_t)lround(rc.origin.y + rc.size.height));
    gwrite16(g + 6, (uint16_t)(int16_t)lround(rc.origin.x + rc.size.width));
}

// Coordinates: Carbon global (top-left of the main display, y down) <-> Cocoa screen.
double cb_main_screen_height(void);
NSPoint cb_global_to_ns(CGPoint p);
CGPoint cb_ns_to_global(NSPoint p);
NSRect cb_global_rect_to_ns(CGRect rc);
CGRect cb_ns_rect_to_global(NSRect rc);

// The application is set up (NSApplication, activation policy, finishLaunching).
void cb_app_init(void);

// --- event targets & handlers ------------------------------------------------------------------
enum { TK_APP, TK_DISPATCHER, TK_WINDOW, TK_VIEW, TK_MENU, TK_OBJECT };

@class CBEvent, CBWindow, CBView, CBMenu, CBObject;

// Root class: caches the guest handle and forgets it on dealloc.
@interface CBBase : NSObject {
  @public
    gaddr_t handle;
}
@end

@interface CBTarget : CBBase {
  @public
    int kind;
    id owner;                  // not retained (the owner retains the target)
    NSMutableArray *handlers;  // CBHandler, newest first
    bool standard;             // InstallStandardEventHandler was called
}
- (instancetype)initWithKind:(int)k owner:(id)o;
@end

@interface CBHandler : CBBase {
  @public
    CBTarget *target; // not retained
    gaddr_t upp, udata;
    uint32_t ntypes;
    uint32_t *types; // pairs {class, kind}
    bool removed;
}
@end

// Host default handlers appended to a dispatch chain.
typedef OSStatus (*cb_default_fn)(CBEvent *ev, CBTarget *t);

// Parameter storage. Data is kept in guest (i386) layout.
@interface CBParam : NSObject {
  @public
    uint32_t type;
    NSData *data;
    CFTypeRef cf; // retained CF object for CF-typed parameters
}
@end

@interface CBEvent : CBBase {
  @public
    uint32_t cls, kind;
    double time;
    uint32_t attrs;
    NSMutableDictionary *params; // NSNumber(name) -> CBParam
    NSEvent *ns;                 // source AppKit event (retained), if any
    bool ns_forwarded;
    bool queued;
    int priority;
}
@end

CBEvent *cb_event_new(uint32_t cls, uint32_t kind, double when); // +1
void cb_event_set(CBEvent *e, uint32_t name, uint32_t type, const void *data, uint32_t size);
// Find a parameter (actual type and bytes); NULL if absent.
CBParam *cb_event_param(CBEvent *e, uint32_t name);
bool cb_event_get_u32(CBEvent *e, uint32_t name, uint32_t *out);
static inline void cb_event_set_u32(CBEvent *e, uint32_t name, uint32_t type, uint32_t v) {
    cb_event_set(e, name, type, &v, 4);
}
static inline void cb_event_set_u16(CBEvent *e, uint32_t name, uint32_t type, uint16_t v) {
    cb_event_set(e, name, type, &v, 2);
}
static inline void cb_event_set_hipoint(CBEvent *e, uint32_t name, CGPoint p) {
    float f[2] = {(float)p.x, (float)p.y};
    cb_event_set(e, name, typeHIPoint, f, 8);
}

double cb_now(void); // GetCurrentEventTime

CBTarget *cb_app_target(void);
CBTarget *cb_dispatcher_target(void);
CBTarget *cb_target_parent(CBTarget *t);
// Send an event to a target (handler chain + default handlers). Main thread or any.
OSStatus cb_send(CBEvent *e, CBTarget *t);
OSStatus cb_send_noprop(CBEvent *e, CBTarget *t); // kEventTargetDontPropagate
// Post to the main event queue (retains).
void cb_post(CBEvent *e);
// Install a guest handler on a target.
CBHandler *cb_install_handler(CBTarget *t, gaddr_t upp, uint32_t ntypes, const uint32_t *types, gaddr_t udata);

// Keyboard/mouse state helpers.
uint32_t cb_modifiers_from_ns(NSEventModifierFlags f);
uint32_t cb_current_modifiers(void);

// Send a kEventCommandProcess for an HICommand.
OSStatus cb_send_command(uint32_t command, uint32_t attributes, gaddr_t menu, uint16_t item, gaddr_t control,
                         CBTarget *t);

// --- HIObject base -----------------------------------------------------------------------------
@interface CBObject : CBBase {
  @public
    CBTarget *target;
    NSString *classID;      // HIObject class id (registered subclass or built-in)
    gaddr_t instance;       // guest instance data of a custom HIObject subclass
    bool destructing;
}
- (instancetype)initWithKind:(int)k;
@end

// Called when a CBObject subclass with a registered HIObject class is created.
OSStatus cb_hiobject_construct(CBObject *o, NSString *classID, gaddr_t initEvent);

// --- windows -----------------------------------------------------------------------------------
@interface CBWindow : CBObject {
  @public
    NSWindow *ns;
    uint32_t wclass, attrs;
    CBView *root, *content;
    NSMutableDictionary *props; // NSNumber(creator<<32|tag) -> NSData
    CGSize minSize, maxSize;
    CBView *focus;
    gaddr_t group;
    gaddr_t port;
    CBView *defaultButton, *cancelButton;
    bool modal_running, modal_quit;
    bool disposed;
    bool visible;
}
@end
CBWindow *cb_window_for_ns(NSWindow *w);
CBWindow *cb_key_window(void);
// Window content origin in Carbon global coords / structure origin.
CGRect cb_window_content_global(CBWindow *w);
CGRect cb_window_structure_global(CBWindow *w);
void cb_window_dispose(CBWindow *w);

// --- views / controls --------------------------------------------------------------------------
enum {
    CT_VIEW = 0, CT_ROOT, CT_CONTENT, CT_COCOA, CT_USERPANE, CT_PUSH, CT_CHECK, CT_RADIO, CT_POPUP,
    CT_STATICTEXT, CT_TABS, CT_SLIDER, CT_PROGRESS, CT_CHASING, CT_GROUPBOX, CT_BEVEL, CT_ROUND,
    CT_SEPARATOR, CT_DISCLOSURE, CT_SCROLLVIEW, CT_CUSTOM, CT_EDITTEXT,
};

@interface CBView : CBObject {
  @public
    int ctype;
    CBWindow *window;   // not retained
    CBView *superview;  // not retained
    NSMutableArray *subviews;
    NSView *ns;         // outer AppKit view (positioned in the parent's container)
    NSView *container;  // where subviews' AppKit views go (flipped)
    CGRect frame;       // in the superview's coordinates (top-left origin)
    CGPoint boundsOrigin;
    uint32_t sig;
    int32_t vid;
    bool visible, enabled, active, hilite;
    int32_t value, minimum, maximum;
    NSString *title;
    uint32_t command;
    gaddr_t action;
    NSMutableDictionary *props;
    uint8_t layout[84];
    bool has_layout;
    CBMenu *menu;
    NSMutableArray *tabs; // tab titles
    gaddr_t refcon;
    gaddr_t help;
}
@end
// Flipped AppKit view used for content views, user panes and custom HIViews.
@interface CBFlippedView : NSView {
  @public
    CBView *cb; // not retained
}
@end

CBView *cb_view_new(int ctype, CBWindow *w); // +1
void cb_view_add_subview(CBView *parent, CBView *child);
void cb_view_remove(CBView *v);
void cb_view_set_frame(CBView *v, CGRect rc);
CBView *cb_view_for_ns(NSView *n);
// Convert a point from view coordinates to window structure coordinates.
CGPoint cb_view_to_window(CBView *v, CGPoint p);
CGPoint cb_window_to_view(CBView *v, CGPoint p);
void cb_view_sync_from_ns(CBView *v); // value from AppKit control

// --- menus ---------------------------------------------------------------------------------------
@interface CBMenu : CBObject {
  @public
    NSMenu *ns;
    uint16_t menuID;
    uint32_t mattrs;
}
@end
CBMenu *cb_menu_for_ns(NSMenu *m);
CBMenu *cb_menu_new(uint16_t mid, NSMenu *existing); // +1
