// Carbon Event Manager emulation: events and typed parameters (incl. coercions),
// handler chains with CallNextEventHandler, posting/receiving, timers, HIObject
// subclasses, menus and a window with controls.
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <string.h>

#include "libc_check.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

_Static_assert(sizeof(HICommand) == 14, "i386 HICommand (pack 2)");
_Static_assert(sizeof(EventTypeSpec) == 8, "EventTypeSpec");

static int order[8], norder;

static OSStatus h_first(EventHandlerCallRef ref, EventRef ev, void *ud) {
    order[norder++] = (int)(long)ud;
    OSStatus st = CallNextEventHandler(ref, ev); // runs h_second
    order[norder++] = 100 + (int)st;
    return noErr;
}
static OSStatus h_second(EventHandlerCallRef ref, EventRef ev, void *ud) {
    order[norder++] = (int)(long)ud;
    return 7;
}
static OSStatus h_unhandled(EventHandlerCallRef ref, EventRef ev, void *ud) {
    order[norder++] = (int)(long)ud;
    return eventNotHandledErr;
}

static int timer_fired;
static void timer_proc(EventLoopTimerRef t, void *ud) {
    timer_fired += (int)(long)ud;
    QuitApplicationEventLoop();
}

// HIObject subclass
static int constructed, initialized, destructed, inst_seen;
static int my_instance = 1234;
static OSStatus obj_handler(EventHandlerCallRef ref, EventRef ev, void *ud) {
    if (GetEventClass(ev) != kEventClassHIObject) return eventNotHandledErr;
    switch (GetEventKind(ev)) {
    case kEventHIObjectConstruct: {
        constructed++;
        void *p = &my_instance;
        SetEventParameter(ev, kEventParamHIObjectInstance, typeVoidPtr, sizeof p, &p);
        return noErr;
    }
    case kEventHIObjectInitialize: {
        initialized++;
        if (ud == &my_instance) inst_seen++;
        UInt32 v = 0;
        if (GetEventParameter(ev, 'test', typeUInt32, NULL, 4, NULL, &v) == noErr && v == 42) inst_seen++;
        return noErr;
    }
    case kEventHIObjectDestruct: destructed++; return noErr;
    }
    return eventNotHandledErr;
}

static int hit_count;
static OSStatus cmd_handler(EventHandlerCallRef ref, EventRef ev, void *ud) {
    HICommand cmd;
    if (GetEventParameter(ev, kEventParamDirectObject, typeHICommand, NULL, sizeof cmd, NULL, &cmd) == noErr &&
        cmd.commandID == 'tst1')
        hit_count++;
    return noErr;
}

int main(void) {
    // --- events & parameters ---
    EventRef ev = NULL;
    CHECK(CreateEvent(NULL, 'TEST', 1, 0, kEventAttributeNone, &ev) == noErr && ev, "CreateEvent");
    CHECK(GetEventClass(ev) == 'TEST' && GetEventKind(ev) == 1, "class/kind");
    CHECK(GetEventTime(ev) > 0, "event time %f", GetEventTime(ev));
    HIPoint pt = {12.5f, 30.0f};
    CHECK(SetEventParameter(ev, kEventParamMouseLocation, typeHIPoint, sizeof pt, &pt) == noErr, "set point");
    HIPoint pt2 = {0, 0};
    EventParamType at = 0;
    UInt32 sz = 0;
    CHECK(GetEventParameter(ev, kEventParamMouseLocation, typeWildCard, &at, sizeof pt2, &sz, &pt2) == noErr &&
              at == typeHIPoint && sz == 8 && pt2.x == 12.5f && pt2.y == 30.0f,
          "get point (type %x size %u)", (unsigned)at, (unsigned)sz);
    Point qd = {0, 0};
    CHECK(GetEventParameter(ev, kEventParamMouseLocation, typeQDPoint, NULL, sizeof qd, NULL, &qd) == noErr &&
              qd.h == 13 && qd.v == 30,
          "HIPoint -> QDPoint %d,%d", qd.h, qd.v);
    UInt16 b16 = 3;
    SetEventParameter(ev, kEventParamMouseButton, typeMouseButton, sizeof b16, &b16);
    UInt32 b32 = 0;
    CHECK(GetEventParameter(ev, kEventParamMouseButton, typeUInt32, NULL, 4, NULL, &b32) == noErr && b32 == 3,
          "int coercion %u", (unsigned)b32);
    CFStringRef s = CFSTR("hello");
    SetEventParameter(ev, 'strg', typeCFStringRef, sizeof s, &s);
    CFStringRef s2 = NULL;
    CHECK(GetEventParameter(ev, 'strg', typeCFStringRef, NULL, sizeof s2, NULL, &s2) == noErr && s2 &&
              CFStringCompare(s, s2, 0) == kCFCompareEqualTo,
          "CFString param");
    CHECK(GetEventParameter(ev, 'none', typeUInt32, NULL, 4, NULL, &b32) == eventParameterNotFoundErr, "missing");
    CHECK(RemoveEventParameter(ev, 'strg') == noErr, "remove");
    CHECK(RetainEvent(ev) == ev, "retain");
    ReleaseEvent(ev);

    // --- handler chain ---
    EventTypeSpec spec = {'TEST', 1};
    EventHandlerRef h1, h2, h3;
    InstallEventHandler(GetApplicationEventTarget(), h_unhandled, 1, &spec, (void *)3, &h3);
    InstallEventHandler(GetApplicationEventTarget(), h_second, 1, &spec, (void *)2, &h2);
    InstallEventHandler(GetApplicationEventTarget(), h_first, 1, &spec, (void *)1, &h1);
    OSStatus st = SendEventToEventTarget(ev, GetApplicationEventTarget());
    CHECK(st == noErr && norder == 3 && order[0] == 1 && order[1] == 2 && order[2] == 107,
          "chain st %d n %d [%d %d %d]", (int)st, norder, order[0], order[1], order[2]);
    RemoveEventHandler(h1);
    RemoveEventHandler(h2);
    norder = 0;
    st = SendEventToEventTarget(ev, GetApplicationEventTarget());
    CHECK(st == eventNotHandledErr && norder == 1 && order[0] == 3, "after remove st %d n %d", (int)st, norder);
    RemoveEventHandler(h3);

    // --- queue ---
    CHECK(PostEventToQueue(GetMainEventQueue(), ev, kEventPriorityStandard) == noErr, "post");
    EventRef got = NULL;
    EventTypeSpec other = {'XXXX', 9};
    CHECK(ReceiveNextEvent(1, &other, 0.05, true, &got) == eventLoopTimedOutErr, "filtered receive times out");
    CHECK(ReceiveNextEvent(1, &spec, 0.5, true, &got) == noErr && got == ev, "receive posted");
    if (got) ReleaseEvent(got);
    ReleaseEvent(ev);

    EventTime t0 = GetCurrentEventTime();
    // --- timer + RunApplicationEventLoop (quit from the timer) ---
    EventLoopTimerRef tm;
    CHECK(InstallEventLoopTimer(GetMainEventLoop(), 0.05, 0, timer_proc, (void *)5, &tm) == noErr, "timer");
    RunApplicationEventLoop();
    CHECK(timer_fired == 5, "timer fired %d", timer_fired);
    CHECK(GetCurrentEventTime() - t0 >= 0.04, "time advanced %f", GetCurrentEventTime() - t0);
    RemoveEventLoopTimer(tm);

    // --- HIObject subclass ---
    EventTypeSpec ospec[] = {{kEventClassHIObject, kEventHIObjectConstruct},
                             {kEventClassHIObject, kEventHIObjectInitialize},
                             {kEventClassHIObject, kEventHIObjectDestruct}};
    CHECK(HIObjectRegisterSubclass(CFSTR("com.test.obj"), CFSTR("com.apple.hiobject"), 0, obj_handler, 3, ospec, NULL, NULL) ==
              noErr,
          "register");
    EventRef init;
    CreateEvent(NULL, kEventClassHIObject, kEventHIObjectInitialize, 0, 0, &init);
    UInt32 v42 = 42;
    SetEventParameter(init, 'test', typeUInt32, 4, &v42);
    HIObjectRef obj = NULL;
    CHECK(HIObjectCreate(CFSTR("com.test.obj"), init, &obj) == noErr && obj, "HIObjectCreate");
    CHECK(constructed == 1 && initialized == 1 && inst_seen == 2, "construct %d init %d inst %d", constructed,
          initialized, inst_seen);
    CFStringRef cid = HIObjectCopyClassID(obj);
    CHECK(cid && CFStringCompare(cid, CFSTR("com.test.obj"), 0) == kCFCompareEqualTo, "class id");
    CFRelease(obj);
    CHECK(destructed == 1, "destruct %d", destructed);
    ReleaseEvent(init);

    // --- menus ---
    MenuRef m;
    CHECK(CreateNewMenu(128, 0, &m) == noErr, "menu");
    MenuItemIndex idx = 0;
    AppendMenuItemTextWithCFString(m, CFSTR("One"), 0, 'one ', &idx);
    AppendMenuItemTextWithCFString(m, CFSTR("Two"), 0, 'two ', &idx);
    CHECK(idx == 2, "item index %d", idx);
    MenuCommand mc = 0;
    GetMenuItemCommandID(m, 2, &mc);
    CHECK(mc == 'two ', "command id");
    MenuRef om;
    MenuItemIndex oi;
    CHECK(GetIndMenuItemWithCommandID(m, 'one ', 1, &om, &oi) == noErr && om == m && oi == 1, "find command");
    DisableMenuCommand(m, 'two ');
    CHECK(!IsMenuCommandEnabled(m, 'two ') && IsMenuCommandEnabled(m, 'one '), "enable state");

    // --- window, views, controls ---
    Rect r = {100, 100, 300, 400}; // top, left, bottom, right
    WindowRef w = NULL;
    CHECK(CreateNewWindow(kDocumentWindowClass, kWindowStandardDocumentAttributes | kWindowCompositingAttribute, &r,
                          &w) == noErr && w,
          "window");
    HIRect b;
    HIWindowGetBounds(w, kWindowContentRgn, kHICoordSpace72DPIGlobal, &b);
    CHECK(b.origin.x == 100 && b.origin.y == 100 && b.size.width == 300 && b.size.height == 200,
          "content bounds %g,%g %gx%g", b.origin.x, b.origin.y, b.size.width, b.size.height);
    HIViewRef content = NULL;
    CHECK(HIViewFindByID(HIViewGetRoot(w), kHIViewWindowContentID, &content) == noErr && content, "content view");
    Rect cr = {10, 20, 30, 120};
    ControlRef btn = NULL;
    CHECK(CreatePushButtonControl(w, &cr, CFSTR("Press"), &btn) == noErr && btn, "button");
    CHECK(HIViewGetSuperview(btn) == content, "embedded in content");
    Rect gb;
    GetControlBounds(btn, &gb);
    CHECK(gb.top == 10 && gb.left == 20 && gb.bottom == 30 && gb.right == 120, "control bounds");
    HIPoint p = {0, 0};
    HIViewConvertPoint(&p, btn, content);
    CHECK(p.x == 20 && p.y == 10, "convert point %g,%g", p.x, p.y);
    ControlRef cb = NULL;
    Rect cbr = {40, 20, 60, 120};
    CreateCheckBoxControl(w, &cbr, CFSTR("Check"), 1, true, &cb);
    CHECK(GetControl32BitValue(cb) == 1, "checkbox value");
    SetControl32BitValue(cb, 0);
    CHECK(GetControl32BitValue(cb) == 0, "checkbox value set");
    SetControlCommandID(btn, 'tst1');
    EventTypeSpec cspec = {kEventClassCommand, kEventCommandProcess};
    InstallEventHandler(GetWindowEventTarget(w), cmd_handler, 1, &cspec, NULL, NULL);
    HICommandExtended hc = {0};
    hc.commandID = 'tst1';
    EventRef ce;
    CreateEvent(NULL, kEventClassCommand, kEventCommandProcess, 0, 0, &ce);
    SetEventParameter(ce, kEventParamDirectObject, typeHICommand, sizeof hc, &hc);
    SendEventToEventTarget(ce, GetControlEventTarget(btn)); // propagates control -> content -> root -> window
    CHECK(hit_count == 1, "command reached window handler (%d)", hit_count);
    ReleaseEvent(ce);
    SetWindowProperty(w, 'TEST', 'prop', 4, &v42);
    UInt32 pv = 0, psz = 0;
    CHECK(GetWindowProperty(w, 'TEST', 'prop', 4, &psz, &pv) == noErr && pv == 42 && psz == 4, "window property");
    DisposeWindow(w);

    UInt32 keys[4];
    GetKeys((void *)keys);
    CHECK(GetDblTime() > 0, "dbl time");
    DONE();
}
