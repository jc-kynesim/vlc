#include <stdint.h>

#ifdef MAKE_TEST
#define vlc_CPU_ARM64_NEON() (1)
#define HAVE_AARCH64_ASM 1
#else
#include <vlc_common.h>
#include <vlc_cpu.h>
#endif

#include "rgba_premul.h"

#ifdef HAVE_AARCH64_ASM
#include "rgba_premul_aarch64.h"
#endif

// x, y src offset, not dest
// This won't be bit exact with aarch64 asm which has slightly different
// rounding (this is faster when done in C)
static void
copy_xxxa_with_premul_c(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
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
#ifdef HAVE_AARCH64_ASM
    if (vlc_CPU_ARM64_NEON())
        copy_xxxa_with_premul_aarch64(dst_data, dst_stride, src_data, src_stride, w, h, global_alpha);
    else
#endif
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

//============================================================================
#ifdef MAKE_TEST
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
utime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_nsec / 1000 + (uint64_t)ts.tv_sec * 1000000;
}

// What the ASM is meant to do exactly
static void
copy_xxxa_with_premul_c_asm(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
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
            // What the ASM is meant to do exactly
            unsigned int a = global_alpha * 257;
            const unsigned int k = 0x800000;
            dst[0] = (((src[0] * src[3] * 257) >> 8) * a + k) >> 24;
            dst[1] = (((src[1] * src[3] * 257) >> 8) * a + k) >> 24;
            dst[2] = (((src[2] * src[3] * 257) >> 8) * a + k) >> 24;
            dst[3] = (src[3] * global_alpha * 257 + 0x8000) >> 16;
        }
        src += src_inc;
        dst += dst_inc;
    }
}

static void
timetest(const unsigned int w, const unsigned int h, const int s, bool use_c)
{
    uint64_t now;
    uint64_t done;

    uint8_t * src = malloc(h * s);
    uint8_t * dst = malloc(h * s);

    memset(src, 0x80, h * s);
    memset(dst, 0xff, h * s);

    now = utime();
    for (unsigned int i = 0; i != 10; ++i)
    {
        if (use_c)
            copy_xxxa_with_premul_c(dst, s, src, s, w, h, 0xba);
        else
            copy_xxxa_with_premul(dst, s, src, s, w, h, 0xba);
    }
    done = utime();

    printf("Time %3s: %dx%d stride %d: %6dus\n", use_c ? "C" : "Asm", w, h, s,
           (int)((done - now)/10));
}

static int
docheck(const uint8_t * const a, const uint8_t * const b, const size_t n)
{
    int t = 0;
    for (size_t i = 0; i != n && t < 128; ++i)
    {
        if (a[i] != b[i])
        {
            printf("@ %zd: %02x %02x\n", i, a[i], b[i]);
            ++t;
        }
    }
    return t;
}

static void
checktest(const unsigned int w, const unsigned int h, const int s, const int offset)
{
    size_t dsize = ((h + 3) * s + 63) & ~63;

    uint8_t * src = malloc(h * s);
    uint8_t * dst = malloc(dsize);
    uint8_t * dst2 = malloc(dsize);
    uint8_t * d  = (uint8_t*)(((uintptr_t)dst + s + 63) & ~63);
    uint8_t * d2 = (uint8_t*)(((uintptr_t)dst2 + s + 63) & ~63);

    for (unsigned int i = 0; i != h * s; ++i)
        src[i] = rand();

    memset(dst2, 0xff, dsize);
    memset(dst,  0xff, dsize);

    copy_xxxa_with_premul_c_asm(d + offset, s, src, s, w, h, 0xba);
    copy_xxxa_with_premul(d2 + offset, s, src, s, w, h, 0xba);

    printf("Check: %dx%d stride %d offset %d: %s\n", w, h, s, offset,
           docheck(d - s, d2 - s, (h + 2) * s) ? "FAIL" : "OK");
}



int
main (int argc, char *argv[])
{
    timetest(1920, 1080, 1920 * 4, true);
    timetest(1920, 1080, 1920 * 4, false);
    timetest(1917, 1080, 1920 * 4, false);
    timetest(1917, 1080, 1917 * 4, false);
    timetest(1920 * 1080, 1, 1920 * 1080 * 4, false);

    checktest(1920, 1080, 1920 * 4, 0);

    for (unsigned int i = 1; i != 64; ++i)
    {
        checktest(i, 16, 64 * 4, 0);
    }
    for (unsigned int i = 0; i != 16; ++i)
    {
        checktest(15, 16, 64 * 4, i * 4);
        checktest(32, 16, 64 * 4, i * 4);
        checktest(43, 16, 64 * 4, i * 4);
    }



    return 0;
}


#endif
