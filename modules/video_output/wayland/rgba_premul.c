#include <stdint.h>

#include <vlc_common.h>
#include <vlc_cpu.h>

#include "rgba_premul.h"

// x, y src offset, not dest
// This won't be bit exact with aarch64 asm which has slightly different
// rounding (this is faster when done in C)
static void
copy_xxxa_with_premul_c(void * restrict dst_data, int dst_stride,
                      const void * restrict src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha)
{
    uint8_t * dst = (uint8_t*)dst_data;
    const uint8_t * src = (uint8_t*)src_data;
    const int src_inc = src_stride - (int)w * 4;
    const int dst_inc = dst_stride - (int)w * 4;

    for (unsigned int i = 0; i != h; ++i)
    {
        for (unsigned int j = 0; j != w; ++j, src+=4, dst += 4)
        {
            unsigned int a = src[3] * global_alpha * 258;
            const unsigned int k = 0x800000;
            dst[0] = (src[0] * a + k) >> 24;
            dst[1] = (src[1] * a + k) >> 24;
            dst[2] = (src[2] * a + k) >> 24;
            dst[3] = (src[3] * global_alpha * 257 + 0x8000) >> 16;
        }
        src += src_inc;
        dst += dst_inc;
    }
}

void
copy_xxxa_with_premul(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha)
{
    copy_xxxa_with_premul_c(dst_data, dst_stride, src_data, src_stride, w, h, global_alpha);
}

// Has the optimization of copying as a single lump if strides are the same
// and the width is fairly close to the stride
// at the expense of possibly overwriting some bytes outside the active area
// (but within the frame)
void
copy_frame_xxxa_with_premul(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha)
{
    if (dst_stride == src_stride && (dst_stride & 3) == 0 && (int)w * 4 <= dst_stride && (int)w * 4 + 64 >= dst_stride)
        copy_xxxa_with_premul(dst_data, dst_stride, src_data, src_stride, h * dst_stride / 4, 1, global_alpha);
    else
        copy_xxxa_with_premul(dst_data, dst_stride, src_data, src_stride, w, h, global_alpha);
}

