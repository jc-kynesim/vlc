#ifndef _WAYLAND_RGBA_PREMUL_AARCH64_H
#define _WAYLAND_RGBA_PREMUL_AARCH64_H

#ifdef HAVE_AARCH64_ASM
void
copy_xxxa_with_premul_aarch64(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha);
#endif

#endif

