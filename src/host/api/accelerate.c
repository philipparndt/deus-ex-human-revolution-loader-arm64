// Accelerate (vImage). vImage_Buffer i386: {void *data; u32 height, width, rowBytes}.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "common.h"

#include <Accelerate/Accelerate.h>
#include "ret_return.h"

__asm__(".linker_option \"-framework\", \"Accelerate\"");

static void get_vbuf(gaddr_t g, vImage_Buffer *b) {
    b->data = g2h(gread32(g));
    b->height = gread32(g + 4);
    b->width = gread32(g + 8);
    b->rowBytes = gread32(g + 12);
}

// vImageConvolve_ARGB8888(src, dest, tempBuffer, srcOffsetToROI_X, srcOffsetToROI_Y,
//     const int16_t *kernel, uint32_t kh, uint32_t kw, int32_t divisor, const Pixel_8888 bg, vImage_Flags)
THUNK(vImageConvolve_ARGB8888) {
    vImage_Buffer src, dst;
    get_vbuf(ARG(0), &src);
    get_vbuf(ARG(1), &dst);
    const uint8_t *bg = ARGP(9);
    Pixel_8888 bgc = {0, 0, 0, 0};
    if (bg) memcpy(bgc, bg, 4);
    vImage_Error e = vImageConvolve_ARGB8888(&src, &dst, ARGP(2), ARG(3), ARG(4), ARGP(5), ARG(6), ARG(7), ARGI(8),
                                             bgc, ARG(10));
    RET((uint32_t)e);
}
