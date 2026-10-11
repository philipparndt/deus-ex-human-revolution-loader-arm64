// AppleEvent descriptor conversion shared by the CoreServices AE thunks and the
// Carbon side (AESend etc.).
//
// Guest AEDesc (i386, 8 bytes): { DescType descriptorType @0; AEDataStorage dataHandle @4 }
// The host dataHandle (an opaque host pointer) is represented in the guest by a
// hobj handle. AppleEvent, AEDescList, AERecord and AEAddressDesc are AEDescs.
#pragma once
#include "common.h"

#include <CoreServices/CoreServices.h>

#define G_AEDESC_SIZE 8

static inline void aedesc_from_guest(AEDesc *h, gaddr_t g) {
    if (!g) {
        h->descriptorType = typeNull;
        h->dataHandle = NULL;
        return;
    }
    h->descriptorType = gread32(g);
    h->dataHandle = (AEDataStorage)hobj_unwrap(gread32(g + 4));
}

static inline void aedesc_to_guest(gaddr_t g, const AEDesc *h) {
    if (!g) return;
    gwrite32(g, h->descriptorType);
    gwrite32(g + 4, hobj_wrap(h->dataHandle));
}
