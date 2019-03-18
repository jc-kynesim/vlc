// Trim this include list!

#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>

struct mmal_gl_converter_s;

typedef struct cma_buf_s {
    struct mmal_gl_converter_s * sys;

    size_t size;
    __u32 h_dumb;
    int fd;
    unsigned int h_vcsm;
    void * mapped_addr;
    GLuint texture;
} cma_buf_t;

typedef struct cma_pic_sys_s {
    cma_buf_t * cmabuf;
} cma_pic_sys_t;

static inline unsigned int
hw_mmal_h_vcsm(const picture_t * const pic)
{
    const cma_pic_sys_t *const pic_sys = (cma_pic_sys_t *)pic->p_sys;

    if (pic->format.i_chroma != VLC_CODEC_MMAL_GL_RGB32 ||
        pic_sys == NULL || pic_sys->cmabuf == NULL) {
        return 0;
    }

    return pic_sys->cmabuf->h_vcsm;
}

