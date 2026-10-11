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

// The process's main bundle must be the game's .app (Info.plist, resources,
// data paths next to the bundle), not dxhost's. Guest code reaches NSBundle
// through the bridge, and AppKit itself also asks +mainBundle.
#include "cf_bridge.h"
static NSBundle *game_bundle;
static NSBundle *dx_main_bundle(id self, SEL _cmd) {
    (void)self, (void)_cmd;
    return game_bundle;
}
// The game's language, as Steam would launch it: DXH_LANGUAGE (code or Steam
// name) or Contents/Resources/steam_language written by the language depot.
static NSString *game_language(NSBundle *bundle) {
    NSString *l = nil;
    const char *e = getenv("DXH_LANGUAGE");
    if (e && *e) l = [NSString stringWithUTF8String:e];
    if (!l.length) {
        NSString *p = [bundle pathForResource:@"steam_language" ofType:nil];
        l = p ? [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:NULL] : nil;
    }
    l = [[l stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] lowercaseString];
    NSDictionary *names = @{@"english": @"en", @"german": @"de", @"french": @"fr", @"italian": @"it",
                            @"spanish": @"es", @"russian": @"ru", @"polish": @"pl", @"japanese": @"ja"};
    if (names[l]) l = names[l];
    return l.length ? l : nil;
}

// Steam's name for the game language ("german"), or NULL to ask Steam.
static char game_language_name[32];
static char game_language_code[16];
const char *dx_game_language_name(void) { return game_language_name[0] ? game_language_name : NULL; }
const char *dx_game_language_code(void) { return game_language_code[0] ? game_language_code : NULL; }

void objc_install_main_bundle(void) {
    // A game must keep running in the background (music, movies, network): no
    // App Nap timer throttling. Stalls also trip game bugs (see patches.c).
    static id activity;
    if (!activity)
        activity = [[[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"game"] retain];
    CFBundleRef b = cf_main_bundle();
    if (!b) return;
    CFURLRef url = CFBundleCopyBundleURL(b);
    game_bundle = [[NSBundle bundleWithURL:(__bridge NSURL *)url] retain];
    CFRelease(url);
    if (!game_bundle) return;
    Method m = class_getClassMethod([NSBundle class], @selector(mainBundle));
    method_setImplementation(m, (IMP)dx_main_bundle);
    LOGI("objc: main bundle is %s", game_bundle.bundlePath.UTF8String);
    NSString *lang = game_language(game_bundle);
    if (lang) {
        // like launching with -AppleLanguages (xx): the launcher compares its
        // localization with steam_language and otherwise asks for a restart forever
        [[NSUserDefaults standardUserDefaults] setVolatileDomain:@{@"AppleLanguages": @[lang]}
                                                         forName:NSArgumentDomain];
        NSDictionary *steam = @{@"en": @"english", @"de": @"german", @"fr": @"french", @"it": @"italian",
                                @"es": @"spanish", @"ru": @"russian", @"pl": @"polish", @"ja": @"japanese"};
        strlcpy(game_language_code, lang.UTF8String, sizeof game_language_code);
        NSString *name = steam[lang];
        if (name) strlcpy(game_language_name, name.UTF8String, sizeof game_language_name);
        LOGI("objc: game language %s", lang.UTF8String);
    }
}
