#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_picture.h>

#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "../../video_output/opengl/converter.h"
#include "../../codec/avcodec/drm_pic.h"

#include <assert.h>

#define TRACE_ALL 0

typedef struct drm_gl_converter_s
{
    EGLint drm_fourcc;

    EGLImageKHR last_image;
    picture_t * last_pic;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
} drm_gl_converter_t;


static void unset_last(const opengl_tex_converter_t * const tc)
{
    drm_gl_converter_t * const sys = tc->priv;

    if (sys->last_image)
    {
        tc->gl->egl.destroyImageKHR(tc->gl, sys->last_image);
        sys->last_image = NULL;
    }

    if (sys->last_pic)
    {
        picture_Release(sys->last_pic);
        sys->last_pic = NULL;
    }
}

static void set_last(const opengl_tex_converter_t * const tc, EGLImageKHR image, picture_t * pic)
{
    drm_gl_converter_t * const sys = tc->priv;
    sys->last_image = image;
    sys->last_pic = picture_Hold(pic);
}

static int
tc_drm_update(const opengl_tex_converter_t *tc, GLuint *textures,
                const GLsizei *tex_width, const GLsizei *tex_height,
                picture_t *pic, const size_t *plane_offset)
{
    drm_gl_converter_t * const sys = tc->priv;
#if TRACE_ALL || 1
    {
        char cbuf[5];
        msg_Dbg(tc, "%s: %s %d*%dx%d : %d*%dx%d", __func__,
                str_fourcc(cbuf, pic->format.i_chroma),
                tc->tex_count, tex_width[0], tex_height[0], pic->i_planes, pic->p[0].i_pitch, pic->p[0].i_lines);
    }
#endif
    VLC_UNUSED(tex_width);
    VLC_UNUSED(tex_height);
    VLC_UNUSED(plane_offset);

    {
        const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
        EGLint attribs[64] = {0};

        static const EGLint plane_exts[] = {
            EGL_DMA_BUF_PLANE0_FD_EXT,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE1_FD_EXT,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_FD_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        };

    //    msg_Info(o, "<<< %s", __func__);

        if (!desc)
        {
            msg_Err(tc, "%s: No DRM Frame desriptor found", __func__);
            return VLC_EGENERIC;
        }

        EGLint *a = attribs;
        *a++ = EGL_WIDTH;
        *a++ = tex_width[0];
        *a++ = EGL_HEIGHT;
        *a++ = tex_height[0];
        *a++ = EGL_LINUX_DRM_FOURCC_EXT;
        *a++ = desc->layers[0].format;

        const EGLint * ext = plane_exts;

        for (int i = 0; i < desc->nb_layers; ++i)
        {
            const AVDRMLayerDescriptor * const layer = desc->layers + i;
            for (int j = 0; j != layer->nb_planes; ++j)
            {
                const AVDRMPlaneDescriptor * const plane = layer->planes + j;
                const AVDRMObjectDescriptor * const obj = desc->objects + plane->object_index;

                *a++ = *ext++; // FD
                *a++ = obj->fd;
                *a++ = *ext++; // OFFSET
                *a++ = plane->offset;
                *a++ = *ext++; // PITCH
                *a++ = plane->pitch;
                if (!obj->format_modifier || obj->format_modifier == DRM_FORMAT_MOD_INVALID)
                {
                    ext += 2;
                }
                else
                {
                    *a++ = *ext++; // MODIFIER_LO
                    *a++ = (EGLint)(obj->format_modifier & 0xffffffff);
                    *a++ = *ext++; // MODIFIER_HI
                    *a++ = (EGLint)(obj->format_modifier >> 32);
                }
            }
        }
        *a++ = EGL_NONE;
        *a++ = 0;

        const EGLImageKHR image = tc->gl->egl.createImageKHR(tc->gl, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (!image) {
           msg_Err(tc, "Failed to createImageKHR: Err=%#x", tc->vt->GetError());
           return VLC_EGENERIC;
        }

        // *** MMAL ZC does this a little differently
        // tc->tex_target == OES???

        tc->vt->BindTexture(GL_TEXTURE_EXTERNAL_OES, textures[0]);
        tc->vt->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        tc->vt->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        sys->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

        unset_last(tc);
        set_last(tc, image, pic);
    }

    return VLC_SUCCESS;
}

static int
tc_drm_fetch_locations(opengl_tex_converter_t *tc, GLuint program)
{
    tc->uloc.Texture[0] = tc->vt->GetUniformLocation(program, "Texture0");
    return tc->uloc.Texture[0] != -1 ? VLC_SUCCESS : VLC_EGENERIC;
}

static void
tc_drm_prepare_shader(const opengl_tex_converter_t *tc,
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

    tc->pf_fetch_locations = tc_drm_fetch_locations;
    tc->pf_prepare_shader = tc_drm_prepare_shader;


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
    drm_gl_converter_t * const sys = tc->priv;

    if (sys == NULL)
        return;

    unset_last(tc);
    free(sys);
}


static int
OpenGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    int rv = VLC_EGENERIC;

    // Do we know what to do with this?
    // There must be a way of probing for supported formats...
    if (!(tc->fmt.i_chroma == VLC_CODEC_DRM_PRIME_I420 ||
          tc->fmt.i_chroma == VLC_CODEC_DRM_PRIME_NV12 ||
          tc->fmt.i_chroma == VLC_CODEC_DRM_PRIME_SAND8))
        return VLC_EGENERIC;

    {
        msg_Dbg(tc, "<<< %s: %.4s %dx%d [(%d,%d) %d/%d] sar:%d/%d", __func__,
                (char *)&tc->fmt.i_chroma,
                tc->fmt.i_width, tc->fmt.i_height,
                tc->fmt.i_x_offset, tc->fmt.i_y_offset,
                tc->fmt.i_visible_width, tc->fmt.i_visible_height,
                tc->fmt.i_sar_num, tc->fmt.i_sar_den);
    }

    if (tc->gl->ext != VLC_GL_EXT_EGL ||
        !tc->gl->egl.createImageKHR || !tc->gl->egl.destroyImageKHR)
    {
        // Missing an important callback
        msg_Dbg(tc, "Missing EGL xxxImageKHR calls");
        return rv;
    }

    if ((tc->priv = calloc(1, sizeof(drm_gl_converter_t))) == NULL)
    {
        msg_Err(tc, "priv alloc failure");
        rv = VLC_ENOMEM;
        goto fail;
    }
    drm_gl_converter_t * const sys = tc->priv;

    if ((sys->glEGLImageTargetTexture2DOES = vlc_gl_GetProcAddress(tc->gl, "glEGLImageTargetTexture2DOES")) == NULL)
    {
        msg_Err(tc, "Failed to bind GL fns");
        goto fail;
    }

    if ((tc->fshader = tc_fragment_shader_init(tc, GL_TEXTURE_EXTERNAL_OES, tc->fmt.i_chroma, tc->fmt.space)) == 0)
    {
        msg_Err(tc, "Failed to make shader");
        goto fail;
    }

    tc->handle_texs_gen = true;  // We manage the texs
    tc->pf_update  = tc_drm_update;

    return VLC_SUCCESS;

fail:
    CloseGLConverter(obj);
    return rv;
}

vlc_module_begin ()
    set_description("DRM OpenGL surface converter")
    set_shortname (N_("DRMGLConverter"))
    set_capability("glconv", 900)
    set_callbacks(OpenGLConverter, CloseGLConverter)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    add_shortcut("drm_gl_converter")
vlc_module_end ()

