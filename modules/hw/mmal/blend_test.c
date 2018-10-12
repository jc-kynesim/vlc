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

static void test_line(const uint32_t dx[256], const uint32_t s0[256], unsigned int alpha, unsigned int len)
{
    uint32_t d0[256];
    uint32_t d1[256];
    unsigned int i;

    memcpy(d0, dx, sizeof(d1));
    memcpy(d1, dx, sizeof(d1));

    merge_line(d0, s0, alpha, len);

    blend_rgbx_rgba_neon(d1, s0, alpha, len);

    for (i = 0; i != 256; ++i) {
        if (d0[i] != d1[i]) {
            printf("%3d: %08x + %08x * %02x: %08x / %08x: len=%d\n", i, dx[i], s0[i], alpha, d0[i], d1[i], len);
        }
    }
}

static void test_line0(const uint32_t dx[256], const uint32_t s0[256], unsigned int alpha, unsigned int len)
{
    uint32_t d0[256];
    uint32_t d1[256];
    unsigned int i;

    memcpy(d0, dx, sizeof(d1));
    memcpy(d1, dx, sizeof(d1));

    merge_line(d0, s0, alpha, len);

    PROFILE_START();
    blend_rgbx_rgba_neon(d1, s0, alpha, len);
    PROFILE_ACC(prof0);

    for (i = 0; i != 256; ++i) {
        if (d0[i] != d1[i]) {
            printf("%3d: %08x + %08x * %02x: %08x / %08x: len=%d\n", i, dx[i], s0[i], alpha, d0[i], d1[i], len);
        }
    }
}

static void test_line1(const uint32_t dx[256], const uint32_t s0[256], unsigned int alpha, unsigned int len)
{
    uint32_t d0[256];
    uint32_t d1[256];
    unsigned int i;

    memcpy(d0, dx, sizeof(d1));
    memcpy(d1, dx, sizeof(d1));

    merge_line(d0, s0, alpha, len);
    PROFILE_START();
    blend_rgbx_rgba_neon(d1, s0, alpha, len);
    PROFILE_ACC(prof1);

    for (i = 0; i != 256; ++i) {
        if (d0[i] != d1[i]) {
            printf("%3d: %08x + %08x * %02x: %08x / %08x: len=%d\n", i, dx[i], s0[i], alpha, d0[i], d1[i], len);
        }
    }
}


int main(int argc, char *argv[])
{
    unsigned int i;
    uint32_t d0_buf[512];
    uint32_t s0_buf[512];

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

    for (i = 0; i != 384; ++i) {
        d0[i] = 0xff00 | i;
        s0[i] = (i << 24) | 0xffffff;
    }
    for (i = 0; i != 256; ++i) {
        test_line0(d0, s0, i, 256);
    }
    for (i = 0; i != 256; ++i) {
        test_line1(d0, s0, i, 256);
    }

    for (i = 0; i != 256; ++i) {
        test_line(d0, s0, 128, i);
    }

    PROFILE_PRINTF(prof0);
    PROFILE_PRINTF(prof1);

    printf("Done\n");

    return 0;
}

