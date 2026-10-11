// Foundation / AppKit / GameKit / AVFoundation C functions and constants.
#import "objc_internal.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <GameKit/GameKit.h>

// Guest variables holding handles of host string constants.
#define GOBJCONST(sym)                                                                             \
    static void init_##sym(gaddr_t a) { gwrite32(a, o2g((id)sym)); }                               \
    GDATA(sym, 4, init_##sym)

GOBJCONST(NSLocalizedDescriptionKey);
GOBJCONST(NSUnderlyingErrorKey);
GOBJCONST(NSPOSIXErrorDomain);
GOBJCONST(NSFileHFSCreatorCode);
GOBJCONST(NSFileHFSTypeCode);
GOBJCONST(NSFilePosixPermissions);
GOBJCONST(NSFontAttributeName);
GOBJCONST(NSForegroundColorAttributeName);
GOBJCONST(NSParagraphStyleAttributeName);
GOBJCONST(NSShadowAttributeName);
GOBJCONST(NSImageNameCaution);
GOBJCONST(NSWindowDidBecomeMainNotification);
GOBJCONST(NSWindowDidResignMainNotification);
GOBJCONST(NSWindowDidEnterFullScreenNotification);
GOBJCONST(NSWindowDidExitFullScreenNotification);
GOBJCONST(GKErrorDomain);
GOBJCONST(GKPlayerDidChangeNotificationName);
GOBJCONST(AVMediaTypeAudio);
GOBJCONST(AVMediaTypeVideo);
GOBJCONST(NSDefaultRunLoopMode);
GOBJCONST(NSRunLoopCommonModes);
GOBJCONST(NSModalPanelRunLoopMode);
GOBJCONST(NSEventTrackingRunLoopMode);
GOBJCONST(NSApplicationDidFinishLaunchingNotification);
GOBJCONST(NSApplicationWillTerminateNotification);
GOBJCONST(NSWindowWillCloseNotification);
GOBJCONST(NSWindowDidResizeNotification);
GOBJCONST(NSWindowDidMoveNotification);
GOBJCONST(NSFileSize);
GOBJCONST(NSFileModificationDate);
GOBJCONST(NSFileType);
GOBJCONST(NSFileTypeDirectory);
GOBJCONST(NSFileTypeRegular);
GOBJCONST(NSCocoaErrorDomain);

// NSRect constants (4 floats in i386)
GDATA(NSZeroRect, 16, NULL);
GDATA(NSZeroPoint, 8, NULL);
GDATA(NSZeroSize, 8, NULL);


static NSRect arg_rect(struct guest_regs *r, int i) {
    return NSMakeRect(ARGF(i), ARGF(i + 1), ARGF(i + 2), ARGF(i + 3));
}

THUNK(NSRectFill) { NSRectFill(arg_rect(r, 0)); }
THUNK(NSFrameRect) { NSFrameRect(arg_rect(r, 0)); }
THUNK(NSBeep) { NSBeep(); }
THUNK(NSApplicationLoad) {
    BOOL ok = NSApplicationLoad();
    gwrite32(hostsym_lookup("_NSApp"), o2g(NSApp));
    RET(ok);
}
THUNK(NSTemporaryDirectory) { RET(o2g(NSTemporaryDirectory())); }
THUNK(NSHomeDirectory) { RET(o2g(NSHomeDirectory())); }
THUNK(NSUserName) { RET(o2g(NSUserName())); }
THUNK(NSFullUserName) { RET(o2g(NSFullUserName())); }
// NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory, NSSearchPathDomainMask, BOOL)
THUNK(NSSearchPathForDirectoriesInDomains) {
    RET(o2g(NSSearchPathForDirectoriesInDomains(ARG(0), ARG(1), ARG(2) & 0xff)));
}
THUNK(NSStringFromRect) { RET(o2g(NSStringFromRect(arg_rect(r, 0)))); }
THUNK(NSEqualRects) { RET(NSEqualRects(arg_rect(r, 0), arg_rect(r, 4))); }
THUNK(NSPointInRect) {
    RET(NSPointInRect(NSMakePoint(ARGF(0), ARGF(1)), arg_rect(r, 2)));
}
THUNK(NSAllocateMemoryPages) { RET(gheap_memalign(4096, ARG(0))); }
THUNK(NSDeallocateMemoryPages) { gheap_free(ARG(0)); }
THUNK(NSPageSize) { RET(4096); }

// Objective-C retain/release entry points used by blocks capturing objects.
static gaddr_t blk_retain(gaddr_t o) {
    [g2o(o) retain];
    return o;
}
static void blk_release(gaddr_t o) { [g2o(o) release]; }

#include "blocks.h"
__attribute__((constructor)) static void install_block_hooks(void) {
    block_objc_retain = blk_retain;
    block_objc_release = blk_release;
}
