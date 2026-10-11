// Component Manager (FMOD uses it to find and open audio output units).
//
// On 64-bit macOS audio units are AudioComponents and AudioUnit ==
// AudioComponentInstance, so the Component Manager calls are mapped onto the
// AudioComponent API:
//   Component          -> hobj handle of a host AudioComponent
//   ComponentInstance  -> hobj handle of a host AudioComponentInstance (= AudioUnit)
// The audio thunks (AudioUnit*, AudioOutputUnit*) can hobj_unwrap() the guest
// ComponentInstance/AudioUnit and use it directly.
// ComponentDescription and AudioComponentDescription are both five UInt32s.
#include "common.h"

#include <AudioToolbox/AudioToolbox.h>

__asm__(".linker_option \"-framework\", \"AudioToolbox\"");

_Static_assert(sizeof(AudioComponentDescription) == 20, "AudioComponentDescription");

static void read_desc(gaddr_t g, AudioComponentDescription *d) {
    memset(d, 0, sizeof *d);
    if (g) memcpy(d, g2h(g), 20);
}

THUNK(FindNextComponent) {
    AudioComponentDescription d;
    read_desc(ARG(1), &d);
    AudioComponent prev = hobj_unwrap(ARG(0));
    AudioComponent c = AudioComponentFindNext(prev, &d);
    LOGD("FindNextComponent('%.4s','%.4s','%.4s') -> %p", (char *)&d.componentType, (char *)&d.componentSubType,
         (char *)&d.componentManufacturer, (void *)c);
    RET(hobj_wrap(c));
}

THUNK(CountComponents) {
    AudioComponentDescription d;
    read_desc(ARG(0), &d);
    RET(AudioComponentCount(&d));
}

// OpenAComponent(Component, ComponentInstance *ci)
THUNK(OpenAComponent) {
    AudioComponent c = hobj_unwrap(ARG(0));
    AudioComponentInstance inst = NULL;
    OSStatus st = c ? AudioComponentInstanceNew(c, &inst) : -2003 /* badComponentInstance */;
    if (ARG(1)) gwrite32(ARG(1), st == noErr ? hobj_wrap(inst) : 0);
    RET((uint32_t)(int32_t)(OSErr)st);
}

THUNK(OpenComponent) {
    AudioComponent c = hobj_unwrap(ARG(0));
    AudioComponentInstance inst = NULL;
    if (c && AudioComponentInstanceNew(c, &inst) != noErr) inst = NULL;
    RET(hobj_wrap(inst));
}

THUNK(CloseComponent) {
    AudioComponentInstance inst = hobj_unwrap(ARG(0));
    if (!inst) {
        RET((uint32_t)(int32_t)-2002); // badComponentInstance
        return;
    }
    OSStatus st = AudioComponentInstanceDispose(inst);
    hobj_forget(ARG(0));
    RET((uint32_t)(int32_t)(OSErr)st);
}

// GetComponentInfo(Component, ComponentDescription *cd, Handle name, Handle info, Handle icon)
// Only the description is filled in; the guest Handles are left untouched.
THUNK(GetComponentInfo) {
    AudioComponent c = hobj_unwrap(ARG(0));
    AudioComponentDescription d;
    if (!c || AudioComponentGetDescription(c, &d) != noErr) {
        RET((uint32_t)(int32_t)-2003);
        return;
    }
    if (ARG(1)) memcpy(g2h(ARG(1)), &d, 20);
    RET(0);
}
