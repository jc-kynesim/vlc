#ifndef HW_MMAL_BLEND_RGBA_NEON_H
#define HW_MMAL_BLEND_RGBA_NEON_H

#ifdef __cplusplus
extern "C" {
#endif

extern void blend_rgbx_rgba_neon(void * dest, const void * src, int alpha, unsigned int n);
extern void blend_bgrx_rgba_neon(void * dest, const void * src, int alpha, unsigned int n);

#ifdef __cplusplus
}
#endif

#endif

