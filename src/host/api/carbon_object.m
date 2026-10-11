// Carbon HIObject emulation: the CBObject base class, registered HIObject
// subclasses with guest construct/initialize/destruct handlers.
#import "carbon_internal.h"

@implementation CBObject
- (instancetype)initWithKind:(int)k {
    if ((self = [super init])) target = [[CBTarget alloc] initWithKind:k owner:self];
    return self;
}
- (void)dealloc {
    if (instance || (target && target->handlers.count)) {
        // Destruct event for custom subclasses.
        destructing = true;
        CBEvent *e = cb_event_new(kEventClassHIObject, kEventHIObjectDestruct, cb_now());
        cb_send_noprop(e, target);
        [e release];
    }
    target->owner = nil;
    [target release];
    [classID release];
    [super dealloc];
}
@end

// --- registered classes ----------------------------------------------------------------------------

@interface CBClass : CBBase {
  @public
    NSString *classID, *baseID;
    gaddr_t upp, udata;
    uint32_t ntypes;
    uint32_t *types;
}
@end
@implementation CBClass
@end

static NSMutableDictionary *classes; // classID -> CBClass

static NSString *const kViewClass = @"com.apple.hiview";
static NSString *const kObjectClass = @"com.apple.hiobject";

// HIObjectRegisterSubclass(classID, baseClassID, options, constructProc, numEvents, list, constructData, outClassRef)
THUNK(HIObjectRegisterSubclass) {
    NSString *cid = (NSString *)ARGSTR(0), *bid = (NSString *)ARGSTR(1);
    if (!cid) RET(paramErr);
    if (!classes) classes = [[NSMutableDictionary alloc] init];
    if ([classes objectForKey:cid]) RET(hiObjectClassExistsErr);
    CBClass *c = [[CBClass alloc] init];
    c->classID = [cid copy];
    c->baseID = [(bid ? bid : kObjectClass) copy];
    c->upp = ARG(3);
    c->ntypes = ARG(4);
    c->types = malloc(c->ntypes * 8 + 8);
    if (c->ntypes) memcpy(c->types, g2h(ARG(5)), c->ntypes * 8);
    c->udata = ARG(6);
    [classes setObject:c forKey:cid];
    [c release];
    LOGI("carbon: HIObjectRegisterSubclass(%s, base %s, %u events)", cid.UTF8String, c->baseID.UTF8String, c->ntypes);
    if (ARG(7)) gwrite32(ARG(7), cb_handle(c));
    RET(noErr);
}

// Copy the parameters of the guest's construct-data event into e.
static void copy_params(CBEvent *dst, gaddr_t src_handle) {
    CBEvent *src = CB_UNWRAP(CBEvent, src_handle);
    if (!src) return;
    @synchronized(src->params) {
        for (NSNumber *k in src->params) {
            CBParam *p = src->params[k];
            cb_event_set(dst, k.unsignedIntValue, p->type, p->data.bytes, (uint32_t)p->data.length);
        }
    }
}

// Run the construct + initialize protocol for a new object of a registered class.
OSStatus cb_hiobject_construct(CBObject *o, NSString *cid, gaddr_t initEvent) {
    // class chain: leaf first
    NSMutableArray *chain = [NSMutableArray array];
    for (CBClass *c = classes[cid]; c; c = classes[c->baseID]) [chain insertObject:c atIndex:0];
    o->classID = [cid copy];
    for (CBClass *c in chain) {
        // construct: the class handler is called with the registered construct data
        CBTarget *tmp = [[CBTarget alloc] initWithKind:TK_OBJECT owner:o];
        CBHandler *h = cb_install_handler(tmp, c->upp, c->ntypes, c->types, c->udata);
        uint32_t ctor[2] = {kEventClassHIObject, kEventHIObjectConstruct};
        bool has_ctor = false;
        for (uint32_t i = 0; i < c->ntypes; i++)
            if (c->types[2 * i] == ctor[0] && c->types[2 * i + 1] == ctor[1]) has_ctor = true;
        gaddr_t inst = 0;
        if (has_ctor) {
            CBEvent *e = cb_event_new(kEventClassHIObject, kEventHIObjectConstruct, cb_now());
            uint32_t oh = cb_handle(o);
            cb_event_set(e, kEventParamHIObjectInstance, typeHIObjectRef, &oh, 4);
            OSStatus st = cb_send(e, tmp);
            CBParam *p = cb_event_param(e, kEventParamHIObjectInstance);
            if (p && p->type == typeVoidPtr && p->data.length >= 4) memcpy(&inst, p->data.bytes, 4);
            [e release];
            if (st != noErr && st != eventNotHandledErr) {
                LOGE("carbon: HIObject construct of %s failed (%d)", cid.UTF8String, (int)st);
                [tmp release];
                return st;
            }
        }
        // move the handler to the object with the instance data as user data
        [h retain];
        [tmp->handlers removeObjectIdenticalTo:h];
        h->target = o->target;
        h->udata = inst;
        [o->target->handlers insertObject:h atIndex:0];
        [h release];
        [tmp release];
        o->instance = inst;
    }
    // initialize
    CBEvent *ie = cb_event_new(kEventClassHIObject, kEventHIObjectInitialize, cb_now());
    copy_params(ie, initEvent);
    OSStatus st = cb_send_noprop(ie, o->target);
    [ie release];
    if (st == eventNotHandledErr) st = noErr;
    return st;
}

CBView *cb_view_new_of_class(NSString *base); // carbon_view.m

// HIObjectCreate(classID, constructData event, HIObjectRef *out)
THUNK(HIObjectCreate) {
    NSString *cid = (NSString *)ARGSTR(0);
    if (ARG(2)) gwrite32(ARG(2), 0);
    if (!cid) RET(paramErr);
    CBClass *c = classes[cid];
    NSString *base = cid;
    while (classes[base]) base = ((CBClass *)classes[base])->baseID;
    CBObject *o;
    if ([base isEqualToString:kViewClass] || [base hasPrefix:@"com.apple.HI"]) {
        o = cb_view_new_of_class(base);
    } else if ([base isEqualToString:kObjectClass]) {
        o = [[CBObject alloc] initWithKind:TK_OBJECT];
    } else {
        LOGI("carbon: HIObjectCreate(%s): unknown class", cid.UTF8String);
        RET(-22084); // class not registered
    }
    OSStatus st = noErr;
    if (c) st = cb_hiobject_construct(o, cid, ARG(1));
    else o->classID = [cid copy];
    LOGD("carbon: HIObjectCreate(%s) = %d", cid.UTF8String, (int)st);
    if (st != noErr) {
        [o release];
        RET(st);
    }
    if (ARG(2)) gwrite32(ARG(2), cb_handle(o));
    RET(noErr);
}

THUNK(HIObjectCopyClassID) {
    CBObject *o = CB_UNWRAP(CBObject, ARG(0));
    if (!o) RET(0);
    NSString *cid = o->classID;
    if (!cid) {
        if ([o isKindOfClass:[CBWindow class]]) cid = @"com.apple.window";
        else if ([o isKindOfClass:[CBMenu class]]) cid = @"com.apple.HIMenu";
        else if ([o isKindOfClass:[CBView class]]) cid = kViewClass;
        else cid = kObjectClass;
    }
    RET(cfg((CFTypeRef)[cid copy]));
}

THUNK(HIObjectGetEventTarget) {
    CBObject *o = CB_UNWRAP(CBObject, ARG(0));
    RET(o ? cb_handle(o->target) : 0);
}
