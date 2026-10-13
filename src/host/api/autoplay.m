// Autoplay (DXH_AUTOPLAY=<script>): a bot thread drives the game with synthetic
// keyboard/mouse input, takes screenshots and watches for hangs, so long test
// sessions run unattended. Input goes through the normal NSEvent -> Carbon
// translation (cb_bot_inject); real input is ignored meanwhile and the mouse is
// never grabbed or warped.
//
// Script: one command per line, '#' comments.
//   wait SECS                    sleep
//   key NAME [HOLD]              press and release (HOLD seconds, default 0.08)
//   down NAME / up NAME          hold / release a key
//   cursor FX FY                 virtual cursor at a fraction of the game window
//   click [left|right] [HOLD]    mouse click at the virtual cursor
//   look DX DY [SECS]            relative mouse motion spread over SECS (0.3)
//   shot NAME                    screenshot (presented frame, else front window)
//   wander SECS                  random play: walk, look, jump, crouch, use, shoot
//   waitframes N [TIMEOUT]       wait until N more frames were presented
//   repeat N ... end             loop (N = 0: forever)
//   log TEXT / quit
// Env: DXH_AUTOPLAY_OUT (screenshot dir, default .), DXH_AUTOPLAY_SEED,
// DXH_AUTOPLAY_HANG (seconds without a frame before exiting with 86, default 120,
// 0 = off), DXH_AUTOPLAY_SHOTS (seconds between automatic screenshots, 0 = off).
#import "carbon_internal.h"
#include "cpu.h"

#include <IOKit/hid/IOHIDUsageTables.h>

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

extern bool cb_bot;
extern NSEventModifierFlags cb_bot_flags;
extern NSUInteger cb_bot_buttons;
extern CGPoint cb_bot_mouse;
void cb_bot_inject(NSEvent *e, float dx, float dy);
extern _Atomic uint64_t gl_frames;                         // gl_cgl.c
uint8_t *gl_capture_frame(int *w, int *h, double timeout); // gl_cgl.c

static const char *out_dir = ".";
static int shot_no;
static pthread_mutex_t shot_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t rng;

static uint32_t rnd(void) {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng >> 33);
}
static double frand(double lo, double hi) { return lo + (hi - lo) * (rnd() / 2147483648.0); }
static void sleep_s(double s) {
    if (s > 0) usleep((useconds_t)(s * 1e6));
}

// --- keys ---------------------------------------------------------------------------------------------------

static const struct {
    const char *name;
    unsigned short code;
} keys[] = {
    {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C}, {"d", kVK_ANSI_D}, {"e", kVK_ANSI_E},
    {"f", kVK_ANSI_F}, {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H}, {"i", kVK_ANSI_I}, {"j", kVK_ANSI_J},
    {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L}, {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O},
    {"p", kVK_ANSI_P}, {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R}, {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T},
    {"u", kVK_ANSI_U}, {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X}, {"y", kVK_ANSI_Y},
    {"z", kVK_ANSI_Z}, {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2}, {"3", kVK_ANSI_3},
    {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5}, {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7}, {"8", kVK_ANSI_8},
    {"9", kVK_ANSI_9}, {"return", kVK_Return}, {"enter", kVK_ANSI_KeypadEnter}, {"esc", kVK_Escape},
    {"space", kVK_Space}, {"tab", kVK_Tab}, {"backspace", kVK_Delete}, {"up", kVK_UpArrow},
    {"down", kVK_DownArrow}, {"left", kVK_LeftArrow}, {"right", kVK_RightArrow}, {"shift", kVK_Shift},
    {"ctrl", kVK_Control}, {"alt", kVK_Option}, {"cmd", kVK_Command}, {"f1", kVK_F1}, {"f2", kVK_F2},
    {"f3", kVK_F3}, {"f4", kVK_F4}, {"f5", kVK_F5}, {"f6", kVK_F6}, {"f7", kVK_F7}, {"f8", kVK_F8},
    {"f9", kVK_F9}, {"f10", kVK_F10}, {"f11", kVK_F11}, {"f12", kVK_F12},
};

static int key_code(const char *name) {
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcasecmp(name, keys[i].name)) return keys[i].code;
    return -1;
}

static NSEventModifierFlags modifier_bit(unsigned short code) {
    switch (code) {
    case kVK_Shift: return NSEventModifierFlagShift | 0x2;
    case kVK_Control: return NSEventModifierFlagControl | 0x1;
    case kVK_Option: return NSEventModifierFlagOption | 0x20;
    case kVK_Command: return NSEventModifierFlagCommand | 0x8;
    default: return 0;
    }
}

// In-game input is read from IOHIDManager (iokit.c), menus from Carbon events:
// every key/button/motion goes to both.
bool hid_bot_value(uint32_t page, uint32_t usage, int32_t value);

static uint32_t hid_usage(unsigned short code) {
    static const struct {
        unsigned short vk;
        uint8_t usage;
    } map[] = {
        {kVK_ANSI_A, 0x04}, {kVK_ANSI_B, 0x05}, {kVK_ANSI_C, 0x06}, {kVK_ANSI_D, 0x07}, {kVK_ANSI_E, 0x08},
        {kVK_ANSI_F, 0x09}, {kVK_ANSI_G, 0x0a}, {kVK_ANSI_H, 0x0b}, {kVK_ANSI_I, 0x0c}, {kVK_ANSI_J, 0x0d},
        {kVK_ANSI_K, 0x0e}, {kVK_ANSI_L, 0x0f}, {kVK_ANSI_M, 0x10}, {kVK_ANSI_N, 0x11}, {kVK_ANSI_O, 0x12},
        {kVK_ANSI_P, 0x13}, {kVK_ANSI_Q, 0x14}, {kVK_ANSI_R, 0x15}, {kVK_ANSI_S, 0x16}, {kVK_ANSI_T, 0x17},
        {kVK_ANSI_U, 0x18}, {kVK_ANSI_V, 0x19}, {kVK_ANSI_W, 0x1a}, {kVK_ANSI_X, 0x1b}, {kVK_ANSI_Y, 0x1c},
        {kVK_ANSI_Z, 0x1d}, {kVK_ANSI_1, 0x1e}, {kVK_ANSI_2, 0x1f}, {kVK_ANSI_3, 0x20}, {kVK_ANSI_4, 0x21},
        {kVK_ANSI_5, 0x22}, {kVK_ANSI_6, 0x23}, {kVK_ANSI_7, 0x24}, {kVK_ANSI_8, 0x25}, {kVK_ANSI_9, 0x26},
        {kVK_ANSI_0, 0x27}, {kVK_Return, 0x28}, {kVK_Escape, 0x29}, {kVK_Delete, 0x2a}, {kVK_Tab, 0x2b},
        {kVK_Space, 0x2c}, {kVK_F1, 0x3a}, {kVK_F2, 0x3b}, {kVK_F3, 0x3c}, {kVK_F4, 0x3d}, {kVK_F5, 0x3e},
        {kVK_F6, 0x3f}, {kVK_F7, 0x40}, {kVK_F8, 0x41}, {kVK_F9, 0x42}, {kVK_F10, 0x43}, {kVK_F11, 0x44},
        {kVK_F12, 0x45}, {kVK_RightArrow, 0x4f}, {kVK_LeftArrow, 0x50}, {kVK_DownArrow, 0x51},
        {kVK_UpArrow, 0x52}, {kVK_ANSI_KeypadEnter, 0x58}, {kVK_Control, 0xe0}, {kVK_Shift, 0xe1},
        {kVK_Option, 0xe2}, {kVK_Command, 0xe3},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].vk == code) return map[i].usage;
    return 0;
}

static void key_event(unsigned short code, bool down) {
    uint32_t u = hid_usage(code);
    if (u) hid_bot_value(kHIDPage_KeyboardOrKeypad, u, down);
    cb_on_main(^{
      @autoreleasepool {
          CGEventRef ce = CGEventCreateKeyboardEvent(NULL, code, down);
          NSEventModifierFlags bit = modifier_bit(code);
          if (bit) {
              if (down) cb_bot_flags |= bit;
              else cb_bot_flags &= ~bit;
              CGEventSetType(ce, kCGEventFlagsChanged);
          }
          CGEventSetFlags(ce, (CGEventFlags)cb_bot_flags);
          NSEvent *e = [NSEvent eventWithCGEvent:ce];
          CFRelease(ce);
          if (e) cb_bot_inject(e, 0, 0);
      }
    });
}

// --- mouse --------------------------------------------------------------------------------------------------

// The game's window: the largest visible Carbon window (main thread).
static NSWindow *game_window(void) {
    NSWindow *best = nil;
    double area = 0;
    for (NSWindow *w in [NSApp windows]) {
        if (!w.isVisible || !cb_window_for_ns(w)) continue;
        double a = w.frame.size.width * w.frame.size.height;
        if (a > area) area = a, best = w;
    }
    return best;
}

static void mouse_event(NSEventType type, float dx, float dy, int clicks) {
    cb_on_main(^{
      @autoreleasepool {
          NSWindow *w = game_window();
          NSPoint g = cb_global_to_ns(cb_bot_mouse);
          NSPoint p = w ? [w convertPointFromScreen:g] : g;
          NSEvent *e = [NSEvent mouseEventWithType:type location:p modifierFlags:cb_bot_flags
                                         timestamp:[[NSProcessInfo processInfo] systemUptime]
                                      windowNumber:w ? w.windowNumber : 0 context:nil eventNumber:0
                                        clickCount:clicks pressure:clicks ? 1.0f : 0.0f];
          if (e) cb_bot_inject(e, dx, dy);
      }
    });
}

static void set_cursor(double fx, double fy) {
    cb_on_main(^{
      NSWindow *w = game_window();
      CBWindow *cw = w ? cb_window_for_ns(w) : nil;
      if (!cw) return;
      CGRect c = cb_window_content_global(cw);
      cb_bot_mouse = CGPointMake(c.origin.x + fx * c.size.width, c.origin.y + fy * c.size.height);
    });
    mouse_event(NSEventTypeMouseMoved, 0, 0, 0);
}

static void button(bool right, bool down) {
    NSUInteger bit = right ? 2 : 1;
    hid_bot_value(kHIDPage_Button, right ? 2 : 1, down);
    if (down) cb_bot_buttons |= bit;
    else cb_bot_buttons &= ~bit;
    NSEventType t = right ? (down ? NSEventTypeRightMouseDown : NSEventTypeRightMouseUp)
                          : (down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp);
    mouse_event(t, 0, 0, 1);
}

static void look(double dx, double dy, double secs) {
    int steps = secs > 0 ? (int)(secs / 0.016) : 1;
    if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) {
        NSEventType t = (cb_bot_buttons & 1)   ? NSEventTypeLeftMouseDragged
                        : (cb_bot_buttons & 2) ? NSEventTypeRightMouseDragged
                                               : NSEventTypeMouseMoved;
        // integer HID counts; carry the remainder so the total is exact
        static double rx, ry;
        rx += dx / steps, ry += dy / steps;
        int ix = (int)rx, iy = (int)ry;
        rx -= ix, ry -= iy;
        if (ix) hid_bot_value(kHIDPage_GenericDesktop, kHIDUsage_GD_X, ix);
        if (iy) hid_bot_value(kHIDPage_GenericDesktop, kHIDUsage_GD_Y, iy);
        // the game warps the cursor back to the centre and reads the offset
        cb_bot_mouse.x += dx / steps, cb_bot_mouse.y += dy / steps;
        mouse_event(t, (float)(dx / steps), (float)(dy / steps), 0);
        sleep_s(secs / steps);
    }
}

// --- screenshots ----------------------------------------------------------------------------------------------

// Write RGBA pixels (rows bottom-up when flip) scaled to at most 1600 wide.
static bool write_png(const char *path, const uint8_t *px, int w, int h, size_t stride, bool flip) {
    int ow = w > 1600 ? 1600 : w, oh = (int)((double)h * ow / w);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, stride * h, NULL);
    CGImageRef src = CGImageCreate(w, h, 8, 32, stride, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast, dp, NULL,
                                   false, kCGRenderingIntentDefault);
    CGContextRef ctx = CGBitmapContextCreate(NULL, ow, oh, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    if (flip) {
        CGContextTranslateCTM(ctx, 0, oh);
        CGContextScaleCTM(ctx, 1, -1);
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, ow, oh), src);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    bool ok = [png writeToFile:[NSString stringWithUTF8String:path] atomically:NO];
    [rep release];
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGImageRelease(src);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
}

static void shot(const char *name) {
    pthread_mutex_lock(&shot_lock);
    char path[1024];
    snprintf(path, sizeof path, "%s/%03d-%s.png", out_dir, shot_no++, name);
    pthread_mutex_unlock(&shot_lock);
    int w = 0, h = 0;
    uint8_t *px = gl_capture_frame(&w, &h, 2.0);
    if (px) {
        bool ok = write_png(path, px, w, h, (size_t)w * 4, true);
        free(px);
        LOGI("bot: shot %s (%dx%d frame)%s", path, w, h, ok ? "" : " FAILED");
        return;
    }
    // no GL frames (launcher, dialogs): draw the front window's views
    __block NSBitmapImageRep *rep = nil;
    cb_on_main(^{
      NSWindow *w = NSApp.keyWindow ?: game_window();
      NSView *v = w.contentView.superview ?: w.contentView;
      if (!v) return;
      rep = [[v bitmapImageRepForCachingDisplayInRect:v.bounds] retain];
      [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    });
    if (!rep) {
        LOGI("bot: shot %s: nothing to capture", path);
        return;
    }
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    bool ok = [png writeToFile:[NSString stringWithUTF8String:path] atomically:NO];
    LOGI("bot: shot %s (window %ldx%ld)%s", path, (long)rep.pixelsWide, (long)rep.pixelsHigh, ok ? "" : " FAILED");
    [rep release];
}

// --- random play ----------------------------------------------------------------------------------------------

static void release_all(void) {
    static const char *held[] = {"w", "a", "s", "d", "shift", "ctrl", "space", "e"};
    for (size_t i = 0; i < sizeof held / sizeof held[0]; i++) key_event((unsigned short)key_code(held[i]), false);
    if (cb_bot_buttons & 1) button(false, false);
    if (cb_bot_buttons & 2) button(true, false);
}

// Presented frames per second over the last `secs`.
static double frame_rate(double secs) {
    uint64_t f0 = atomic_load(&gl_frames);
    double t0 = cb_now();
    sleep_s(secs);
    return (atomic_load(&gl_frames) - f0) / (cb_now() - t0);
}

// Menus (game over, pause, inventory) render far faster than the 3D world. If
// the game sits in one, pick its first entry ("load last save" on the game
// over screen) with the cursor off the items. True if it did something.
static bool recover_from_menu(void) {
    static int streak; // still in a menu after the last attempt: back out first
    double fps = frame_rate(1.0);
    if (fps < 100 || frame_rate(2.0) < 100) {
        streak = 0;
        return false;
    }
    LOGI("bot: menu detected (%.0f frames/s): %sfirst entry", fps, streak ? "Esc, " : "");
    release_all();
    set_cursor(0.02, 0.98);
    if (streak++) key_event(kVK_Escape, true), sleep_s(0.1), key_event(kVK_Escape, false), sleep_s(1.5);
    for (int i = 0; i < 4; i++) key_event(kVK_UpArrow, true), sleep_s(0.1), key_event(kVK_UpArrow, false), sleep_s(0.3);
    key_event(kVK_Return, true), sleep_s(0.1), key_event(kVK_Return, false);
    sleep_s(30); // loading
    return true;
}

// Walk around for secs: mostly forward with strafing and turning, some jumps,
// crouching, using things and shooting. Never touches menus, saves or Esc; if
// the game ends up in a menu anyway (death), it continues from the last save.
static void wander(double secs) {
    double end = cb_now() + secs, check = cb_now() + 10;
    while (cb_now() < end) {
        if (cb_now() > check) {
            recover_from_menu();
            check = cb_now() + 10;
        }
        uint32_t r = rnd() % 100;
        if (r < 40) { // walk with a turn
            const char *k = rnd() % 5 ? "w" : (rnd() % 2 ? "a" : (rnd() % 2 ? "d" : "s"));
            double t = frand(0.5, 3.0);
            LOGI("bot: walk %s %.1fs", k, t);
            key_event((unsigned short)key_code(k), true);
            look(frand(-600, 600), frand(-40, 40), t);
            key_event((unsigned short)key_code(k), false);
        } else if (r < 60) {
            double dx = frand(-1500, 1500), dy = frand(-150, 150);
            LOGI("bot: look %.0f %.0f", dx, dy);
            look(dx, dy, frand(0.2, 1.0));
        } else if (r < 70) {
            LOGI("bot: jump");
            key_event(kVK_Space, true), sleep_s(0.1), key_event(kVK_Space, false);
            sleep_s(0.5);
        } else if (r < 77) {
            LOGI("bot: crouch");
            key_event(kVK_Control, true), sleep_s(0.1), key_event(kVK_Control, false);
            sleep_s(frand(0.5, 2));
        } else if (r < 85) {
            LOGI("bot: use");
            key_event(kVK_ANSI_E, true), sleep_s(0.1), key_event(kVK_ANSI_E, false);
            sleep_s(1.0);
        } else if (r < 90) {
            LOGI("bot: shoot");
            button(true, true), sleep_s(0.3);
            button(false, true), sleep_s(frand(0.1, 1.0)), button(false, false);
            button(true, false);
        } else if (r < 95) { // dialog / menu choices if something opened
            int n = 1 + (int)(rnd() % 3);
            LOGI("bot: choose %d", n);
            unsigned short c = (unsigned short)key_code((char[]){(char)('0' + n), 0});
            key_event(c, true), sleep_s(0.1), key_event(c, false);
            sleep_s(0.5);
        } else {
            double t = frand(0.5, 3);
            LOGI("bot: idle %.1fs", t);
            sleep_s(t);
        }
    }
    release_all();
}

// --- watchdogs ---------------------------------------------------------------------------------------------

static void *hang_thread(void *arg) {
    int limit = (int)(intptr_t)arg;
    uint64_t last = atomic_load(&gl_frames);
    double since = cb_now();
    for (;;) {
        sleep_s(1);
        uint64_t f = atomic_load(&gl_frames);
        if (f != last || !f) { // nothing to watch before the first frame (launcher)
            last = f, since = cb_now();
            continue;
        }
        if (cb_now() - since > limit) {
            LOGE("bot: HANG: no frame for %d s (frame %llu)", limit, (unsigned long long)f);
            cpu_dump_guest_threads();
            char cmd[1200];
            snprintf(cmd, sizeof cmd, "/usr/bin/sample %d 3 -file '%s/hang-sample.txt' >/dev/null 2>&1", getpid(),
                     out_dir);
            if (system(cmd) == 0) LOGE("bot: host stacks in %s/hang-sample.txt", out_dir);
            _exit(86);
        }
    }
    return NULL;
}

static void *shots_thread(void *arg) {
    int every = (int)(intptr_t)arg;
    for (;;) {
        sleep_s(every);
        if (atomic_load(&gl_frames)) shot("auto");
    }
    return NULL;
}

// --- script ---------------------------------------------------------------------------------------------------

static void run(char **lines, int n) {
    struct {
        int pc, left;
    } stack[16];
    int depth = 0;
    for (int pc = 0; pc < n; pc++) {
        char cmd[32] = "", a[256] = "", b[64] = "", c[64] = "";
        int got = sscanf(lines[pc], " %31s %255s %63s %63s", cmd, a, b, c);
        if (got < 1 || cmd[0] == '#') continue;
        if (!strcmp(cmd, "log")) {
            LOGI("bot: %s", lines[pc] + strspn(lines[pc], " \t") + 4);
            continue;
        }
        LOGI("bot: %s", lines[pc] + strspn(lines[pc], " \t"));
        // the game only takes input with the cursor over its window; a corner
        // keeps it off menu items (in game the game centres it itself)
        if (!cb_bot_mouse.x && !cb_bot_mouse.y && atomic_load(&gl_frames)) set_cursor(0.02, 0.98);
        if (!strcmp(cmd, "wait")) sleep_s(atof(a));
        else if (!strcmp(cmd, "key") || !strcmp(cmd, "down") || !strcmp(cmd, "up")) {
            int k = key_code(a);
            if (k < 0) {
                LOGE("bot: unknown key '%s'", a);
                continue;
            }
            if (cmd[0] != 'u') key_event((unsigned short)k, true);
            if (cmd[0] == 'k') sleep_s(got > 2 ? atof(b) : 0.08);
            if (cmd[0] != 'd') key_event((unsigned short)k, false);
        } else if (!strcmp(cmd, "cursor")) set_cursor(atof(a), atof(b));
        else if (!strcmp(cmd, "click")) {
            bool right = !strcmp(a, "right");
            button(right, true);
            sleep_s(got > 2 ? atof(b) : 0.08);
            button(right, false);
        } else if (!strcmp(cmd, "look")) look(atof(a), atof(b), got > 3 ? atof(c) : 0.3);
        else if (!strcmp(cmd, "shot")) shot(got > 1 ? a : "shot");
        else if (!strcmp(cmd, "wander")) wander(atof(a));
        else if (!strcmp(cmd, "waitframes")) {
            uint64_t want = atomic_load(&gl_frames) + (uint64_t)atoll(a);
            double end = cb_now() + (got > 2 ? atof(b) : 600);
            while (atomic_load(&gl_frames) < want && cb_now() < end) sleep_s(0.1);
            if (atomic_load(&gl_frames) < want) LOGE("bot: waitframes timed out");
        } else if (!strcmp(cmd, "repeat")) {
            if (depth == 16) {
                LOGE("bot: repeat nested too deep");
                continue;
            }
            stack[depth].pc = pc, stack[depth].left = atoi(a), depth++;
        } else if (!strcmp(cmd, "end")) {
            if (!depth) continue;
            if (stack[depth - 1].left == 0 || --stack[depth - 1].left > 0) pc = stack[depth - 1].pc;
            else depth--;
        } else if (!strcmp(cmd, "quit")) {
            LOGI("bot: quit");
            _exit(0);
        } else LOGE("bot: unknown command '%s'", cmd);
    }
    LOGI("bot: script finished");
}

static void *bot_thread(void *arg) {
    const char *path = arg;
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGE("bot: cannot read script %s", path);
        return NULL;
    }
    char **lines = NULL, buf[512];
    int n = 0;
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        lines = realloc(lines, sizeof *lines * (n + 1));
        lines[n++] = strdup(buf);
    }
    fclose(f);
    @autoreleasepool {
        run(lines, n);
    }
    return NULL;
}

void autoplay_start(void) {
    const char *script = getenv("DXH_AUTOPLAY");
    if (!script || !*script) return;
    cb_bot = true;
    setenv("DXH_NO_MOUSE_GRAB", "1", 1); // the bot must not capture the real cursor
    if (getenv("DXH_AUTOPLAY_OUT")) out_dir = getenv("DXH_AUTOPLAY_OUT");
    rng = getenv("DXH_AUTOPLAY_SEED") ? strtoull(getenv("DXH_AUTOPLAY_SEED"), NULL, 0) : (uint64_t)time(NULL);
    LOGI("bot: autoplay %s, seed %llu, screenshots in %s", script, (unsigned long long)rng, out_dir);
    int hang = getenv("DXH_AUTOPLAY_HANG") ? atoi(getenv("DXH_AUTOPLAY_HANG")) : 120;
    int every = getenv("DXH_AUTOPLAY_SHOTS") ? atoi(getenv("DXH_AUTOPLAY_SHOTS")) : 0;
    pthread_t t;
    pthread_create(&t, NULL, bot_thread, strdup(script));
    pthread_detach(t);
    if (hang > 0) pthread_create(&t, NULL, hang_thread, (void *)(intptr_t)hang), pthread_detach(t);
    if (every > 0) pthread_create(&t, NULL, shots_thread, (void *)(intptr_t)every), pthread_detach(t);
}
