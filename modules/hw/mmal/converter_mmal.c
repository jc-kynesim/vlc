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

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>
//#include <xf86drm.h>
//#include <xf86drmMode.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>

#include "mmal_cma.h"

#include <vlc_xlib.h>
#include "../../video_output/opengl/converter.h"
#include <vlc_vout_window.h>

#include "mmal_picture.h"

#include <assert.h>

#define OPT_SAND 0
#define OPT_I420 1
#define OPT_RGB32 0


#if OPT_SAND
#define FMT_IN VLC_CODEC_MMAL_ZC_SAND8
#elif OPT_I420
#define FMT_IN VLC_CODEC_MMAL_ZC_I420
#elif OPT_RGB32
#define FMT_IN VLC_CODEC_MMAL_ZC_RGB32
#elif
#error Missing input format
#endif

#define TRACE_ALL 0

typedef struct mmal_gl_converter_s
{
    EGLint drm_fourcc;
    vcsm_init_type_t vcsm_init_type;
    struct cma_pic_context_s * last_ctx_ref;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
} mmal_gl_converter_t;


static EGLint vlc_to_gl_fourcc(const video_format_t * const fmt)
{
    // Converting to mmal selects the right RGB32 varient
    switch(vlc_to_mmal_video_fourcc(fmt))
    {
       case MMAL_ENCODING_I420:
          return MMAL_FOURCC('Y','U','1','2');
       case MMAL_ENCODING_YV12:
          return MMAL_FOURCC('Y','V','1','2');
       case MMAL_ENCODING_I422:
          return MMAL_FOURCC('Y','U','1','6');
       case MMAL_ENCODING_YUVUV128:
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

typedef struct tex_context_s {
    picture_context_t cmn;
    GLuint texture;

    PFNGLDELETETEXTURESPROC DeleteTextures;  // Copy fn pointer so we don't need tc on delete
} tex_context_t;

static void tex_context_delete(tex_context_t * const tex)
{
    tex->DeleteTextures(1, &tex->texture);

    free(tex);
}

static void tex_context_destroy(picture_context_t * pic_ctx)
{
    tex_context_delete((tex_context_t *)pic_ctx);
}

static picture_context_t * tex_context_copy(picture_context_t * pic_ctx)
{
    return pic_ctx;
}

static tex_context_t * get_tex_context(const opengl_tex_converter_t * const tc, picture_t * const pic)
{
    mmal_gl_converter_t * const sys = tc->priv;

    tex_context_t * tex = (tex_context_t *)cma_buf_pic_context2(pic);
    if (tex != NULL)
        return tex;

    if ((tex = malloc(sizeof(*tex))) == NULL)
        return NULL;

    *tex = (tex_context_t){
        .cmn = {
            .destroy = tex_context_destroy,
            .copy = tex_context_copy
        },
        .texture = 0,
        .DeleteTextures = tc->vt->DeleteTextures
    };

    {
        EGLint attribs[30];
        EGLint * a = attribs;
        const int fd = cma_buf_pic_fd(pic);
        uint8_t * base_addr = cma_buf_pic_addr(pic);

        if (pic->i_planes >= 4 || pic->i_planes <= 0)
        {
            msg_Err(tc, "%s: Bad planes", __func__);
            goto fail;
        }

        *a++ = EGL_WIDTH;
        *a++ = pic->format.i_visible_width;
        *a++ = EGL_HEIGHT;
        *a++ = pic->format.i_visible_height;
        *a++ = EGL_LINUX_DRM_FOURCC_EXT;
        *a++ = sys->drm_fourcc;

        if (pic->format.i_chroma == VLC_CODEC_MMAL_ZC_SAND8)
        {
            // Sand is its own very special bunny :-(
            static const EGLint attnames[] = {
                EGL_DMA_BUF_PLANE0_FD_EXT,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                EGL_DMA_BUF_PLANE0_PITCH_EXT,
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                EGL_DMA_BUF_PLANE1_FD_EXT,
                EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                EGL_DMA_BUF_PLANE1_PITCH_EXT,
                EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT
            };

            const EGLint * n = attnames;

            for (int i = 0; i < pic->i_planes; ++i)
            {
                const uint64_t mod = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(pic->p[i].i_pitch >> 7);

                *a++ = *n++;
                *a++ = fd;
                *a++ = *n++;
                *a++ = pic->p[i].p_pixels - base_addr;
                *a++ = *n++;
                *a++ = pic->format.i_width;
                *a++ = *n++;
                *a++ = (EGLint)(mod >> 32);
                *a++ = *n++;
                *a++ = (EGLint)(mod & 0xffffffff);
            }
        }
        else
        {
            static const EGLint attnames[] = {
                EGL_DMA_BUF_PLANE0_FD_EXT,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                EGL_DMA_BUF_PLANE0_PITCH_EXT,
                EGL_DMA_BUF_PLANE1_FD_EXT,
                EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                EGL_DMA_BUF_PLANE1_PITCH_EXT,
                EGL_DMA_BUF_PLANE2_FD_EXT,
                EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                EGL_DMA_BUF_PLANE2_PITCH_EXT,
                EGL_DMA_BUF_PLANE3_FD_EXT,
                EGL_DMA_BUF_PLANE3_OFFSET_EXT,
                EGL_DMA_BUF_PLANE3_PITCH_EXT
            };

            const EGLint * n = attnames;

            for (int i = 0; i < pic->i_planes; ++i)
            {
                *a++ = *n++;
                *a++ = fd;
                *a++ = *n++;
                *a++ = pic->p[i].p_pixels - base_addr;
                *a++ = *n++;
                *a++ = pic->p[i].i_pitch;
            }
        }

        *a = EGL_NONE;

        const EGLImage image = tc->gl->egl.createImageKHR(tc->gl, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (!image) {
           msg_Err(tc, "Failed to import fd %d: Err=%#x", fd, eglGetError());
           goto fail;
        }

        // ** ?? tc->tex_target
        tc->vt->GenTextures(1, &tex->texture);
        tc->vt->BindTexture(GL_TEXTURE_EXTERNAL_OES, tex->texture);
        tc->vt->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        tc->vt->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        sys->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

        tc->gl->egl.destroyImageKHR(tc->gl, image);
    }

    if (cma_buf_pic_add_context2(pic, &tex->cmn) != VLC_SUCCESS)
    {
        msg_Err(tc, "%s: add_context2 failed", __func__);
        goto fail;
    }
    return tex;

fail:
    tex_context_delete(tex);
    return NULL;
}


static int
tc_mmal_update(const opengl_tex_converter_t *tc, GLuint *textures,
                const GLsizei *tex_width, const GLsizei *tex_height,
                picture_t *pic, const size_t *plane_offset)
{
    mmal_gl_converter_t * const sys = tc->priv;
#if TRACE_ALL
    msg_Err(tc, "%s: %d*%dx%d : %d*%dx%d", __func__, tc->tex_count, tex_width[0], tex_height[0], pic->i_planes, pic->p[0].i_pitch, pic->p[0].i_lines);
#endif
    VLC_UNUSED(tex_width);
    VLC_UNUSED(tex_height);
    VLC_UNUSED(plane_offset);

    if (!is_cma_buf_pic_chroma(pic->format.i_chroma))
    {
        char cbuf[5];
        msg_Err(tc, "Pic with unexpected chroma: %s", str_fourcc(cbuf, pic->format.i_chroma));
        return VLC_EGENERIC;
    }

    tex_context_t * const tex = get_tex_context(tc, pic);
    if (tex == NULL)
        return VLC_EGENERIC;

//    tc->vt->BindTexture(GL_TEXTURE_EXTERNAL_OES, tex->texture);

    cma_buf_pic_context_unref(sys->last_ctx_ref);  // ?? Needed ??
    sys->last_ctx_ref = cma_buf_pic_context_ref(pic);

    textures[0] = tex->texture;
    return VLC_SUCCESS;
}

static int
tc_mmal_fetch_locations(opengl_tex_converter_t *tc, GLuint program)
{
    tc->uloc.Texture[0] = tc->vt->GetUniformLocation(program, "Texture0");
    return tc->uloc.Texture[0] != -1 ? VLC_SUCCESS : VLC_EGENERIC;
}

static void
tc_mmal_prepare_shader(const opengl_tex_converter_t *tc,
                        const GLsizei *tex_width, const GLsizei *tex_height,
                        float alpha)
{
    (void) tex_width; (void) tex_height; (void) alpha;
    VLC_UNUSED(tc);
//    tc->vt->Uniform1i(tc->uloc.Texture[0], 0);
}

static GLuint
tc_fragment_shader_init(opengl_tex_converter_t * const tc, const GLenum tex_target,
                        const vlc_fourcc_t chroma, const video_color_space_t yuv_space)
{
    VLC_UNUSED(yuv_space);

    tc->tex_count = 1;
    tc->tex_target = tex_target;
    tc->texs[0] = (struct opengl_tex_cfg) {
        { 1, 1 }, { 1, 1 }, GL_RGB, chroma, GL_UNSIGNED_SHORT  //** ??
    };

    tc->pf_fetch_locations = tc_mmal_fetch_locations;
    tc->pf_prepare_shader = tc_mmal_prepare_shader;


    const char fs[] =
       "#extension GL_OES_EGL_image_external : enable\n"
       "precision mediump float;\n"
       "uniform samplerExternalOES Texture0;\n"
       "varying vec2 TexCoord0;\n"
       "void main() {\n"
       "  gl_FragColor = texture2D(Texture0, TexCoord0);\n"
       "}\n";


    const char *code = fs;

    GLuint fragment_shader = tc->vt->CreateShader(GL_FRAGMENT_SHADER);
    tc->vt->ShaderSource(fragment_shader, 1, &code, NULL);
    tc->vt->CompileShader(fragment_shader);
    return fragment_shader;
}


static void
CloseGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    mmal_gl_converter_t * const sys = tc->priv;

    if (sys == NULL)
        return;

    cma_buf_pic_context_unref(sys->last_ctx_ref);
    cma_vcsm_exit(sys->vcsm_init_type);
    free(sys);
}

static int
OpenGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    int rv = VLC_EGENERIC;
    const EGLint eglfmt = vlc_to_gl_fourcc(&tc->fmt);

    // Accept Opaque (as it can definitely be converted) or what we actually want
    if (!(tc->fmt.i_chroma == FMT_IN ||
          tc->fmt.i_chroma == VLC_CODEC_MMAL_OPAQUE))
    {
        return rv;
    }

    if (tc->gl->ext != VLC_GL_EXT_EGL ||
        !tc->gl->egl.createImageKHR || !tc->gl->egl.destroyImageKHR)
    {
        // Missing an important callback
        msg_Dbg(tc, "Missing EGL xxxImageKHR calls");
        return rv;
    }

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

    if ((sys->vcsm_init_type = cma_vcsm_init()) != VCSM_INIT_CMA) {
        msg_Dbg(tc, "VCSM init failed");
        goto fail;
    }

    if ((sys->glEGLImageTargetTexture2DOES = vlc_gl_GetProcAddress(tc->gl, "glEGLImageTargetTexture2DOES")) == NULL)
    {
        msg_Err(tc, "Failed to bind GL fns");
        goto fail;
    }

    if ((tc->fshader = tc_fragment_shader_init(tc, GL_TEXTURE_EXTERNAL_OES,
                                                   eglfmt == 0 ? VLC_CODEC_RGB32 : tc->fmt.i_chroma,
                                                   eglfmt == 0 ? COLOR_SPACE_SRGB : tc->fmt.space)) == 0)
    {
        msg_Err(tc, "Failed to make shader");
        goto fail;
    }

    if (eglfmt == 0)
    {
        tc->fmt.i_chroma = FMT_IN;
        tc->fmt.i_bits_per_pixel = 8;
        if (tc->fmt.i_chroma == VLC_CODEC_MMAL_ZC_RGB32)
        {
            tc->fmt.i_rmask = 0xff0000;
            tc->fmt.i_gmask = 0xff00;
            tc->fmt.i_bmask = 0xff;
            tc->fmt.space = COLOR_SPACE_SRGB;
        }
        else
        {
            tc->fmt.i_rmask = 0;
            tc->fmt.i_gmask = 0;
            tc->fmt.i_bmask = 0;
            tc->fmt.space = COLOR_SPACE_UNDEF;
        }
        sys->drm_fourcc = vlc_to_gl_fourcc(&tc->fmt);
    }

    tc->handle_texs_gen = true;  // We manage the texs
    tc->pf_update  = tc_mmal_update;
    return VLC_SUCCESS;

fail:
    CloseGLConverter(obj);
    return rv;
}

vlc_module_begin ()
    set_description("MMAL OpenGL surface converter")
    set_shortname (N_("MMALGLConverter"))
    set_capability("glconv", 900)
    set_callbacks(OpenGLConverter, CloseGLConverter)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    add_shortcut("mmal_gl_converter")
vlc_module_end ()

