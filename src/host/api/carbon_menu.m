// Carbon Menu Manager emulation on NSMenu. Item indices are 1-based; each
// NSMenuItem carries a CBItemInfo (command ID, properties, submenu) as its
// representedObject. Choosing an item sends kEventCommandProcess.
#import "carbon_internal.h"

@interface CBItemInfo : NSObject {
  @public
    uint32_t command;
    CBMenu *sub; // retained
    NSMutableDictionary *props;
}
@end
@implementation CBItemInfo
- (void)dealloc {
    [sub release];
    [props release];
    [super dealloc];
}
@end

@interface CBMenuTarget : NSObject
@end

static CBMenuTarget *menu_target;
static NSMapTable *menu_map; // NSMenu -> CBMenu (weak both)

@implementation CBMenu
- (void)dealloc {
    [menu_map removeObjectForKey:ns];
    [ns release];
    [super dealloc];
}
@end

CBMenu *cb_menu_for_ns(NSMenu *m) { return m ? [menu_map objectForKey:m] : nil; }

CBMenu *cb_menu_new(uint16_t mid, NSMenu *existing) {
    if (!menu_map) menu_map = [[NSMapTable weakToWeakObjectsMapTable] retain];
    CBMenu *m = [[CBMenu alloc] initWithKind:TK_MENU];
    m->menuID = mid;
    m->ns = existing ? [existing retain] : [[NSMenu alloc] initWithTitle:@""];
    [m->ns setAutoenablesItems:NO];
    [menu_map setObject:m forKey:m->ns];
    return m;
}

static CBItemInfo *info_for(NSMenuItem *it) {
    id o = [it representedObject];
    if ([o isKindOfClass:[CBItemInfo class]]) return o;
    CBItemInfo *i = [[[CBItemInfo alloc] init] autorelease];
    i->props = [[NSMutableDictionary alloc] init];
    [it setRepresentedObject:i];
    return i;
}

@implementation CBMenuTarget
- (void)menuAction:(NSMenuItem *)it {
    CBItemInfo *i = info_for(it);
    CBMenu *m = cb_menu_for_ns([it menu]);
    uint16_t idx = (uint16_t)([[it menu] indexOfItem:it] + 1);
    gaddr_t mh = m ? cb_handle(m) : 0;
    if (!i->command) return;
    OSStatus st = cb_send_command(i->command, kHICommandFromMenu, mh, idx, 0, nil);
    if (st != noErr && i->command == kHICommandQuit) [NSApp terminate:nil];
    if (st != noErr && i->command == kHICommandHide) [NSApp hide:nil];
}
@end

static NSMenuItem *item_at(CBMenu *m, uint32_t idx) {
    if (!m || idx < 1 || idx > (uint32_t)[m->ns numberOfItems]) return nil;
    return [m->ns itemAtIndex:(NSInteger)idx - 1];
}

static NSMenuItem *new_item(NSString *s, uint32_t attrs, uint32_t cmd) {
    NSMenuItem *it;
    if (attrs & kMenuItemAttrSeparator) {
        it = [[NSMenuItem separatorItem] retain];
    } else {
        it = [[NSMenuItem alloc] initWithTitle:s ? s : @"" action:@selector(menuAction:) keyEquivalent:@""];
        [it setTarget:menu_target];
        [it setEnabled:!(attrs & kMenuItemAttrDisabled)];
        [it setHidden:(attrs & kMenuItemAttrHidden) != 0];
        if (attrs & kMenuItemAttrSectionHeader) [it setEnabled:NO];
    }
    info_for(it)->command = cmd;
    return [it autorelease];
}

void cb_menu_module_init(void) {
    menu_target = [[CBMenuTarget alloc] init];
    if ([NSApp mainMenu]) return;
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@""];
    NSString *name = [[NSProcessInfo processInfo] processName];
    CFTypeRef bn = CFBundleGetValueForInfoDictionaryKey(cf_main_bundle(), kCFBundleNameKey);
    if (bn && CFGetTypeID(bn) == CFStringGetTypeID()) name = (NSString *)bn;
    NSMenuItem *hide = new_item([@"Hide " stringByAppendingString:name], 0, kHICommandHide);
    [hide setKeyEquivalent:@"h"];
    [appMenu addItem:hide];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = new_item([@"Quit " stringByAppendingString:name], 0, kHICommandQuit);
    [quit setKeyEquivalent:@"q"];
    [appMenu addItem:quit];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];
    [NSApp setMainMenu:bar];
    [appMenu release];
    [appItem release];
    [bar release];
}

#define MENU(i) CB_UNWRAP(CBMenu, ARG(i))

static NSString *nsstr(gaddr_t g) {
    NSString *s = g ? (NSString *)gcf_string(g) : nil;
    return s ? s : @"";
}

// CreateNewMenu(MenuID, MenuAttributes, MenuRef *out)
THUNK(CreateNewMenu) {
    __block CBMenu *m;
    cb_on_main(^{
      m = cb_menu_new((uint16_t)ARG(0), nil);
    });
    m->mattrs = ARG(1);
    if (ARG(2)) gwrite32(ARG(2), cb_handle(m));
    RET(noErr);
}

THUNK(AcquireRootMenu) {
    __block CBMenu *m;
    cb_on_main(^{
      cb_app_init();
      NSMenu *bar = [NSApp mainMenu];
      m = cb_menu_for_ns(bar);
      if (m) [m retain];
      else m = cb_menu_new(0, bar);
    });
    RET(cb_handle(m));
}

THUNK(GetNewMBar) {
    // No resources: hand out a dummy handle; SetMenuBar ignores it.
    gaddr_t h = gheap_calloc(1, 8);
    gwrite32(h, h + 4);
    RET(h);
}
THUNK(SetMenuBar) {}

THUNK(DeleteMenu) {
    uint16_t mid = (uint16_t)ARG(0);
    cb_on_main(^{
      NSMenu *bar = [NSApp mainMenu];
      for (NSMenuItem *it in [[[bar itemArray] copy] autorelease]) {
          CBMenu *m = cb_menu_for_ns([it submenu]);
          if (m && m->menuID == mid) [bar removeItem:it];
      }
    });
}

// AppendMenuItemTextWithCFString(menu, string, attrs, commandID, MenuItemIndex *out)
THUNK(AppendMenuItemTextWithCFString) {
    CBMenu *m = MENU(0);
    if (!m) RET(paramErr);
    NSString *s = nsstr(ARG(1));
    __block NSInteger idx;
    cb_on_main(^{
      [m->ns addItem:new_item(s, ARG(2), ARG(3))];
      idx = [m->ns numberOfItems];
    });
    if (ARG(4)) gwrite16(ARG(4), (uint16_t)idx);
    RET(noErr);
}
// InsertMenuItemTextWithCFString(menu, string, afterItem, attrs, commandID)
THUNK(InsertMenuItemTextWithCFString) {
    CBMenu *m = MENU(0);
    if (!m) RET(paramErr);
    NSString *s = nsstr(ARG(1));
    cb_on_main(^{
      NSInteger after = (uint16_t)ARG(2);
      if (after > [m->ns numberOfItems]) after = [m->ns numberOfItems];
      [m->ns insertItem:new_item(s, ARG(3), ARG(4)) atIndex:after];
    });
    RET(noErr);
}
THUNK(DeleteMenuItem) {
    CBMenu *m = MENU(0);
    uint32_t i = (uint16_t)ARG(1);
    cb_on_main(^{
      NSMenuItem *it = item_at(m, i);
      if (it) [m->ns removeItem:it];
    });
}
THUNK(DeleteMenuItems) {
    CBMenu *m = MENU(0);
    if (!m) RET(paramErr);
    uint32_t first = (uint16_t)ARG(1), n = ARG(2);
    cb_on_main(^{
      for (uint32_t k = 0; k < n; k++) {
          NSMenuItem *it = item_at(m, first);
          if (!it) break;
          [m->ns removeItem:it];
      }
    });
    RET(noErr);
}
THUNK(CopyMenuItemTextAsCFString) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    if (ARG(2)) gwrite32(ARG(2), cfg((CFTypeRef)[[it title] copy]));
    RET(noErr);
}
THUNK(SetMenuItemTextWithCFString) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    NSString *s = nsstr(ARG(2));
    cb_on_main(^{
      [it setTitle:s];
    });
    RET(noErr);
}
THUNK(SetMenuTitleWithCFString) {
    CBMenu *m = MENU(0);
    if (!m) RET(paramErr);
    NSString *s = nsstr(ARG(1));
    cb_on_main(^{
      [m->ns setTitle:s];
    });
    RET(noErr);
}

static void set_item_enabled(CBMenu *m, uint32_t idx, bool en) {
    cb_on_main(^{
      if (idx == 0) {
          for (NSMenuItem *it in [m->ns itemArray]) [it setEnabled:en];
          return;
      }
      [item_at(m, idx) setEnabled:en];
    });
}
THUNK(EnableMenuItem) {
    CBMenu *m = MENU(0);
    if (m) set_item_enabled(m, (uint16_t)ARG(1), true);
}
THUNK(DisableMenuItem) {
    CBMenu *m = MENU(0);
    if (m) set_item_enabled(m, (uint16_t)ARG(1), false);
}

// Find the nth (1-based) item with a command ID in a menu tree.
static bool find_command(NSMenu *menu, uint32_t cmd, uint32_t *nth, NSMenu **outMenu, NSInteger *outIdx) {
    for (NSInteger i = 0; i < [menu numberOfItems]; i++) {
        NSMenuItem *it = [menu itemAtIndex:i];
        id o = [it representedObject];
        if ([o isKindOfClass:[CBItemInfo class]] && ((CBItemInfo *)o)->command == cmd && --*nth == 0) {
            *outMenu = menu;
            *outIdx = i;
            return true;
        }
        if ([it submenu] && find_command([it submenu], cmd, nth, outMenu, outIdx)) return true;
    }
    return false;
}

static NSMenuItem *command_item(gaddr_t menu, uint32_t cmd, NSMenu **outMenu, NSInteger *outIdx) {
    CBMenu *m = CB_UNWRAP(CBMenu, menu);
    NSMenu *root = m ? m->ns : [NSApp mainMenu];
    uint32_t nth = 1;
    NSMenu *om = nil;
    NSInteger oi = 0;
    if (!root || !find_command(root, cmd, &nth, &om, &oi)) return nil;
    if (outMenu) *outMenu = om;
    if (outIdx) *outIdx = oi;
    return [om itemAtIndex:oi];
}

THUNK(EnableMenuCommand) {
    gaddr_t mh = ARG(0);
    uint32_t c = ARG(1);
    cb_on_main(^{
      [command_item(mh, c, NULL, NULL) setEnabled:YES];
    });
}
THUNK(DisableMenuCommand) {
    gaddr_t mh = ARG(0);
    uint32_t c = ARG(1);
    cb_on_main(^{
      [command_item(mh, c, NULL, NULL) setEnabled:NO];
    });
}
THUNK(IsMenuCommandEnabled) {
    gaddr_t mh = ARG(0);
    uint32_t c = ARG(1);
    __block bool en = false;
    cb_on_main(^{
      NSMenuItem *it = command_item(mh, c, NULL, NULL);
      en = it && [it isEnabled];
    });
    RET(en);
}
THUNK(SetMenuCommandMark) {
    gaddr_t mh = ARG(0);
    uint32_t c = ARG(1);
    uint16_t mark = (uint16_t)ARG(2);
    __block bool found = false;
    cb_on_main(^{
      NSMenuItem *it = command_item(mh, c, NULL, NULL);
      found = it != nil;
      [it setState:mark ? NSControlStateValueOn : NSControlStateValueOff];
    });
    RET(found ? noErr : menuItemNotFoundErr);
}
THUNK(GetMenuItemCommandID) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    if (ARG(2)) gwrite32(ARG(2), info_for(it)->command);
    RET(noErr);
}
// GetIndMenuItemWithCommandID(menu, commandID, UInt32 index, MenuRef *outMenu, MenuItemIndex *outIndex)
THUNK(GetIndMenuItemWithCommandID) {
    CBMenu *m = MENU(0);
    NSMenu *root = m ? m->ns : [NSApp mainMenu];
    uint32_t nth = ARG(2) ? ARG(2) : 1;
    NSMenu *om = nil;
    NSInteger oi = 0;
    if (!root || !find_command(root, ARG(1), &nth, &om, &oi)) RET(menuItemNotFoundErr);
    CBMenu *cm = cb_menu_for_ns(om);
    if (!cm) {
        cm = cb_menu_new(0, om); // leaked on purpose: lives as long as the NSMenu mapping
    }
    if (ARG(3)) gwrite32(ARG(3), cb_handle(cm));
    if (ARG(4)) gwrite16(ARG(4), (uint16_t)(oi + 1));
    RET(noErr);
}
THUNK(GetMenuItemHierarchicalMenu) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    if (ARG(2)) gwrite32(ARG(2), cb_handle(cb_menu_for_ns([it submenu])));
    RET(noErr);
}
THUNK(SetMenuItemHierarchicalMenu) {
    CBMenu *m = MENU(0), *sub = MENU(2);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    CBItemInfo *i = info_for(it);
    [sub retain];
    [i->sub release];
    i->sub = sub;
    cb_on_main(^{
      if (sub && [sub->ns supermenu]) {
          NSMenu *sm = [sub->ns supermenu];
          NSInteger k = [sm indexOfItemWithSubmenu:sub->ns];
          if (k >= 0) [[sm itemAtIndex:k] setSubmenu:nil];
      }
      if (sub && ![[sub->ns title] length]) [sub->ns setTitle:[it title]];
      [it setSubmenu:sub ? sub->ns : nil];
    });
    RET(noErr);
}
THUNK(GetMenuItemProperty) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    NSData *d = [info_for(it)->props objectForKey:@(((uint64_t)ARG(2) << 32) | ARG(3))];
    if (!d) RET(menuPropertyNotFoundErr);
    uint32_t n = (uint32_t)d.length;
    if (ARG(5)) gwrite32(ARG(5), n);
    if (ARG(6)) memcpy(g2h(ARG(6)), d.bytes, n < ARG(4) ? n : ARG(4));
    RET(noErr);
}
// SetMenuItemCommandKey(menu, item, Boolean virtualKey, UInt16 key)
THUNK(SetMenuItemCommandKey) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    bool virt = ARG(2) & 0xff;
    uint16_t key = (uint16_t)ARG(3);
    if (virt) RET(noErr); // virtual key equivalents are not mapped
    unichar c = key;
    NSString *s = [[NSString stringWithCharacters:&c length:1] lowercaseString];
    cb_on_main(^{
      [it setKeyEquivalent:s];
    });
    RET(noErr);
}
// SetMenuItemModifiers(menu, item, UInt8 modifiers)
THUNK(SetMenuItemModifiers) {
    CBMenu *m = MENU(0);
    NSMenuItem *it = item_at(m, (uint16_t)ARG(1));
    if (!it) RET(menuItemNotFoundErr);
    uint8_t mods = (uint8_t)ARG(2);
    NSEventModifierFlags f = 0;
    if (!(mods & kMenuNoCommandModifier)) f |= NSEventModifierFlagCommand;
    if (mods & kMenuShiftModifier) f |= NSEventModifierFlagShift;
    if (mods & kMenuOptionModifier) f |= NSEventModifierFlagOption;
    if (mods & kMenuControlModifier) f |= NSEventModifierFlagControl;
    cb_on_main(^{
      [it setKeyEquivalentModifierMask:f];
    });
    RET(noErr);
}

// HMGetHelpMenu(MenuRef *outHelpMenu, MenuItemIndex *outFirstCustomItemIndex)
THUNK(HMGetHelpMenu) {
    __block CBMenu *m;
    __block NSInteger first;
    cb_on_main(^{
      cb_app_init();
      NSMenu *help = [NSApp helpMenu];
      if (!help) {
          NSMenuItem *hi = [[[NSMenuItem alloc] initWithTitle:@"Help" action:NULL keyEquivalent:@""] autorelease];
          help = [[[NSMenu alloc] initWithTitle:@"Help"] autorelease];
          [hi setSubmenu:help];
          [[NSApp mainMenu] addItem:hi];
          [NSApp setHelpMenu:help];
      }
      m = cb_menu_for_ns(help);
      if (!m) m = cb_menu_new(0, help);
      first = [help numberOfItems] + 1;
    });
    if (ARG(0)) gwrite32(ARG(0), cb_handle(m));
    if (ARG(1)) gwrite16(ARG(1), (uint16_t)first);
    RET(noErr);
}

// Popup button contents mirror a CBMenu.
void cb_popup_set_menu(CBView *v, CBMenu *m) {
    if (v->menu != m) {
        [m retain];
        [v->menu release];
        v->menu = m;
    }
    NSPopUpButton *p = (NSPopUpButton *)v->ns;
    if (![p isKindOfClass:[NSPopUpButton class]] || !m) return;
    NSInteger sel = [p indexOfSelectedItem];
    [p removeAllItems];
    for (NSMenuItem *it in [m->ns itemArray]) {
        if ([it isSeparatorItem]) {
            [[p menu] addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem *n = [[NSMenuItem alloc] initWithTitle:[it title] action:NULL keyEquivalent:@""];
        [n setEnabled:[it isEnabled]];
        [[p menu] addItem:n];
        [n release];
    }
    [[p menu] setAutoenablesItems:NO];
    if (sel >= 0 && sel < [p numberOfItems]) [p selectItemAtIndex:sel];
    v->maximum = (int32_t)[p numberOfItems];
}
