// OpenCL.framework: the four context functions the game imports (it probably
// probes for a GL-sharing compute context). OpenCL is loaded with dlopen so the
// build does not need -framework OpenCL.
//
// cl_context / cl_device_id are host pointers -> hobj handles. size_t and
// cl_context_properties (intptr_t) are 32-bit in the guest; cl_device_type is
// a 64-bit bitfield.
#include "common.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_device_type;
typedef intptr_t cl_context_properties;
typedef void *cl_context;
typedef void (*cl_notify_fn)(const char *, const void *, size_t, void *);

#define CL_INVALID_VALUE (-30)
#define CL_DEVICE_NOT_AVAILABLE (-2)
#define CL_CONTEXT_REFERENCE_COUNT 0x1080
#define CL_CONTEXT_DEVICES 0x1081
#define CL_CONTEXT_PROPERTIES 0x1082
#define CL_CONTEXT_NUM_DEVICES 0x1083

static struct {
    cl_context (*create_from_type)(const cl_context_properties *, cl_device_type, cl_notify_fn, void *, cl_int *);
    cl_int (*get_context_info)(cl_context, cl_uint, size_t, void *, size_t *);
    cl_int (*release_context)(cl_context);
    cl_notify_fn log_to_stdout;
    bool ok;
} cl;

static void cl_load_once(void) {
    void *h = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        LOGE("cl: cannot load OpenCL.framework: %s", dlerror());
        return;
    }
    cl.create_from_type = (__typeof__(cl.create_from_type))dlsym(h, "clCreateContextFromType");
    cl.get_context_info = (__typeof__(cl.get_context_info))dlsym(h, "clGetContextInfo");
    cl.release_context = (__typeof__(cl.release_context))dlsym(h, "clReleaseContext");
    cl.log_to_stdout = (cl_notify_fn)dlsym(h, "clLogMessagesToStdoutAPPLE");
    cl.ok = cl.create_from_type && cl.get_context_info && cl.release_context;
    if (!cl.ok) LOGE("cl: OpenCL.framework lacks required symbols");
}
static bool cl_load(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, cl_load_once);
    return cl.ok;
}

// Guest notification callbacks would run on OpenCL's own threads; messages are
// logged on the host instead of calling back into guest code.
static void cl_log_notify(const char *errinfo, const void *priv, size_t cb, void *user) {
    LOGE("cl: notify: %s", errinfo ? errinfo : "(null)");
}

THUNK(clLogMessagesToStdoutAPPLE) {
    const char *s = ARGS(0);
    printf("%s\n", s ? s : "");
    fflush(stdout);
}

THUNK(clCreateContextFromType) {
    gaddr_t gprops = ARG(0);
    cl_device_type type = ARG64(1);
    gaddr_t pfn = ARG(3);
    gaddr_t errp = ARG(5);
    if (!cl_load()) {
        if (errp) gwrite32(errp, (uint32_t)CL_DEVICE_NOT_AVAILABLE);
        RET(0);
        return;
    }
    // Property list: (name, value) pairs, 0-terminated. Values that are host
    // object handles (e.g. the CGL share group) are unwrapped.
    cl_context_properties props[64];
    int n = 0;
    if (gprops) {
        for (gaddr_t p = gprops; n < 62; p += 8) {
            uint32_t name = gread32(p);
            if (!name) break;
            uint32_t val = gread32(p + 4);
            props[n++] = (cl_context_properties)name;
            props[n++] = hobj_is_handle(val) ? (cl_context_properties)hobj_unwrap(val)
                                             : (cl_context_properties)(int32_t)val;
            LOGI("cl: context property %#x = %#x", name, val);
        }
        props[n] = 0;
    }
    cl_notify_fn notify = NULL;
    if (pfn) notify = pfn == hostsym_lookup("_clLogMessagesToStdoutAPPLE") && cl.log_to_stdout ? cl.log_to_stdout
                                                                                               : cl_log_notify;
    cl_int err = 0;
    cl_context ctx = cl.create_from_type(gprops ? props : NULL, type, notify, NULL, &err);
    LOGI("cl: clCreateContextFromType(type %#llx) -> %p, err %d", (unsigned long long)type, ctx, err);
    if (errp) gwrite32(errp, (uint32_t)err);
    RET(hobj_wrap(ctx));
}

THUNK(clGetContextInfo) {
    cl_context ctx = hobj_unwrap(ARG(0));
    cl_uint pname = ARG(1);
    uint32_t gsize = ARG(2);
    gaddr_t gval = ARG(3), gsize_ret = ARG(4);
    if (!cl_load()) {
        RET(CL_INVALID_VALUE);
        return;
    }
    size_t need = 0;
    cl_int err = cl.get_context_info(ctx, pname, 0, NULL, &need);
    if (err) {
        RET(err);
        return;
    }
    void *buf = calloc(1, need ? need : 1);
    err = cl.get_context_info(ctx, pname, need, buf, NULL);
    if (err) {
        free(buf);
        RET(err);
        return;
    }
    uint32_t out_size;
    if (pname == CL_CONTEXT_DEVICES || pname == CL_CONTEXT_PROPERTIES) {
        // Arrays of pointer-sized values -> 32-bit guest values.
        size_t cnt = need / sizeof(void *);
        out_size = (uint32_t)(cnt * 4);
        if (gval) {
            if (gsize < out_size) {
                free(buf);
                RET(CL_INVALID_VALUE);
                return;
            }
            const intptr_t *v = buf;
            for (size_t i = 0; i < cnt; i++) {
                uint32_t gv;
                bool is_obj = pname == CL_CONTEXT_DEVICES || (i & 1); // property values may be objects
                if (pname == CL_CONTEXT_PROPERTIES && (i & 1) && (uintptr_t)v[i] < 0x100000000ULL)
                    is_obj = false;
                gv = is_obj ? hobj_wrap((const void *)v[i]) : (uint32_t)v[i];
                gwrite32(gval + 4 * (uint32_t)i, gv);
            }
        }
    } else {
        out_size = (uint32_t)need; // cl_uint scalars
        if (gval) {
            if (gsize < out_size) {
                free(buf);
                RET(CL_INVALID_VALUE);
                return;
            }
            memcpy(g2h(gval), buf, need);
        }
    }
    if (gsize_ret) gwrite32(gsize_ret, out_size);
    free(buf);
    RET(0);
}

THUNK(clReleaseContext) {
    if (!cl_load()) {
        RET(CL_INVALID_VALUE);
        return;
    }
    RET(cl.release_context(hobj_unwrap(ARG(0))));
}
