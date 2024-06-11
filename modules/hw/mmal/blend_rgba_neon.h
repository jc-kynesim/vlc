#ifndef HW_MMAL_BLEND_RGBA_NEON_H
#define HW_MMAL_BLEND_RGBA_NEON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void blend_neon_fn(void * dest, const void * src, int alpha, unsigned int n);
extern blend_neon_fn blend_rgbx_rgba_neon;
extern blend_neon_fn blend_bgrx_rgba_neon;

#ifdef __cplusplus
}
#endif

#endif

