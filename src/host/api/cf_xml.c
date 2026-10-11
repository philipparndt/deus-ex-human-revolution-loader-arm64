// CoreFoundation: deprecated CFXMLNode / CFXMLTree API (used by FMOD).
// Still present in the x86_64 CoreFoundation on macOS 27.
//
// CFXMLNodeGetInfoPtr returns a type-specific struct; the guest gets an i386
// copy (CF objects as handles, CFIndex/pointers 32-bit) tied to the node's lifetime.
#include "cf_bridge.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define N(i) ((CFXMLNodeRef)ARGCF(i))

THUNK(CFXMLNodeGetTypeID) { RET(CFXMLNodeGetTypeID()); }
THUNK(CFXMLNodeGetTypeCode) { RET((uint32_t)CFXMLNodeGetTypeCode(N(0))); }
THUNK(CFXMLNodeGetString) { RET_CF(CFXMLNodeGetString(N(0))); }
THUNK(CFXMLNodeGetVersion) { RET((uint32_t)CFXMLNodeGetVersion(N(0))); }

static char info_key;

THUNK(CFXMLNodeGetInfoPtr) {
    CFXMLNodeRef n = N(0);
    const void *info = n ? CFXMLNodeGetInfoPtr(n) : NULL;
    if (!info) {
        RET(0);
        return;
    }
    uint32_t w[8] = {0};
    uint32_t size = 0;
    switch (CFXMLNodeGetTypeCode(n)) {
    case kCFXMLNodeTypeDocument: {
        const CFXMLDocumentInfo *d = info;
        w[0] = cfg(d->sourceURL);
        w[1] = d->encoding;
        size = 8;
        break;
    }
    case kCFXMLNodeTypeElement: {
        const CFXMLElementInfo *e = info;
        w[0] = cfg(e->attributes);
        w[1] = cfg(e->attributeOrder);
        w[2] = e->isEmpty ? 1 : 0; // Boolean + 3 reserved bytes
        size = 12;
        break;
    }
    case kCFXMLNodeTypeProcessingInstruction: {
        const CFXMLProcessingInstructionInfo *p = info;
        w[0] = cfg(p->dataString);
        size = 4;
        break;
    }
    case kCFXMLNodeTypeEntity: {
        const CFXMLEntityInfo *e = info;
        w[0] = (uint32_t)e->entityType;
        w[1] = cfg(e->replacementText);
        w[2] = cfg(e->entityID.systemID);
        w[3] = cfg(e->entityID.publicID);
        w[4] = cfg(e->notationName);
        size = 20;
        break;
    }
    case kCFXMLNodeTypeEntityReference: {
        const CFXMLEntityReferenceInfo *e = info;
        w[0] = (uint32_t)e->entityType;
        size = 4;
        break;
    }
    case kCFXMLNodeTypeDocumentType: {
        const CFXMLDocumentTypeInfo *d = info;
        w[0] = cfg(d->externalID.systemID);
        w[1] = cfg(d->externalID.publicID);
        size = 8;
        break;
    }
    case kCFXMLNodeTypeNotation: {
        const CFXMLNotationInfo *d = info;
        w[0] = cfg(d->externalID.systemID);
        w[1] = cfg(d->externalID.publicID);
        size = 8;
        break;
    }
    case kCFXMLNodeTypeElementTypeDeclaration: {
        const CFXMLElementTypeDeclarationInfo *d = info;
        w[0] = cfg(d->contentDescription);
        size = 4;
        break;
    }
    case kCFXMLNodeTypeAttributeListDeclaration: {
        // {CFIndex numberOfAttributes; CFXMLAttributeDeclarationInfo *attributes} + array of
        // {attributeName, typeString, defaultString} placed right after the header.
        const CFXMLAttributeListDeclarationInfo *a = info;
        uint32_t n_attr = (uint32_t)a->numberOfAttributes;
        uint32_t sz = 8 + 12 * n_attr;
        uint32_t *buf = calloc(1, sz);
        gaddr_t g = cf_guest_copy(n, &info_key, buf, sz); // allocate to learn the address
        buf[0] = n_attr;
        buf[1] = n_attr ? g + 8 : 0;
        for (uint32_t i = 0; i < n_attr; i++) {
            buf[2 + 3 * i] = cfg(a->attributes[i].attributeName);
            buf[3 + 3 * i] = cfg(a->attributes[i].typeString);
            buf[4 + 3 * i] = cfg(a->attributes[i].defaultString);
        }
        g = cf_guest_copy(n, &info_key, buf, sz);
        free(buf);
        RET(g);
        return;
    }
    default: LOGE("CFXMLNodeGetInfoPtr: unknown node type %ld", (long)CFXMLNodeGetTypeCode(n)); RET(0); return;
    }
    RET(cf_guest_copy(n, &info_key, w, size));
}

THUNK(CFXMLNodeCreate) {
    // (alloc, xmlType, dataString, additionalInfoPtr, version)
    CFXMLNodeTypeCode type = ARGIDX(1);
    gaddr_t g = ARG(3);
    union {
        CFXMLDocumentInfo doc;
        CFXMLElementInfo elem;
        CFXMLProcessingInstructionInfo pi;
        CFXMLEntityInfo ent;
        CFXMLEntityReferenceInfo eref;
        CFXMLDocumentTypeInfo dt;
        CFXMLNotationInfo nt;
        CFXMLElementTypeDeclarationInfo etd;
        CFXMLAttributeListDeclarationInfo ald;
    } u;
    memset(&u, 0, sizeof u);
    const void *info = NULL;
    CFXMLAttributeDeclarationInfo *attrs = NULL;
    if (g) {
        info = &u;
        switch (type) {
        case kCFXMLNodeTypeDocument:
            u.doc.sourceURL = (CFURLRef)gcf(gread32(g));
            u.doc.encoding = gread32(g + 4);
            break;
        case kCFXMLNodeTypeElement:
            u.elem.attributes = (CFDictionaryRef)gcf(gread32(g));
            u.elem.attributeOrder = (CFArrayRef)gcf(gread32(g + 4));
            u.elem.isEmpty = gread8(g + 8);
            break;
        case kCFXMLNodeTypeProcessingInstruction: u.pi.dataString = gcf_string(gread32(g)); break;
        case kCFXMLNodeTypeEntity:
            u.ent.entityType = (int32_t)gread32(g);
            u.ent.replacementText = gcf_string(gread32(g + 4));
            u.ent.entityID.systemID = (CFURLRef)gcf(gread32(g + 8));
            u.ent.entityID.publicID = gcf_string(gread32(g + 12));
            u.ent.notationName = gcf_string(gread32(g + 16));
            break;
        case kCFXMLNodeTypeEntityReference: u.eref.entityType = (int32_t)gread32(g); break;
        case kCFXMLNodeTypeDocumentType:
        case kCFXMLNodeTypeNotation:
            u.dt.externalID.systemID = (CFURLRef)gcf(gread32(g));
            u.dt.externalID.publicID = gcf_string(gread32(g + 4));
            break;
        case kCFXMLNodeTypeElementTypeDeclaration: u.etd.contentDescription = gcf_string(gread32(g)); break;
        case kCFXMLNodeTypeAttributeListDeclaration: {
            CFIndex n = (int32_t)gread32(g);
            gaddr_t a = gread32(g + 4);
            attrs = calloc(n > 0 ? n : 1, sizeof *attrs);
            for (CFIndex i = 0; i < n; i++) {
                attrs[i].attributeName = gcf_string(gread32(a + 12 * (uint32_t)i));
                attrs[i].typeString = gcf_string(gread32(a + 12 * (uint32_t)i + 4));
                attrs[i].defaultString = gcf_string(gread32(a + 12 * (uint32_t)i + 8));
            }
            u.ald.numberOfAttributes = n;
            u.ald.attributes = attrs;
            break;
        }
        default: info = NULL; break;
        }
    }
    CFXMLNodeRef node = CFXMLNodeCreate(gcf_allocator(ARG(0)), type, ARGSTR(2), info, ARGIDX(4));
    free(attrs);
    RET_CF(node);
}
THUNK(CFXMLNodeCreateCopy) { RET_CF(CFXMLNodeCreateCopy(gcf_allocator(ARG(0)), N(1))); }

THUNK(CFXMLTreeCreateFromData) {
    // (alloc, xmlData, dataSource URL, parseOptions, versionOfNodes)
    RET_CF(CFXMLTreeCreateFromData(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), (CFURLRef)ARGCF(2), ARG(3), ARGIDX(4)));
}
THUNK(CFXMLTreeCreateFromDataWithError) {
    CFDictionaryRef err = NULL;
    CFXMLTreeRef t = CFXMLTreeCreateFromDataWithError(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), (CFURLRef)ARGCF(2),
                                                      ARG(3), ARGIDX(4), ARG(5) ? &err : NULL);
    if (ARG(5)) gwrite32(ARG(5), cfg(err));
    RET_CF(t);
}
THUNK(CFXMLTreeCreateWithDataFromURL) {
    RET_CF(CFXMLTreeCreateWithDataFromURL(gcf_allocator(ARG(0)), (CFURLRef)ARGCF(1), ARG(2), ARGIDX(3)));
}
THUNK(CFXMLTreeCreateWithNode) { RET_CF(CFXMLTreeCreateWithNode(gcf_allocator(ARG(0)), N(1))); }
THUNK(CFXMLTreeGetNode) { RET_CF(CFXMLTreeGetNode((CFXMLTreeRef)ARGCF(0))); }
THUNK(CFXMLTreeCreateXMLData) { RET_CF(CFXMLTreeCreateXMLData(gcf_allocator(ARG(0)), (CFXMLTreeRef)ARGCF(1))); }
THUNK(CFXMLCreateStringByEscapingEntities) {
    RET_CF(CFXMLCreateStringByEscapingEntities(gcf_allocator(ARG(0)), ARGSTR(1), (CFDictionaryRef)ARGCF(2)));
}
THUNK(CFXMLCreateStringByUnescapingEntities) {
    RET_CF(CFXMLCreateStringByUnescapingEntities(gcf_allocator(ARG(0)), ARGSTR(1), (CFDictionaryRef)ARGCF(2)));
}
