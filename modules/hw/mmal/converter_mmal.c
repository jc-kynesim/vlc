#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <interface/vcsm/user-vcsm.h>


#include <vlc_common.h>
#include <vlc_picture.h>

#include "mmal_gl.h"

#include <vlc_xlib.h>
#include "../../video_output/opengl/converter.h"
#include <vlc_vout_window.h>

#include "mmal_picture.h"

#include <assert.h>


typedef struct mmal_gl_converter_s
{
    EGLint drm_fourcc;

    Display * dpy;
    EGLDisplay * egl_dpy;
    int drm_fd;

} mmal_gl_converter_t;


static int
tc_mmal_update(const opengl_tex_converter_t *tc, GLuint *textures,
                const GLsizei *tex_width, const GLsizei *tex_height,
                picture_t *pic, const size_t *plane_offset)
{
    msg_Err(tc, "%s", __func__);
    VLC_UNUSED(textures);
    VLC_UNUSED(tex_width);
    VLC_UNUSED(tex_height);
    VLC_UNUSED(pic);
    VLC_UNUSED(plane_offset);

    return VLC_EGENERIC;
}

#if 0
static int buffer_create(struct buffer *b, int drmfd, MMAL_PORT_T *port,
                         EGLDisplay dpy, EGLContext ctx)
{
   struct drm_mode_create_dumb gem;
   struct drm_mode_destroy_dumb gem_destroy;
   int ret;

   memset(&gem, 0, sizeof gem);
   gem.width = port->format->es->video.width;
   gem.height = port->format->es->video.height;
   gem.bpp = 32;
   gem.size = port->buffer_size;
   ret = ioctl(drmfd, DRM_IOCTL_MODE_CREATE_DUMB, &gem);
   if (ret)
   {
      printf("CREATE_DUMB failed: %s\n", ERRSTR);
      return -1;
   }
   printf("bo %u %ux%u bpp %u size %lu (%u)\n", gem.handle, gem.width, gem.height, gem.bpp, (long)gem.size, port->buffer_size);
   b->bo_handle = gem.handle;

   struct drm_prime_handle prime;
   memset(&prime, 0, sizeof prime);
   prime.handle = b->bo_handle;

   ret = ioctl(drmfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
   if (ret)
   {
      printf("PRIME_HANDLE_TO_FD failed: %s\n", ERRSTR);
      goto fail_gem;
   }
   printf("dbuf_fd = %d\n", prime.fd);
   b->dbuf_fd = prime.fd;

   uint32_t pitches[4] = { 0 };
   uint32_t offsets[4] = { 0 };
   uint32_t bo_handles[4] = { b->bo_handle };
   unsigned int fourcc = mmal_encoding_to_drm_fourcc(port->format->encoding);

   mmal_format_to_drm_pitches_offsets(pitches, offsets, bo_handles, port->format);


   fprintf(stderr, "FB fourcc %c%c%c%c\n",
      fourcc,
      fourcc >> 8,
      fourcc >> 16,
      fourcc >> 24);

   b->vcsm_handle = vcsm_import_dmabuf(b->dbuf_fd, "DRM Buf");
   if (!b->vcsm_handle)
      goto fail_prime;

   EGLint attribs[50];
   int i = 0;

   attribs[i++] = EGL_WIDTH;
   attribs[i++] = port->format->es->video.crop.width;
   attribs[i++] = EGL_HEIGHT;
   attribs[i++] = port->format->es->video.crop.height;

   attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
   attribs[i++] = fourcc;

   attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
   attribs[i++] = b->dbuf_fd;

   attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
   attribs[i++] = offsets[0];

   attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
   attribs[i++] = pitches[0];

   if (pitches[1]) {
      attribs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
      attribs[i++] = b->dbuf_fd;

      attribs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
      attribs[i++] = offsets[1];

      attribs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
      attribs[i++] = pitches[1];
   }

   if (pitches[2]) {
      attribs[i++] = EGL_DMA_BUF_PLANE2_FD_EXT;
      attribs[i++] = b->dbuf_fd;

      attribs[i++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
      attribs[i++] = offsets[2];

      attribs[i++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
      attribs[i++] = pitches[2];
   }

   attribs[i++] = EGL_NONE;

   EGLImage image = eglCreateImageKHR(dpy,
                                      EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT,
                                      NULL, attribs);
   if (!image) {
      fprintf(stderr, "Failed to import fd %d: Err=%#x\n", b->dbuf_fd, eglGetError());
      exit(1);
   }

   glGenTextures(1, &b->texture);
   glBindTexture(GL_TEXTURE_EXTERNAL_OES, b->texture);
   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

   eglDestroyImageKHR(dpy, image);

   return 0;

fail_prime:
   close(b->dbuf_fd);

fail_gem:
   memset(&gem_destroy, 0, sizeof gem_destroy);
   gem_destroy.handle = b->bo_handle,
   ret = ioctl(drmfd, DRM_IOCTL_MODE_DESTROY_DUMB, &gem_destroy);
   if (ret)
   {
      printf("DESTROY_DUMB failed: %s\n", ERRSTR);
   }

   return -1;
}
#endif

static void
tc_free_buf(cma_buf_t * const cmabuf)
{
    if (cmabuf == NULL)
        return;

    if (cmabuf->texture != 0)
        glDeleteTextures(1, &cmabuf->texture);

    if (cmabuf->mapped_addr != NULL && cmabuf->mapped_addr != MAP_FAILED)
        munmap(cmabuf->mapped_addr, cmabuf->size);

    if (cmabuf->h_vcsm != 0)
        vcsm_free(cmabuf->h_vcsm);

    if (cmabuf->fd != -1)
        close(cmabuf->fd);

    {
        struct drm_mode_destroy_dumb gem_destroy = {
            .handle = cmabuf->h_dumb
        };
        ioctl(cmabuf->sys->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &gem_destroy);
    }
}

static cma_buf_t *
tc_alloc_buf(const opengl_tex_converter_t * const tc, mmal_gl_converter_t * const sys, const size_t size)
{
    cma_buf_t * const cmabuf = malloc(sizeof(cma_buf_t));

    if (cmabuf == NULL)
    {
        msg_Err(tc, "%s: malloc fail", __func__);
        return NULL;
    }

    // Zap
    *cmabuf = (cma_buf_t){
        .size = size,
        .h_dumb = 0,
        .fd = -1,
        .h_vcsm = 0,
        .mapped_addr = NULL,
        .texture = 0
    };

    {
        struct drm_mode_create_dumb gem = {
            .width = (uint32_t)(cmabuf->size),          // Width / height aren't actually used for anything so cheat
            .height = 1,
            .bpp = 8
            // other fields zero
        };

        if (ioctl(sys->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem) != 0)
        {
            msg_Err(tc, "CREATE_DUMB failed (fd=%d, size=%d): errno=%d", sys->drm_fd, size, errno);
            goto fail;
        }
        cmabuf->h_dumb = gem.handle;
    }

    {
        struct drm_prime_handle prime = {
            .handle = cmabuf->h_dumb,
            .flags  = DRM_CLOEXEC | DRM_RDWR,
            .fd     = -1
        };
        if (ioctl(sys->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0)
        {
            msg_Err(tc, "DRM_IOCTL_PRIME_HANDLE failed: errno=%d", errno);
            goto fail;
        }
        cmabuf->fd = prime.fd;
    }

    if ((cmabuf->mapped_addr = mmap(NULL, cmabuf->size, PROT_READ | PROT_WRITE, MAP_SHARED, cmabuf->fd, 0)) == MAP_FAILED)
    {
        msg_Err(tc, "%s: mapping failed: size=%zu, fd=%d, errno=%d", __func__, cmabuf->size, cmabuf->fd, errno);
        goto fail;
    }

    if ((cmabuf->h_vcsm = vcsm_import_dmabuf(cmabuf->fd, (char*)"VLC DRM Buf")) == 0)
    {
        msg_Err(tc, "vcsm_import_dmabuf failed");
        goto fail;
    }

    return cmabuf;

fail:
    tc_free_buf(cmabuf);
    return NULL;
}

static EGLint vlc_to_gl_fourcc(const video_format_t * const fmt)
{
    switch(vlc_to_mmal_video_fourcc(fmt))
    {
       case MMAL_ENCODING_I420:
          return MMAL_FOURCC('Y','U','1','2');
       case MMAL_ENCODING_YV12:
          return MMAL_FOURCC('Y','V','1','2');
       case MMAL_ENCODING_I422:
          return MMAL_FOURCC('Y','U','1','6');
       case MMAL_ENCODING_NV12:
          return MMAL_FOURCC('N','V','1','2');
       case MMAL_ENCODING_NV21:
          return MMAL_FOURCC('N','V','2','1');
       case MMAL_ENCODING_RGB16:
          return MMAL_FOURCC('R','G','1','6');
       case MMAL_ENCODING_RGB24:
          return MMAL_FOURCC('B','G','2','4');
       case MMAL_ENCODING_BGR24:
          return MMAL_FOURCC('R','G','2','4');
       case MMAL_ENCODING_BGR32:
       case MMAL_ENCODING_BGRA:
          return MMAL_FOURCC('X','R','2','4');
       case MMAL_ENCODING_RGB32:
       case MMAL_ENCODING_RGBA:
          return MMAL_FOURCC('X','B','2','4');
       default:
          break;
    }
    return 0;
}

static inline unsigned int
round_width(unsigned int w)
{
    return ((w + 31) & ~31);
}

static inline unsigned int
round_height(unsigned int h)
{
    return ((h + 15) & ~15);
}


// Get total size & fill in resource plane info.  Addresses are 0 based as we don't have a map yet
static size_t
get_resource_size(const opengl_tex_converter_t *tc, picture_resource_t * const res, const video_format_t * const fmt)
{
    VLC_UNUSED(tc);  // For debug

    const vlc_chroma_description_t * const p_dsc =
        vlc_fourcc_GetChromaDescription(fmt->i_chroma);

    if (p_dsc == NULL)
        return 0;

    assert(p_dsc->plane_count <= PICTURE_PLANE_MAX);

    const unsigned int stride0 = round_width(fmt->i_width) * p_dsc->pixel_size;
    const unsigned int height0 = round_height(fmt->i_height);
    const unsigned int size0 = stride0 * height0;

    size_t size_total = 0;
    for (unsigned int i = 0; i < p_dsc->plane_count; ++i)
    {
        res->p[i].i_pitch = (stride0 * p_dsc->p[i].w.num) / p_dsc->p[i].w.den;
        res->p[i].i_lines = (height0 * p_dsc->p[i].h.num) / p_dsc->p[i].h.den;
        res->p[i].p_pixels = (uint8_t *)0 + size_total;
        size_total += (size0 * p_dsc->p[i].h.num * p_dsc->p[i].w.num) / (p_dsc->p[i].h.den * p_dsc->p[i].w.den);
    }
    // For tidyness zap the rest of the array
    for (unsigned int i = p_dsc->plane_count; i < PICTURE_PLANE_MAX; ++i)
    {
        res->p[i].i_lines = 0;
        res->p[i].i_pitch = 0;
        res->p[i].p_pixels = NULL;
    }

    return size_total;
}

static void
fixup_resource_addresses(picture_resource_t *const res, const cma_buf_t * const cmabuf)
{
    uint8_t * const base = res->p[0].p_pixels;
    for (unsigned int i = 0; i < PICTURE_PLANE_MAX && res->p[i].i_lines != 0; ++i)
        res->p[i].p_pixels = (res->p[i].p_pixels - base) + (uint8_t *)(cmabuf->mapped_addr);
}

static void
pic_sys_free(cma_pic_sys_t * const pic_sys)
{
    if (pic_sys == NULL)
        return;

    tc_free_buf(pic_sys->cmabuf);
    free(pic_sys);
}

static void
pic_destroy_cb(picture_t * pic)
{
    pic_sys_free((cma_pic_sys_t *)pic->p_sys);
    free(pic);
}

static picture_t *
tc_mmal_get_pic(const opengl_tex_converter_t *tc, mmal_gl_converter_t * const sys)
{
    picture_resource_t pic_res;

    char cbuf0[5];
    msg_Err(tc, "%s: (%s, %dx%d)", __func__, str_fourcc(cbuf0, tc->fmt.i_chroma), tc->fmt.i_width, tc->fmt.i_height);

    cma_pic_sys_t * pic_sys = calloc(1, sizeof(cma_pic_sys_t));
    if (pic_sys == NULL)
        return NULL;

    pic_res.p_sys = (picture_sys_t *)pic_sys;
    pic_res.pf_destroy = pic_destroy_cb;

    const size_t size_total = get_resource_size(tc, &pic_res, &tc->fmt);
    if (size_total == 0)
    {
        msg_Err(tc, "%s: size == 0", __func__);
        goto fail;
    }

    if ((pic_sys->cmabuf = tc_alloc_buf(tc, sys, size_total)) == NULL)
        goto fail;

    fixup_resource_addresses(&pic_res, pic_sys->cmabuf);

    picture_t * pic = picture_NewFromResource(&tc->fmt, &pic_res);
    if (pic == NULL)
    {
        msg_Err(tc, "%s: picture_New failed", __func__);
        goto fail;
    }

    msg_Dbg(tc, "%s: pic_fmt:%dx%d, total_size=%zd, res:%dx%d pic:%s:%dx%d", __func__,
            pic->format.i_width, pic->format.i_height,
            size_total,
            pic_res.p[0].i_pitch, pic_res.p[0].i_lines,
            str_fourcc(cbuf0, pic->format.i_chroma),
            pic->p[0].i_pitch, pic->p[0].i_lines);

    return pic;

fail:
    pic_sys_free(pic_sys);
    return NULL;
}

static picture_pool_t *
tc_mmal_get_pool(const opengl_tex_converter_t *tc, unsigned requested_count)
{
    mmal_gl_converter_t * const sys = tc->priv;
    const unsigned int pic_count = requested_count < 3 ? 3 : requested_count > 32 ? 32 : requested_count;
    picture_t * pics[33] = {NULL};

    for (unsigned int i = 0; i < pic_count; ++i)
    {
        if ((pics[i] = tc_mmal_get_pic(tc, sys)) == NULL)
            goto fail;
    }

    picture_pool_configuration_t pic_config = {
        .picture_count = pic_count,
        .picture = pics
    };

    picture_pool_t * pic_pool = picture_pool_NewExtended(&pic_config);
    if (pic_pool == NULL)
    {
        msg_Err(tc, "%s: picture_pool_NewExtended fail", __func__);
        goto fail;
    }
    return pic_pool;

fail:
    for (picture_t **pic = pics; *pic != NULL; ++pic)
        picture_Release(*pic);

    return NULL;
}


static int
get_drm_fd(opengl_tex_converter_t * const tc, Display * const dpy)
{
   xcb_connection_t * const c = XGetXCBConnection(dpy);
   const xcb_window_t root = RootWindow(dpy, DefaultScreen(dpy));
   int fd;

   const xcb_query_extension_reply_t *extension =
      xcb_get_extension_data(c, &xcb_dri3_id);
   if (!(extension && extension->present))
      return -1;

   xcb_dri3_open_cookie_t cookie =
      xcb_dri3_open(c, root, None);

   xcb_generic_error_t * err = NULL;
   xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(c, cookie, &err);
   if (!reply)
      return -1;

   if (err != NULL)
   {
       msg_Err(tc, "X dri3 open fail: rtype=%d, err=%d", (int)err->response_type, (int)err->error_code);
       free(err);
       return -1;
   }


   if (reply->nfd != 1) {
      free(reply);
      return -1;
   }

   fd = xcb_dri3_open_reply_fds(c, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

   return fd;
}


static void
CloseGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    mmal_gl_converter_t * const sys = tc->priv;

    if (sys == NULL)
        return;

    if (sys->drm_fd != -1)
        close(sys->drm_fd);

    if (sys->egl_dpy != EGL_NO_DISPLAY)
        eglTerminate(sys->egl_dpy);

    if (sys->dpy)
        XCloseDisplay(sys->dpy);

    free(sys);
}

static int
OpenGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    int rv = VLC_EGENERIC;

    const EGLint eglfmt = vlc_to_gl_fourcc(&tc->fmt);

    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(tc, ">>> %s: V:%s/E:%s,%dx%d [(%d,%d) %d/%d] sar:%d/%d", __func__,
                str_fourcc(dbuf0, tc->fmt.i_chroma),
                str_fourcc(dbuf1, eglfmt),
                tc->fmt.i_width, tc->fmt.i_height,
                tc->fmt.i_x_offset, tc->fmt.i_y_offset,
                tc->fmt.i_visible_width, tc->fmt.i_visible_height,
                tc->fmt.i_sar_num, tc->fmt.i_sar_den);
    }

    if ((tc->priv = calloc(1, sizeof(mmal_gl_converter_t))) == NULL)
    {
        msg_Err(tc, "priv alloc failure");
        rv = VLC_ENOMEM;
        goto fail;
    }
    mmal_gl_converter_t * const sys = tc->priv;

    sys->drm_fourcc = eglfmt;
    sys->egl_dpy = EGL_NO_DISPLAY;
    sys->drm_fd = -1;

    if (!vlc_xlib_init(VLC_OBJECT(tc->gl)))
    {
        msg_Err(tc, "vlc_xlib_init failed");
        goto fail;
    }

    if ((sys->dpy = XOpenDisplay(tc->gl->surface->display.x11)) == NULL)
    {
        msg_Err(tc, "Failed to open X");
        goto fail;
    }

    if ((sys->egl_dpy = eglGetDisplay(sys->dpy)) == EGL_NO_DISPLAY)
    {
        msg_Err(tc, "Failed to get EGL Display");
        goto fail;
    }

    {
        EGLint egl_major, egl_minor;
        if (!eglInitialize(sys->egl_dpy, &egl_major, &egl_minor))
        {
            msg_Err(tc, "eglInitialize() failed");
            goto fail;
        }
    }

    if ((sys->drm_fd = get_drm_fd(tc, sys->dpy)) == -1)
    {
        msg_Err(tc, "Failed to get drm fd");
        goto fail;
    }
    msg_Info(tc, "Got DRM fd=%d", sys->drm_fd);

    // *** May want MMAL_I420 here, but we will need better descriptors
    if ((tc->fshader = opengl_fragment_shader_init(tc, GL_TEXTURE_2D,
                                                   eglfmt == 0 ? VLC_CODEC_RGB32 : tc->fmt.i_chroma,
                                                   eglfmt == 0 ? COLOR_SPACE_SRGB : tc->fmt.space)) == 0)
    {
        msg_Err(tc, "Failed to make shader");
        goto fail;
    }

    if (eglfmt == 0)
    {
        tc->fmt.i_chroma = VLC_CODEC_MMAL_GL_RGB32;
        tc->fmt.i_rmask = 0xff0000;
        tc->fmt.i_gmask = 0xff00;
        tc->fmt.i_bmask = 0xff;
        tc->fmt.space = COLOR_SPACE_SRGB;
//        tc->fmt.i_bits_per_pixel = 8;  // ???
        sys->drm_fourcc = vlc_to_gl_fourcc(&tc->fmt);
    }

    tc->pf_update  = tc_mmal_update;
    tc->pf_get_pool = tc_mmal_get_pool;
    return VLC_SUCCESS;

fail:
    CloseGLConverter(obj);
    return rv;
}

vlc_module_begin ()
    set_description("MMAL OpenGL surface converter")
    set_capability("glconv", 900)
    set_callbacks(OpenGLConverter, CloseGLConverter)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    add_shortcut("mmal_gl_converter")
vlc_module_end ()

