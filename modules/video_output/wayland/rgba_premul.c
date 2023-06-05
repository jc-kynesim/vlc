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

static bool verbose = false;
static bool checkfail = false;

static uint64_t
utime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_nsec / 1000 + (uint64_t)ts.tv_sec * 1000000;
}

// What the ASM is meant to do exactly
static void
copy_xxxa_with_premul_c_asm(void * restrict dst_data, int dst_stride,
                      const void * restrict src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const uint8_t global_alpha)
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

#define ALIGN_SIZE 128
#define ALIGN_PTR(p) ((uint8_t*)(((uintptr_t)p + (ALIGN_SIZE -1)) & ~(ALIGN_SIZE - 1)))

static void
timetest(const unsigned int w, const unsigned int h, const int stride, bool use_c)
{
    uint64_t now;
    uint64_t done;
    size_t dsize = h * stride + ALIGN_SIZE;

    uint8_t * src = malloc(dsize);
    uint8_t * dst = malloc(dsize);
    uint8_t * s  = ALIGN_PTR(src);
    uint8_t * d  = ALIGN_PTR(dst);

    memset(src, 0x80, dsize);
    memset(dst, 0xff, dsize);

    now = utime();
    for (unsigned int i = 0; i != 10; ++i)
    {
        if (use_c)
            copy_xxxa_with_premul_c(d, stride, s, stride, w, h, 0xba);
        else
            copy_xxxa_with_premul(d, stride, s, stride, w, h, 0xba);
    }
    done = utime();

    printf("Time %3s: %dx%d stride %d: %6dus\n", use_c ? "C" : "Asm", w, h, stride,
           (int)((done - now)/10));

    free(src);
    free(dst);
}

static int
docheck(const uint8_t * const a, const uint8_t * const b, const size_t n)
{
    int t = 0;

    if (!verbose)
        return memcmp(a, b, n);

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
checktest(const unsigned int w, const unsigned int h, const int stride, const int offset)
{
    size_t dsize = ((h + 3) * stride + ALIGN_SIZE);

    uint8_t * src = malloc(dsize);
    uint8_t * dst = malloc(dsize);
    uint8_t * dst2 = malloc(dsize);
    uint8_t * s  = ALIGN_PTR(src + stride);
    uint8_t * d  = ALIGN_PTR(dst + stride);
    uint8_t * d2 = ALIGN_PTR(dst2 + stride);

    for (unsigned int i = 0; i != dsize; ++i)
        src[i] = rand();

    memset(dst2, 0xff, dsize);
    memset(dst,  0xff, dsize);

    copy_xxxa_with_premul_c_asm(d + offset, stride, s, stride, w, h, 0xba);
    copy_xxxa_with_premul(d2 + offset, stride, s, stride, w, h, 0xba);

    if (docheck(d - stride, d2 - stride, (h + 2) * stride) != 0)
    {
        printf("Check: %dx%d stride %d offset %d: FAIL\n", w, h, stride, offset);
        checkfail = true;
    }
    else if (verbose)
    {
        printf("Check: %dx%d stride %d offset %d: ok\n", w, h, stride, offset);
    }

    free(src);
    free(dst);
    free(dst2);
}



int
main (int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "-v") == 0)
        verbose = true;

    timetest(1920, 1080, 1920 * 4, true);
    timetest(1920, 1080, 1920 * 4, false);
    timetest(1917, 1080, 1920 * 4, false);
    timetest(1917, 1080, 1917 * 4, false);
    timetest(1920 * 1080, 1, 1920 * 1080 * 4, false);

    checktest(1920, 1080, 1920 * 4, 0);

    // Stride of 65pel will rotate alignment vertically
    for (unsigned int i = 1; i != 64; ++i)
    {
        checktest(i, 32, 65 * 4, 0);
    }

    if (!checkfail)
    {
        printf("All chacks passed\n");
    }

    return checkfail;
}


#endif
