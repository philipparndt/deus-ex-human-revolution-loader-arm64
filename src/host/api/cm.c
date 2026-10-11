// CoreMedia. CMTime {int64 value; int32 timescale; u32 flags; int64 epoch} is 24
// bytes on both architectures; by value it takes 6 stack slots, as a result it is
// returned through a hidden pointer. AudioStreamBasicDescription (40) and
// AudioStreamPacketDescription (16) also have identical layouts.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <CoreMedia/CoreMedia.h>
#include "ret_return.h"

__asm__(".linker_option \"-framework\", \"CoreMedia\"");

_Static_assert(sizeof(CMTime) == 24, "CMTime");
_Static_assert(sizeof(CMTimeRange) == 48, "CMTimeRange");
_Static_assert(sizeof(AudioStreamPacketDescription) == 16, "ASPD");
_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD");

static CMTime arg_cmtime(struct guest_regs *r, int i) {
    CMTime t;
    memcpy(&t, g2h(r->esp + 4 + 4 * (uint32_t)i), sizeof t);
    return t;
}
#define ARGTIME(i) arg_cmtime(r, i)
#define RET_STRUCT(v)                                                                              \
    do {                                                                                           \
        __typeof__(v) v_ = (v);                                                                    \
        memcpy(ARGP(0), &v_, sizeof v_);                                                           \
        r->pop = 4;                                                                                \
        RET(ARG(0));                                                                               \
    } while (0)

static void init_zero(gaddr_t a) { memcpy(g2h(a), &kCMTimeZero, sizeof(CMTime)); }
GDATA(kCMTimeZero, 24, init_zero);
static void init_inval(gaddr_t a) { memcpy(g2h(a), &kCMTimeInvalid, sizeof(CMTime)); }
GDATA(kCMTimeInvalid, 24, init_inval);
static void init_pinf(gaddr_t a) { memcpy(g2h(a), &kCMTimePositiveInfinity, sizeof(CMTime)); }
GDATA(kCMTimePositiveInfinity, 24, init_pinf);

THUNK(CMTimeGetSeconds) { RETD(CMTimeGetSeconds(ARGTIME(0))); }
THUNK(CMTimeCompare) { RET((uint32_t)CMTimeCompare(ARGTIME(0), ARGTIME(6))); }
THUNK(CMTimeMake) { RET_STRUCT(CMTimeMake((int64_t)ARG64(1), ARGI(3))); }
THUNK(CMTimeMakeWithSeconds) { RET_STRUCT(CMTimeMakeWithSeconds(ARGD(1), ARGI(3))); }
THUNK(CMTimeAdd) { RET_STRUCT(CMTimeAdd(ARGTIME(1), ARGTIME(7))); }
THUNK(CMTimeSubtract) { RET_STRUCT(CMTimeSubtract(ARGTIME(1), ARGTIME(7))); }
THUNK(CMTimeConvertScale) { RET_STRUCT(CMTimeConvertScale(ARGTIME(1), ARGI(7), ARG(8))); }
THUNK(CMTimeRangeMake) { RET_STRUCT(CMTimeRangeMake(ARGTIME(1), ARGTIME(7))); }

THUNK(CMSampleBufferGetPresentationTimeStamp) {
    RET_STRUCT(CMSampleBufferGetPresentationTimeStamp((CMSampleBufferRef)ARGCF(1)));
}
THUNK(CMSampleBufferGetDuration) { RET_STRUCT(CMSampleBufferGetDuration((CMSampleBufferRef)ARGCF(1))); }
THUNK(CMSampleBufferGetNumSamples) { RET((uint32_t)CMSampleBufferGetNumSamples((CMSampleBufferRef)ARGCF(0))); }
THUNK(CMSampleBufferGetDataBuffer) { RET_CF(CMSampleBufferGetDataBuffer((CMSampleBufferRef)ARGCF(0))); }
THUNK(CMSampleBufferGetImageBuffer) { RET_CF(CMSampleBufferGetImageBuffer((CMSampleBufferRef)ARGCF(0))); }
THUNK(CMSampleBufferGetFormatDescription) { RET_CF(CMSampleBufferGetFormatDescription((CMSampleBufferRef)ARGCF(0))); }
THUNK(CMSampleBufferGetTypeID) { RET(CMSampleBufferGetTypeID()); }
THUNK(CMSampleBufferIsValid) { RET_BOOL(CMSampleBufferIsValid((CMSampleBufferRef)ARGCF(0))); }
THUNK(CMSampleBufferInvalidate) { RET(CMSampleBufferInvalidate((CMSampleBufferRef)ARGCF(0))); }

static char aspd_key, asbd_key, cookie_key;

// CMSampleBufferGetAudioStreamPacketDescriptionsPtr(sbuf, const ASPD **out, size_t *sizeOut)
THUNK(CMSampleBufferGetAudioStreamPacketDescriptionsPtr) {
    CMSampleBufferRef sb = (CMSampleBufferRef)ARGCF(0);
    const AudioStreamPacketDescription *p = NULL;
    size_t sz = 0;
    OSStatus st = CMSampleBufferGetAudioStreamPacketDescriptionsPtr(sb, &p, &sz);
    if (ARG(1)) gwrite32(ARG(1), p ? cf_guest_copy(sb, &aspd_key, p, (uint32_t)sz) : 0);
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)sz);
    RET(st);
}

THUNK(CMBlockBufferGetDataLength) { RET(CMBlockBufferGetDataLength((CMBlockBufferRef)ARGCF(0))); }
// CMBlockBufferCopyDataBytes(buf, size_t offset, size_t length, void *dest)
THUNK(CMBlockBufferCopyDataBytes) {
    RET(CMBlockBufferCopyDataBytes((CMBlockBufferRef)ARGCF(0), ARG(1), ARG(2), ARGP(3)));
}
THUNK(CMBlockBufferGetTypeID) { RET(CMBlockBufferGetTypeID()); }

THUNK(CMAudioFormatDescriptionGetStreamBasicDescription) {
    CMAudioFormatDescriptionRef d = (CMAudioFormatDescriptionRef)ARGCF(0);
    const AudioStreamBasicDescription *a = CMAudioFormatDescriptionGetStreamBasicDescription(d);
    RET(a ? cf_guest_copy(d, &asbd_key, a, sizeof *a) : 0);
}
// const void *CMAudioFormatDescriptionGetMagicCookie(desc, size_t *sizeOut)
THUNK(CMAudioFormatDescriptionGetMagicCookie) {
    CMAudioFormatDescriptionRef d = (CMAudioFormatDescriptionRef)ARGCF(0);
    size_t sz = 0;
    const void *c = CMAudioFormatDescriptionGetMagicCookie(d, &sz);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)sz);
    RET(c ? cf_guest_copy(d, &cookie_key, c, (uint32_t)sz) : 0);
}
THUNK(CMFormatDescriptionGetMediaType) { RET(CMFormatDescriptionGetMediaType((CMFormatDescriptionRef)ARGCF(0))); }
THUNK(CMFormatDescriptionGetMediaSubType) {
    RET(CMFormatDescriptionGetMediaSubType((CMFormatDescriptionRef)ARGCF(0)));
}
