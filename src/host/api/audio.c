// Audio: OpenAL, CoreAudio HAL (incl. the pre-10.6 AudioDevice/AudioHardware
// API FMOD uses), AudioUnit with guest render callbacks, AudioQueue, AUGraph,
// AudioFile/ExtAudioFile, and stubs for the classic Sound Manager.
//
// Layouts shared by i386 and the 64-bit host: AudioStreamBasicDescription (40),
// AudioTimeStamp (64), AudioComponentDescription (20). Different:
// AudioBuffer {UInt32 ch; UInt32 size; void *data} is 12 bytes in the guest.
#include "common.h"

#include <math.h>
#include <time.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <pthread.h>
#include <stdlib.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD");
_Static_assert(sizeof(AudioTimeStamp) == 64, "AudioTimeStamp");

// --- OpenAL --------------------------------------------------------------------------
#define DEV(i) ((ALCdevice *)hobj_unwrap(ARG(i)))
#define CTX(i) ((ALCcontext *)hobj_unwrap(ARG(i)))

THUNK(alcOpenDevice) { RET(hobj_wrap(alcOpenDevice(ARG(0) ? ARGS(0) : NULL))); }
THUNK(alcCloseDevice) { RET(alcCloseDevice(DEV(0))); }
THUNK(alcCreateContext) { RET(hobj_wrap(alcCreateContext(DEV(0), ARG(1) ? (const ALCint *)ARGP(1) : NULL))); }
THUNK(alcDestroyContext) { alcDestroyContext(CTX(0)); }
THUNK(alcMakeContextCurrent) { RET(alcMakeContextCurrent(CTX(0))); }
THUNK(alcGetCurrentContext) { RET(hobj_wrap(alcGetCurrentContext())); }
THUNK(alcGetContextsDevice) { RET(hobj_wrap(alcGetContextsDevice(CTX(0)))); }
THUNK(alcIsExtensionPresent) { RET(alcIsExtensionPresent(DEV(0), ARGS(1))); }
THUNK(alcGetEnumValue) { RET(alcGetEnumValue(DEV(0), ARGS(1))); }
THUNK(alcGetError) { RET(alcGetError(DEV(0))); }
THUNK(alcGetString) {
    const ALCchar *s = alcGetString(DEV(0), ARGI(1));
    RET(s ? guest_static_string(6, s) : 0);
}
THUNK(alcGetIntegerv) { alcGetIntegerv(DEV(0), ARGI(1), ARGI(2), ARGP(3)); }
// Extension entry points: only those we implement as thunks can be handed out.
THUNK(alcGetProcAddress) {
    char name[256];
    snprintf(name, sizeof name, "_%s", ARGS(1));
    gaddr_t a = hostsym_lookup(name);
    if (!a) LOGI("openal: alcGetProcAddress(%s) not available", ARGS(1));
    RET(a);
}
THUNK(alGetProcAddress) {
    char name[256];
    snprintf(name, sizeof name, "_%s", ARGS(0));
    RET(hostsym_lookup(name));
}
THUNK(alIsExtensionPresent) { RET(alIsExtensionPresent(ARGS(0))); }
THUNK(alGetError) { RET(alGetError()); }
THUNK(alGetString) {
    const ALchar *s = alGetString(ARGI(0));
    RET(s ? guest_static_string(6, s) : 0);
}
THUNK(alGenBuffers) { alGenBuffers(ARGI(0), ARGP(1)); }
THUNK(alDeleteBuffers) { alDeleteBuffers(ARGI(0), ARGP(1)); }
THUNK(alGenSources) { alGenSources(ARGI(0), ARGP(1)); }
THUNK(alDeleteSources) { alDeleteSources(ARGI(0), ARGP(1)); }
THUNK(alBufferData) { alBufferData(ARG(0), ARGI(1), ARGP(2), ARGI(3), ARGI(4)); }
THUNK(alDistanceModel) { alDistanceModel(ARGI(0)); }
THUNK(alGetFloat) { RETD(alGetFloat(ARGI(0))); }
THUNK(alGetInteger) { RET(alGetInteger(ARGI(0))); }
THUNK(alGetSourcei) { alGetSourcei(ARG(0), ARGI(1), ARGP(2)); }
THUNK(alGetSourcef) { alGetSourcef(ARG(0), ARGI(1), ARGP(2)); }
THUNK(alSourcef) { alSourcef(ARG(0), ARGI(1), ARGF(2)); }
THUNK(alSourcei) { alSourcei(ARG(0), ARGI(1), ARGI(2)); }
THUNK(alSourcefv) { alSourcefv(ARG(0), ARGI(1), ARGP(2)); }
THUNK(alSource3f) { alSource3f(ARG(0), ARGI(1), ARGF(2), ARGF(3), ARGF(4)); }
THUNK(alSourcePlay) { alSourcePlay(ARG(0)); }
THUNK(alSourceStop) { alSourceStop(ARG(0)); }
THUNK(alSourcePause) { alSourcePause(ARG(0)); }
THUNK(alSourceQueueBuffers) { alSourceQueueBuffers(ARG(0), ARGI(1), ARGP(2)); }
THUNK(alSourceUnqueueBuffers) { alSourceUnqueueBuffers(ARG(0), ARGI(1), ARGP(2)); }
THUNK(alListenerf) { alListenerf(ARGI(0), ARGF(1)); }
THUNK(alListenerfv) { alListenerfv(ARGI(0), ARGP(1)); }
THUNK(alListener3f) { alListener3f(ARGI(0), ARGF(1), ARGF(2), ARGF(3)); }

// --- AudioBufferList conversion ----------------------------------------------------------
// Guest: { UInt32 n; { UInt32 ch; UInt32 size; void *data } [n] }

static AudioBufferList *abl_alloc(UInt32 n) {
    AudioBufferList *l = calloc(1, offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * (n ? n : 1));
    l->mNumberBuffers = n;
    return l;
}

// Guest list -> host list pointing at the same (guest) data buffers.
static AudioBufferList *abl_from_guest(gaddr_t g) {
    UInt32 n = gread32(g);
    AudioBufferList *l = abl_alloc(n);
    for (UInt32 i = 0; i < n; i++) {
        gaddr_t b = g + 4 + 12 * i;
        l->mBuffers[i].mNumberChannels = gread32(b);
        l->mBuffers[i].mDataByteSize = gread32(b + 4);
        l->mBuffers[i].mData = g2h(gread32(b + 8));
    }
    return l;
}

static void abl_sizes_to_guest(const AudioBufferList *l, gaddr_t g) {
    for (UInt32 i = 0; i < l->mNumberBuffers && i < gread32(g); i++) gwrite32(g + 4 + 12 * i + 4, l->mBuffers[i].mDataByteSize);
}

// --- CoreAudio HAL ---------------------------------------------------------------------------
// AudioObjectPropertyAddress is three UInt32s on both sides.

THUNK(AudioObjectGetPropertyDataSize) {
    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(ARG(0), ARGP(1), ARG(2), ARG(3) ? ARGP(3) : NULL, &size);
    gwrite32(ARG(4), size);
    RET(st);
}

// Properties whose values contain pointers or CF objects need conversion.
static bool cf_valued_property(AudioObjectPropertySelector sel) {
    switch (sel) {
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioObjectPropertyElementName:
        return true;
    default: return false;
    }
}

THUNK(AudioObjectGetPropertyData) {
    const AudioObjectPropertyAddress *addr = ARGP(1);
    UInt32 size = gread32(ARG(4));
    if (cf_valued_property(addr->mSelector)) {
        CFTypeRef v = NULL;
        UInt32 hs = sizeof v;
        OSStatus st = AudioObjectGetPropertyData(ARG(0), addr, ARG(2), ARG(3) ? ARGP(3) : NULL, &hs, &v);
        if (st == noErr) {
            gwrite32(ARG(5), hobj_wrap(v));
            gwrite32(ARG(4), 4);
        }
        RET(st);
    }
    OSStatus st = AudioObjectGetPropertyData(ARG(0), addr, ARG(2), ARG(3) ? ARGP(3) : NULL, &size, ARGP(5));
    gwrite32(ARG(4), size);
    RET(st);
}

THUNK(AudioObjectSetPropertyData) {
    RET(AudioObjectSetPropertyData(ARG(0), ARGP(1), ARG(2), ARG(3) ? ARGP(3) : NULL, ARG(4), ARGP(5)));
}
THUNK(AudioObjectHasProperty) { RET(AudioObjectHasProperty(ARG(0), ARGP(1))); }

// Legacy HAL API (removed from the 64-bit SDK) mapped onto AudioObject calls.
static OSStatus legacy_get(AudioObjectID obj, AudioObjectPropertyScope scope, UInt32 element,
                           AudioObjectPropertySelector sel, gaddr_t io_size, gaddr_t out) {
    AudioObjectPropertyAddress a = {sel, scope, element};
    UInt32 size = io_size ? gread32(io_size) : 0;
    OSStatus st;
    if (cf_valued_property(sel) || sel == kAudioDevicePropertyDeviceNameCFString) {
        CFTypeRef v = NULL;
        UInt32 hs = sizeof v;
        st = AudioObjectGetPropertyData(obj, &a, 0, NULL, &hs, &v);
        if (!st && out) gwrite32(out, hobj_wrap(v));
        if (io_size) gwrite32(io_size, 4);
        return st;
    }
    if (sel == kAudioDevicePropertyDeviceName) { // C string
        CFStringRef name = NULL;
        UInt32 hs = sizeof name;
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, scope, element};
        st = AudioObjectGetPropertyData(obj, &na, 0, NULL, &hs, &name);
        if (!st && name) {
            char buf[256];
            CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
            CFRelease(name);
            if (out) gstrlcpy(out, buf, size);
            if (io_size) gwrite32(io_size, (uint32_t)strlen(buf) + 1);
        }
        return st;
    }
    if (!out) return AudioObjectGetPropertyDataSize(obj, &a, 0, NULL, io_size ? (UInt32 *)g2h(io_size) : &size);
    st = AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, g2h(out));
    if (io_size) gwrite32(io_size, size);
    return st;
}

// AudioHardwareGetProperty(AudioHardwarePropertyID, UInt32 *ioSize, void *outData)
THUNK(AudioHardwareGetProperty) {
    RET(legacy_get(kAudioObjectSystemObject, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain, ARG(0),
                   ARG(1), ARG(2)));
}
// AudioHardwareGetPropertyInfo(id, UInt32 *outSize, Boolean *outWritable)
THUNK(AudioHardwareGetPropertyInfo) {
    AudioObjectPropertyAddress a = {ARG(0), kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    if (ARG(1)) gwrite32(ARG(1), size);
    if (ARG(2)) {
        Boolean w = false;
        AudioObjectIsPropertySettable(kAudioObjectSystemObject, &a, &w);
        gwrite8(ARG(2), w);
    }
    RET(st);
}
THUNK(AudioHardwareAddPropertyListener) {
    LOGD("audio: AudioHardwareAddPropertyListener(%.4s) ignored", (const char *)&r->esp);
    RET(noErr);
}
THUNK(AudioHardwareRemovePropertyListener) { RET(noErr); }

static AudioObjectPropertyScope legacy_scope(uint32_t is_input) {
    return is_input & 0xff ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
}
// AudioDeviceGetProperty(dev, UInt32 channel, Boolean isInput, id, UInt32 *ioSize, void *out)
THUNK(AudioDeviceGetProperty) { RET(legacy_get(ARG(0), legacy_scope(ARG(2)), ARG(1), ARG(3), ARG(4), ARG(5))); }
// AudioDeviceGetPropertyInfo(dev, channel, isInput, id, UInt32 *outSize, Boolean *outWritable)
THUNK(AudioDeviceGetPropertyInfo) {
    AudioObjectPropertyAddress a = {ARG(3), legacy_scope(ARG(2)), ARG(1)};
    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(ARG(0), &a, 0, NULL, &size);
    if (ARG(4)) gwrite32(ARG(4), size);
    if (ARG(5)) {
        Boolean w = false;
        AudioObjectIsPropertySettable(ARG(0), &a, &w);
        gwrite8(ARG(5), w);
    }
    RET(st);
}
// AudioDeviceSetProperty(dev, const AudioTimeStamp *when, channel, isInput, id, size, data)
THUNK(AudioDeviceSetProperty) {
    AudioObjectPropertyAddress a = {ARG(4), legacy_scope(ARG(3)), ARG(2)};
    RET(AudioObjectSetPropertyData(ARG(0), &a, 0, NULL, ARG(5), ARGP(6)));
}

// --- AudioUnit ---------------------------------------------------------------------------------
#define AU(i) ((AudioUnit)hobj_unwrap(ARG(i)))

struct render_cb {
    gaddr_t proc, refcon;
    gaddr_t flags, ts, abl; // guest scratch: AudioUnitRenderActionFlags, AudioTimeStamp, AudioBufferList
    gaddr_t data;           // guest audio data buffer
    uint32_t data_cap;
    uint32_t calls;
    // real-time budget (logged with the level): guest render time per callback
    uint64_t worst_ns;
    uint32_t over_budget; // callbacks that took more than half the buffer duration
};

static OSStatus host_render(void *refcon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                            UInt32 frames, AudioBufferList *io) {
    struct render_cb *cb = refcon;
    cpu_thread_attach();
    UInt32 n = io ? io->mNumberBuffers : 0;
    uint32_t total = 0;
    for (UInt32 i = 0; i < n; i++) total += io->mBuffers[i].mDataByteSize;
    if (total > cb->data_cap) {
        gheap_free(cb->data);
        cb->data_cap = total * 2;
        cb->data = gheap_alloc(cb->data_cap);
    }
    if (!cb->abl || gread32(cb->abl) < n) {
        gheap_free(cb->abl);
        cb->abl = gheap_calloc(1, 4 + 12 * (n ? n : 1));
    }
    gwrite32(cb->abl, n);
    uint32_t off = 0;
    for (UInt32 i = 0; i < n; i++) {
        gaddr_t b = cb->abl + 4 + 12 * i;
        gwrite32(b, io->mBuffers[i].mNumberChannels);
        gwrite32(b + 4, io->mBuffers[i].mDataByteSize);
        gwrite32(b + 8, io->mBuffers[i].mData ? cb->data + off : 0);
        off += io->mBuffers[i].mDataByteSize;
    }
    gwrite32(cb->flags, flags ? *flags : 0);
    if (ts) memcpy(g2h(cb->ts), ts, sizeof *ts);
    uint32_t args[6] = {cb->refcon, cb->flags, cb->ts, bus, frames, cb->abl};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    OSStatus st = (OSStatus)cpu_call(cb->proc, 6, args);
    uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    if (dt > cb->worst_ns) cb->worst_ns = dt;
    if (dt > (uint64_t)frames * 1000000000ull / 48000 / 2) cb->over_budget++; // half of a 48 kHz buffer
    if (cb->calls++ == 0 || st)
        if (cb->calls < 4) LOGI("audio: render callback #%u: %u frames, %u buffers, status %d", cb->calls, frames, n, (int)st);
    if (flags) *flags = gread32(cb->flags);
    off = 0;
    for (UInt32 i = 0; i < n; i++) {
        gaddr_t b = cb->abl + 4 + 12 * i;
        uint32_t size = gread32(b + 4);
        gaddr_t data = gread32(b + 8);
        io->mBuffers[i].mDataByteSize = size;
        if (io->mBuffers[i].mData && data) memcpy(io->mBuffers[i].mData, g2h(data), size);
        else if (!io->mBuffers[i].mData && data) io->mBuffers[i].mData = g2h(data); // guest supplied its own buffer
        off += size;
    }
    if (cb->calls % 500 == 1 && n && io->mBuffers[0].mData) {
        // Level check (float32 output): silence vs garbage vs signal.
        const float *s = io->mBuffers[0].mData;
        uint32_t cnt = io->mBuffers[0].mDataByteSize / 4, nans = 0;
        float peak = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (s[i] != s[i]) nans++;
            else if (fabsf(s[i]) > peak) peak = fabsf(s[i]);
        }
        LOGI("audio: render #%u peak %.4f nan %u flags %#x, worst %.2f ms, %u over half budget", cb->calls, peak,
             nans, flags ? *flags : 0, cb->worst_ns / 1e6, cb->over_budget);
        cb->worst_ns = 0;
        cb->over_budget = 0;
    }
    return st;
}

static struct render_cb *new_render_cb(gaddr_t proc, gaddr_t refcon) {
    struct render_cb *cb = calloc(1, sizeof *cb);
    cb->proc = proc;
    cb->refcon = refcon;
    cb->flags = gheap_calloc(1, 4);
    cb->ts = gheap_calloc(1, sizeof(AudioTimeStamp));
    return cb;
}

// AudioUnitSetProperty(unit, id, scope, element, const void *data, UInt32 size)
THUNK(AudioUnitSetProperty) {
    AudioUnitPropertyID id = ARG(1);
    if (id == kAudioUnitProperty_SetRenderCallback || id == kAudioOutputUnitProperty_SetInputCallback) {
        // AURenderCallbackStruct { proc; refCon } -- two guest words
        gaddr_t p = ARG(4);
        AURenderCallbackStruct hcb = {0};
        if (p && gread32(p)) {
            hcb.inputProc = host_render;
            hcb.inputProcRefCon = new_render_cb(gread32(p), gread32(p + 4));
        }
        OSStatus st = AudioUnitSetProperty(AU(0), id, ARG(2), ARG(3), &hcb, sizeof hcb);
        LOGI("audio: render callback %#x on unit %#x = %d", p ? gread32(p) : 0, ARG(0), (int)st);
        RET(st);
    }
    OSStatus st = AudioUnitSetProperty(AU(0), id, ARG(2), ARG(3), ARG(4) ? ARGP(4) : NULL, ARG(5));
    if (st) LOGI("audio: AudioUnitSetProperty(%#x, id %u, scope %u) = %d", ARG(0), id, ARG(2), (int)st);
    RET(st);
}

// AudioUnitGetProperty(unit, id, scope, element, void *data, UInt32 *ioSize)
THUNK(AudioUnitGetProperty) {
    UInt32 size = ARG(5) ? gread32(ARG(5)) : 0;
    AudioUnitPropertyID id = ARG(1);
    if (id == kAudioUnitProperty_ClassInfo || id == kAudioUnitProperty_CocoaUI || id == kAudioUnitProperty_FastDispatch) {
        LOGI("audio: AudioUnitGetProperty(%u) with pointer contents not supported", id);
        RET(kAudioUnitErr_InvalidProperty);
    }
    OSStatus st = AudioUnitGetProperty(AU(0), id, ARG(2), ARG(3), ARG(4) ? ARGP(4) : NULL, &size);
    if (ARG(5)) gwrite32(ARG(5), size);
    RET(st);
}
THUNK(AudioUnitGetPropertyInfo) {
    UInt32 size = 0;
    Boolean w = false;
    OSStatus st = AudioUnitGetPropertyInfo(AU(0), ARG(1), ARG(2), ARG(3), &size, &w);
    if (ARG(4)) gwrite32(ARG(4), size);
    if (ARG(5)) gwrite8(ARG(5), w);
    RET(st);
}
THUNK(AudioUnitInitialize) {
    OSStatus st = AudioUnitInitialize(AU(0));
    LOGI("audio: AudioUnitInitialize(%#x) = %d", ARG(0), (int)st);
    RET(st);
}
THUNK(AudioUnitUninitialize) { RET(AudioUnitUninitialize(AU(0))); }
THUNK(AudioOutputUnitStart) {
    OSStatus st = AudioOutputUnitStart(AU(0));
    LOGI("audio: AudioOutputUnitStart(%#x) = %d", ARG(0), (int)st);
    RET(st);
}
THUNK(AudioOutputUnitStop) {
    LOGD("audio: AudioOutputUnitStop(%#x)", ARG(0));
    RET(AudioOutputUnitStop(AU(0)));
}
THUNK(AudioUnitSetParameter) { RET(AudioUnitSetParameter(AU(0), ARG(1), ARG(2), ARG(3), ARGF(4), ARG(5))); }
THUNK(AudioUnitGetParameter) { RET(AudioUnitGetParameter(AU(0), ARG(1), ARG(2), ARG(3), ARGP(4))); }
// AudioUnitRender(unit, flags*, const AudioTimeStamp *, bus, frames, AudioBufferList *)
THUNK(AudioUnitRender) {
    AudioBufferList *l = abl_from_guest(ARG(5));
    OSStatus st = AudioUnitRender(AU(0), ARG(1) ? ARGP(1) : NULL, ARGP(2), ARG(3), ARG(4), l);
    abl_sizes_to_guest(l, ARG(5));
    free(l);
    RET(st);
}

// --- AUGraph ----------------------------------------------------------------------------------------
#define GRAPH(i) ((AUGraph)hobj_unwrap(ARG(i)))
THUNK(NewAUGraph) {
    AUGraph g = NULL;
    OSStatus st = NewAUGraph(&g);
    gwrite32(ARG(0), hobj_wrap(g));
    RET(st);
}
THUNK(DisposeAUGraph) { RET(DisposeAUGraph(GRAPH(0))); }
THUNK(AUGraphAddNode) { RET(AUGraphAddNode(GRAPH(0), ARGP(1), ARGP(2))); }
THUNK(AUGraphOpen) { RET(AUGraphOpen(GRAPH(0))); }
THUNK(AUGraphInitialize) { RET(AUGraphInitialize(GRAPH(0))); }
THUNK(AUGraphStart) { RET(AUGraphStart(GRAPH(0))); }
THUNK(AUGraphStop) { RET(AUGraphStop(GRAPH(0))); }
THUNK(AUGraphConnectNodeInput) { RET(AUGraphConnectNodeInput(GRAPH(0), ARG(1), ARG(2), ARG(3), ARG(4))); }
// AUGraphNodeInfo(graph, node, AudioComponentDescription *outDesc, AudioUnit *outUnit)
THUNK(AUGraphNodeInfo) {
    AudioUnit u = NULL;
    OSStatus st = AUGraphNodeInfo(GRAPH(0), ARG(1), ARG(2) ? ARGP(2) : NULL, ARG(3) ? &u : NULL);
    if (ARG(3)) gwrite32(ARG(3), hobj_wrap(u));
    RET(st);
}

// --- AudioQueue -------------------------------------------------------------------------------------
// Guest AudioQueueBuffer (i386): { UInt32 capacity; void *data; UInt32 byteSize; void *userData;
//   UInt32 pdCapacity; AudioStreamPacketDescription *pd; UInt32 pdCount } = 28 bytes.
struct gqueue {
    AudioQueueRef q;
    gaddr_t cb, user, handle;
    struct { AudioQueueBufferRef h; gaddr_t g; } bufs[64];
    int nbufs;
};

static gaddr_t guest_buffer_for(struct gqueue *gq, AudioQueueBufferRef b) {
    for (int i = 0; i < gq->nbufs; i++)
        if (gq->bufs[i].h == b) return gq->bufs[i].g;
    return 0;
}

static void queue_output_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    struct gqueue *gq = user;
    cpu_thread_attach();
    gcall3(gq->cb, gq->user, gq->handle, guest_buffer_for(gq, b));
}

#define GQ(i) ((struct gqueue *)hobj_unwrap(ARG(i)))

// AudioQueueNewOutput(const ASBD *, callback, userData, CFRunLoopRef, CFStringRef mode, UInt32 flags, AudioQueueRef *out)
THUNK(AudioQueueNewOutput) {
    struct gqueue *gq = calloc(1, sizeof *gq);
    gq->cb = ARG(1);
    gq->user = ARG(2);
    // Callbacks run on an internal queue thread rather than the guest run loop.
    OSStatus st = AudioQueueNewOutput(ARGP(0), queue_output_cb, gq, NULL, NULL, 0, &gq->q);
    if (st) {
        free(gq);
        RET(st);
    }
    gq->handle = hobj_wrap(gq);
    gwrite32(ARG(6), gq->handle);
    RET(noErr);
}
THUNK(AudioQueueDispose) {
    struct gqueue *gq = GQ(0);
    OSStatus st = AudioQueueDispose(gq->q, ARG(1) & 0xff);
    for (int i = 0; i < gq->nbufs; i++) gheap_free(gq->bufs[i].g);
    RET(st);
}
THUNK(AudioQueueAllocateBuffer) {
    struct gqueue *gq = GQ(0);
    AudioQueueBufferRef b = NULL;
    OSStatus st = AudioQueueAllocateBuffer(gq->q, ARG(1), &b);
    if (st) RET(st);
    gaddr_t g = gheap_calloc(1, 28 + ARG(1));
    gwrite32(g, ARG(1));
    gwrite32(g + 4, g + 28);
    if (gq->nbufs < 64) gq->bufs[gq->nbufs++] = (__typeof__(gq->bufs[0])){b, g};
    gwrite32(ARG(2), g);
    RET(noErr);
}
// AudioQueueEnqueueBuffer(queue, buffer, UInt32 numPacketDescs, const AudioStreamPacketDescription *)
THUNK(AudioQueueEnqueueBuffer) {
    struct gqueue *gq = GQ(0);
    gaddr_t g = ARG(1);
    AudioQueueBufferRef b = NULL;
    for (int i = 0; i < gq->nbufs; i++)
        if (gq->bufs[i].g == g) b = gq->bufs[i].h;
    if (!b) RET(kAudioQueueErr_InvalidBuffer);
    uint32_t size = gread32(g + 8);
    if (size > b->mAudioDataBytesCapacity) size = b->mAudioDataBytesCapacity;
    memcpy(b->mAudioData, g2h(gread32(g + 4)), size);
    b->mAudioDataByteSize = size;
    // AudioStreamPacketDescription { SInt64 start; UInt32 variableFrames; UInt32 dataByteSize } = 16 bytes both sides
    RET(AudioQueueEnqueueBuffer(gq->q, b, ARG(2), ARG(2) ? ARGP(3) : NULL));
}
THUNK(AudioQueueStart) { RET(AudioQueueStart(GQ(0)->q, ARG(1) ? ARGP(1) : NULL)); }
THUNK(AudioQueueStop) { RET(AudioQueueStop(GQ(0)->q, ARG(1) & 0xff)); }
THUNK(AudioQueuePause) { RET(AudioQueuePause(GQ(0)->q)); }
THUNK(AudioQueueSetParameter) { RET(AudioQueueSetParameter(GQ(0)->q, ARG(1), ARGF(2))); }
THUNK(AudioQueueGetParameter) { RET(AudioQueueGetParameter(GQ(0)->q, ARG(1), ARGP(2))); }
THUNK(AudioQueueSetProperty) { RET(AudioQueueSetProperty(GQ(0)->q, ARG(1), ARGP(2), ARG(3))); }
THUNK(AudioQueueGetProperty) {
    UInt32 size = gread32(ARG(3));
    OSStatus st = AudioQueueGetProperty(GQ(0)->q, ARG(1), ARGP(2), &size);
    gwrite32(ARG(3), size);
    RET(st);
}
THUNK(AudioQueueGetPropertySize) {
    UInt32 size = 0;
    OSStatus st = AudioQueueGetPropertySize(GQ(0)->q, ARG(1), &size);
    gwrite32(ARG(2), size);
    RET(st);
}
struct qlistener {
    gaddr_t proc, user;
    struct gqueue *gq;
};
static void queue_prop_cb(void *user, AudioQueueRef q, AudioQueuePropertyID id) {
    struct qlistener *l = user;
    cpu_thread_attach();
    gcall3(l->proc, l->user, l->gq->handle, id);
}
THUNK(AudioQueueAddPropertyListener) {
    struct qlistener *l = calloc(1, sizeof *l);
    l->proc = ARG(2);
    l->user = ARG(3);
    l->gq = GQ(0);
    RET(AudioQueueAddPropertyListener(l->gq->q, ARG(1), queue_prop_cb, l));
}

// --- AudioFile / ExtAudioFile with guest callbacks -----------------------------------------------
struct gafile {
    gaddr_t client, read, write, get_size, set_size;
    gaddr_t buf;
    uint32_t buf_cap;
};

// AudioFile_ReadProc(void *client, SInt64 pos, UInt32 count, void *buffer, UInt32 *actual)
static OSStatus af_read(void *user, SInt64 pos, UInt32 count, void *buffer, UInt32 *actual) {
    struct gafile *f = user;
    cpu_thread_attach();
    if (count + 4 > f->buf_cap) {
        gheap_free(f->buf);
        f->buf_cap = count + 4096;
        f->buf = gheap_alloc(f->buf_cap);
    }
    gaddr_t act = f->buf; // first word: actual count
    uint32_t args[6] = {f->client, (uint32_t)pos, (uint32_t)((uint64_t)pos >> 32), count, f->buf + 4, act};
    OSStatus st = (OSStatus)cpu_call(f->read, 6, args);
    uint32_t got = gread32(act);
    if (got > count) got = count;
    memcpy(buffer, g2h(f->buf + 4), got);
    *actual = got;
    return st;
}
static SInt64 af_get_size(void *user) {
    struct gafile *f = user;
    cpu_thread_attach();
    return (SInt64)cpu_call(f->get_size, 1, &f->client);
}

// AudioFileOpenWithCallbacks(client, read, write, getSize, setSize, typeHint, AudioFileID *out)
THUNK(AudioFileOpenWithCallbacks) {
    struct gafile *f = calloc(1, sizeof *f);
    f->client = ARG(0);
    f->read = ARG(1);
    f->write = ARG(2);
    f->get_size = ARG(3);
    f->set_size = ARG(4);
    AudioFileID id = NULL;
    OSStatus st = AudioFileOpenWithCallbacks(f, af_read, NULL, af_get_size, NULL, ARG(5), &id);
    if (st) {
        free(f);
        RET(st);
    }
    gwrite32(ARG(6), hobj_wrap(id));
    RET(noErr);
}
THUNK(AudioFileClose) { RET(AudioFileClose(hobj_unwrap(ARG(0)))); }

#define XAF(i) ((ExtAudioFileRef)hobj_unwrap(ARG(i)))
THUNK(ExtAudioFileWrapAudioFileID) {
    ExtAudioFileRef x = NULL;
    OSStatus st = ExtAudioFileWrapAudioFileID(hobj_unwrap(ARG(0)), ARG(1) & 0xff, &x);
    gwrite32(ARG(2), hobj_wrap(x));
    RET(st);
}
THUNK(ExtAudioFileDispose) { RET(ExtAudioFileDispose(XAF(0))); }
// Properties used by games are plain data (ASBD, SInt64 frame counts).
THUNK(ExtAudioFileGetProperty) {
    UInt32 size = gread32(ARG(2));
    OSStatus st = ExtAudioFileGetProperty(XAF(0), ARG(1), &size, ARGP(3));
    gwrite32(ARG(2), size);
    RET(st);
}
THUNK(ExtAudioFileSetProperty) { RET(ExtAudioFileSetProperty(XAF(0), ARG(1), ARG(2), ARGP(3))); }
THUNK(ExtAudioFileRead) {
    AudioBufferList *l = abl_from_guest(ARG(2));
    OSStatus st = ExtAudioFileRead(XAF(0), ARGP(1), l);
    abl_sizes_to_guest(l, ARG(2));
    free(l);
    RET(st);
}
THUNK(ExtAudioFileSeek) { RET(ExtAudioFileSeek(XAF(0), (SInt64)ARG64(1))); }

// --- classic Sound Manager / Sound Input Manager (FMOD fallbacks): unavailable ----------------------
#define notOpenErr (-28)
THUNK(SndSoundManagerVersion) { RET(0); }
THUNK(SndNewChannel) { RET(notOpenErr); }
THUNK(SndDisposeChannel) { RET(notOpenErr); }
THUNK(SndDoCommand) { RET(notOpenErr); }
THUNK(SndDoImmediate) { RET(notOpenErr); }
THUNK(SPBOpenDevice) { RET(notOpenErr); }
THUNK(SPBCloseDevice) { RET(notOpenErr); }
THUNK(SPBGetDeviceInfo) { RET(notOpenErr); }
THUNK(SPBSetDeviceInfo) { RET(notOpenErr); }
THUNK(SPBRecord) { RET(notOpenErr); }
THUNK(SPBStopRecording) { RET(notOpenErr); }
