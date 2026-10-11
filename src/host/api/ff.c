// ForceFeedback: no force feedback devices are reported.
#include "common.h"
#include "ret_return.h"

#define FFERR_NOINTERFACE 0x80004002u
#define FFERR_INVALIDPARAM 0x80070057u

THUNK(FFIsForceFeedback) { RET(FFERR_NOINTERFACE); }
THUNK(FFCreateDevice) {
    LOGI("ff: FFCreateDevice(%#x) -> no force feedback", ARG(0));
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET(FFERR_NOINTERFACE);
}
THUNK(FFReleaseDevice) { RET(0); }
THUNK(FFDeviceCreateEffect) {
    if (ARG(3)) gwrite32(ARG(3), 0);
    RET(FFERR_INVALIDPARAM);
}
THUNK(FFDeviceReleaseEffect) { RET(0); }
THUNK(FFDeviceEscape) { RET(FFERR_INVALIDPARAM); }
THUNK(FFEffectSetParameters) { RET(FFERR_INVALIDPARAM); }
THUNK(FFEffectStart) { RET(FFERR_INVALIDPARAM); }
THUNK(FFEffectStop) { RET(FFERR_INVALIDPARAM); }
THUNK(FFDeviceSendForceFeedbackCommand) { RET(FFERR_INVALIDPARAM); }
THUNK(FFDeviceGetForceFeedbackCapabilities) { RET(FFERR_INVALIDPARAM); }
