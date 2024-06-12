#ifndef _WAYLAND_RGBA_PREMUL_H
#define _WAYLAND_RGBA_PREMUL_H

void copy_xxxa_with_premul(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha);

// Has the optimization of copying as a single lump if strides are the same
// and the width is fairly close to the stride
// at the expense of possibly overwriting some bytes outside the active area
// (but within the frame)
void copy_frame_xxxa_with_premul(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha);

#endif
