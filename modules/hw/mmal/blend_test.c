#include <stdio.h>
#include <stdint.h>
#include <memory.h>

#include "blend_rgba_neon.h"

#define RPI_PROFILE 1
#define RPI_PROC_ALLOC 1
#include "rpi_prof.h"

static inline unsigned div255(unsigned v)
{
    // This models what we we do in the asm for / 255
    // It generates something in the range [(i+126)/255, (i+127)/255] which is good enough
    return ((v * 257) + 0x8000) >> 16;
}

static inline unsigned int a_merge(unsigned int dst, unsigned src, unsigned f)
{
    return div255((255 - f) * (dst) + src * f);
}


static void merge_line(void * dest, const void * src, int alpha, unsigned int n)
{
    unsigned int i;
    const uint8_t * s_data = src;
    uint8_t * d_data = dest;

    for (i = 0; i != n; ++i) {
        const uint32_t s_pel = ((const uint32_t *)s_data)[i];
        const uint32_t d_pel = ((const uint32_t *)d_data)[i];
        const unsigned int a = div255(alpha * (s_pel >> 24));
        ((uint32_t *)d_data)[i] = 0xff000000 |
            (a_merge((d_pel >> 16) & 0xff, (s_pel >> 16) & 0xff, a) << 16) |
            (a_merge((d_pel >> 8)  & 0xff, (s_pel >> 8)  & 0xff, a) << 8 ) |
            (a_merge((d_pel >> 0)  & 0xff, (s_pel >> 0)  & 0xff, a) << 0 );
    }
}


// Merge RGBA with BGRA
static void merge_line2(void * dest, const void * src, int alpha, unsigned int n)
{
    unsigned int i;
    const uint8_t * s_data = src;
    uint8_t * d_data = dest;

    for (i = 0; i != n; ++i) {
        const uint32_t s_pel = ((const uint32_t *)s_data)[i];
        const uint32_t d_pel = ((const uint32_t *)d_data)[i];
        const unsigned int a = div255(alpha * (s_pel >> 24));
        ((uint32_t *)d_data)[i] = 0xff000000 |
            (a_merge((d_pel >> 0)  & 0xff, (s_pel >> 16) & 0xff, a) << 0 ) |
            (a_merge((d_pel >> 8)  & 0xff, (s_pel >> 8)  & 0xff, a) << 8 ) |
            (a_merge((d_pel >> 16) & 0xff, (s_pel >> 0)  & 0xff, a) << 16);
    }
}

#define BUF_SIZE   256
#define BUF_SLACK  16
#define BUF_ALIGN  64
#define BUF_ALLOC  (BUF_SIZE + 2*BUF_SLACK + BUF_ALIGN)

static void test_line(const uint32_t * const dx, const unsigned int d_off,
                      const uint32_t * const sx, const unsigned int s_off,
                      const unsigned int alpha, const unsigned int len, const int prof_no)
{
    uint32_t d0_buf[BUF_ALLOC];
    uint32_t d1_buf[BUF_ALLOC];
    const uint32_t * const s0 = sx + s_off;

    uint32_t * const d0 =  (uint32_t *)(((uintptr_t)d0_buf + (BUF_ALIGN - 1)) & ~(BUF_ALIGN - 1)) + d_off;
    uint32_t * const d1 = (uint32_t *)(((uintptr_t)d1_buf + (BUF_ALIGN - 1)) & ~(BUF_ALIGN - 1)) + d_off;
    unsigned int i;

    memcpy(d0, dx, (BUF_SIZE + BUF_SLACK*2)*4);
    memcpy(d1, dx, (BUF_SIZE + BUF_SLACK*2)*4);

    merge_line(d0 + BUF_SLACK, s0 + BUF_SLACK, alpha, len);

    PROFILE_START();
    blend_rgbx_rgba_neon(d1 + BUF_SLACK, s0 + BUF_SLACK, alpha, len);
    PROFILE_ACC_N(prof_no);

    for (i = 0; i != BUF_SIZE + BUF_SLACK*2; ++i) {
        if (d0[i] != d1[i]) {
            printf("%3d: %08x + %08x * %02x: %08x / %08x: len=%d\n", (int)(i - BUF_SLACK), dx[i], s0[i], alpha, d0[i], d1[i], len);
        }
    }
}

static void test_line2(const uint32_t * const dx, const unsigned int d_off,
                      const uint32_t * const sx, const unsigned int s_off,
                      const unsigned int alpha, const unsigned int len, const int prof_no)
{
    uint32_t d0_buf[BUF_ALLOC];
    uint32_t d1_buf[BUF_ALLOC];
    const uint32_t * const s0 = sx + s_off;

    uint32_t * const d0 =  (uint32_t *)(((uintptr_t)d0_buf + (BUF_ALIGN - 1)) & ~(BUF_ALIGN - 1)) + d_off;
    uint32_t * const d1 = (uint32_t *)(((uintptr_t)d1_buf + (BUF_ALIGN - 1)) & ~(BUF_ALIGN - 1)) + d_off;
    unsigned int i;

    memcpy(d0, dx, (BUF_SIZE + BUF_SLACK*2)*4);
    memcpy(d1, dx, (BUF_SIZE + BUF_SLACK*2)*4);

    merge_line2(d0 + BUF_SLACK, s0 + BUF_SLACK, alpha, len);

    PROFILE_START();
    blend_bgrx_rgba_neon(d1 + BUF_SLACK, s0 + BUF_SLACK, alpha, len);
    PROFILE_ACC_N(prof_no);

    for (i = 0; i != BUF_SIZE + BUF_SLACK*2; ++i) {
        if (d0[i] != d1[i]) {
            printf("%3d: %08x + %08x * %02x: %08x / %08x: len=%d\n", (int)(i - BUF_SLACK), dx[i], s0[i], alpha, d0[i], d1[i], len);
        }
    }
}



int main(int argc, char *argv[])
{
    unsigned int i, j;
    uint32_t d0_buf[BUF_ALLOC];
    uint32_t s0_buf[BUF_ALLOC];

    uint32_t * const d0 = (uint32_t *)(((uintptr_t)d0_buf + 63) & ~63) + 0;
    uint32_t * const s0 = (uint32_t *)(((uintptr_t)s0_buf + 63) & ~63) + 0;

    PROFILE_INIT();

    for (i = 0; i != 255*255; ++i) {
        unsigned int a = div255(i);
        unsigned int b = (i + 127)/255;
        unsigned int c = (i + 126)/255;
        if (a != b && a != c)
            printf("%d/255: %d != %d/%d\n", i, a, b, c);
    }

    for (i = 0; i != BUF_ALLOC; ++i) {
        d0_buf[i] = 0xff00 | i;
        s0_buf[i] = (i << 24) | 0x40ffc0;
    }

    for (i = 0; i != 256; ++i) {
        test_line(d0, 0, s0, 0, i, 256, -1);
    }
    for (i = 0; i != 256; ++i) {
        test_line(d0, 0, s0, 0, 128, i, -1);
    }

    for (j = 0; j != 16; ++j) {
        for (i = 0; i != 256; ++i) {
            test_line(d0, j & 3, s0, j >> 2, i, 256, j);
        }
        PROFILE_PRINTF_N(j);
        PROFILE_CLEAR_N(j);
    }
    printf("Done 1\n");

    for (i = 0; i != 256; ++i) {
        test_line2(d0, 0, s0, 0, i, 256, -1);
    }
    for (i = 0; i != 256; ++i) {
        test_line2(d0, 0, s0, 0, 128, i, -1);
    }

    for (j = 0; j != 16; ++j) {
        for (i = 0; i != 256; ++i) {
            test_line2(d0, j & 3, s0, j >> 2, i, 256, j);
        }
        PROFILE_PRINTF_N(j);
    }
    printf("Done 2\n");

    return 0;
}

