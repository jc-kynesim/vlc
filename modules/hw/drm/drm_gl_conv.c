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

#define ICACHE_SIZE 2

typedef struct drm_gl_converter_s
{
    EGLint drm_fourcc;

    unsigned int icache_n;
    struct icache_s {
        EGLImageKHR last_image;
        picture_context_t * last_ctx;
    } icache[ICACHE_SIZE];

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
} drm_gl_converter_t;


static void
unset_icache_ent(const opengl_tex_converter_t * const tc, struct icache_s * const s)
{
    if (s->last_image)
    {
        tc->gl->egl.destroyImageKHR(tc->gl, s->last_image);
        s->last_image = NULL;
    }

    if (s->last_ctx)
    {
        s->last_ctx->destroy(s->last_ctx);
        s->last_ctx = NULL;
    }
}

static void
update_icache(const opengl_tex_converter_t * const tc, EGLImageKHR image, picture_t * pic)
{
    drm_gl_converter_t * const sys = tc->priv;
    struct icache_s * const s = sys->icache + sys->icache_n;

    s->last_image = image;
    // DRM buffer is held by the context, pictures can be in surprisingly
    // small pools for filters so let go of the pic and keep a ref on the
    // context
    unset_icache_ent(tc, s);
    s->last_ctx = pic->context->copy(pic->context);
    sys->icache_n = sys->icache_n + 1 >= ICACHE_SIZE ? 0 : sys->icache_n + 1;
}

static int
tc_drm_update(const opengl_tex_converter_t *tc, GLuint *textures,
                const GLsizei *tex_width, const GLsizei *tex_height,
                picture_t *pic, const size_t *plane_offset)
{
    drm_gl_converter_t * const sys = tc->priv;
#if TRACE_ALL
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

        if (vlc_fourcc_IsYUV(pic->format.i_chroma))
        {
            *a++ = EGL_SAMPLE_RANGE_HINT_EXT;
            *a++ = pic->format.b_color_range_full ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT;

            switch (pic->format.chroma_location)
            {
                case CHROMA_LOCATION_LEFT:
                    *a++ = EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_EXT;
                    *a++ = EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_5_EXT;
                    break;
                case CHROMA_LOCATION_CENTER:
                    *a++ = EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_5_EXT;
                    *a++ = EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_5_EXT;
                    break;
                case CHROMA_LOCATION_TOP_LEFT:
                    *a++ = EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_EXT;
                    *a++ = EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_EXT;
                    break;
                case CHROMA_LOCATION_TOP_CENTER:
                    *a++ = EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_5_EXT;
                    *a++ = EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT;
                    *a++ = EGL_YUV_CHROMA_SITING_0_EXT;
                    break;
                case CHROMA_LOCATION_BOTTOM_LEFT:
                case CHROMA_LOCATION_BOTTOM_CENTER:
                case CHROMA_LOCATION_UNDEF:
                default:
                    break;
            }

            switch (pic->format.space)
            {
                case COLOR_SPACE_BT2020:
                    *a++ = EGL_YUV_COLOR_SPACE_HINT_EXT;
                    *a++ = EGL_ITU_REC2020_EXT;
                    break;
                case COLOR_SPACE_BT601:
                    *a++ = EGL_YUV_COLOR_SPACE_HINT_EXT;
                    *a++ = EGL_ITU_REC601_EXT;
                    break;
                case COLOR_SPACE_BT709:
                    *a++ = EGL_YUV_COLOR_SPACE_HINT_EXT;
                    *a++ = EGL_ITU_REC709_EXT;
                    break;
                case COLOR_SPACE_UNDEF:
                default:
                    break;
            }
        }

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
                if (obj->format_modifier == DRM_FORMAT_MOD_INVALID)
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

        update_icache(tc, image, pic);
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
    unsigned int i;

    if (sys == NULL)
        return;

    for (i = 0; i != ICACHE_SIZE; ++i)
        unset_icache_ent(tc, sys->icache + i);
    free(sys);
}

#ifndef DRM_FORMAT_P030
#define DRM_FORMAT_P030 fourcc_code('P', '0', '3', '0')
#endif

static struct vlc_to_drm_mod_s {
    vlc_fourcc_t chroma;
    uint32_t drm_fmt;
    uint64_t drm_mod;
} vlc_to_drm_mods[] = {
    {VLC_CODEC_DRM_PRIME_I420,   DRM_FORMAT_YUV420, DRM_FORMAT_MOD_LINEAR},
    {VLC_CODEC_DRM_PRIME_NV12,   DRM_FORMAT_NV12,   DRM_FORMAT_MOD_LINEAR},
    {VLC_CODEC_DRM_PRIME_SAND8,  DRM_FORMAT_NV12,   DRM_FORMAT_MOD_BROADCOM_SAND128},
    {VLC_CODEC_DRM_PRIME_SAND30, DRM_FORMAT_P030,   DRM_FORMAT_MOD_BROADCOM_SAND128},
};

static bool check_chroma(opengl_tex_converter_t * const tc)
{
    char fcc[5] = {0};
    vlc_fourcc_to_char(tc->fmt.i_chroma, fcc);
    uint32_t fmt = 0;
    uint64_t mod = DRM_FORMAT_MOD_INVALID;
    uint64_t mods[16];
    int32_t mod_count = 0;

    for (unsigned int i = 0; i != ARRAY_SIZE(vlc_to_drm_mods); ++i)
    {
        if (tc->fmt.i_chroma == vlc_to_drm_mods[i].chroma)
        {
            fmt = vlc_to_drm_mods[i].drm_fmt;
            mod = vlc_to_drm_mods[i].drm_mod;
            break;
        }
    }
    if (!fmt)
        return false;

    if (!tc->gl->egl.queryDmaBufModifiersEXT)
    {
        msg_Dbg(tc, "No queryDmaBufModifiersEXT");
        return false;
    }

    if (!tc->gl->egl.queryDmaBufModifiersEXT(tc->gl, fmt, 16, mods, NULL, &mod_count))
    {
        msg_Dbg(tc, "queryDmaBufModifiersEXT Failed for %s", fcc);
        return false;
    }

    for (int32_t i = 0; i < mod_count; ++i)
    {
        if (mods[i] == mod)
            return true;
    }
    msg_Dbg(tc, "Mod %" PRIx64 " not found for %s/%.4s in %d mods", mod, fcc, (char*)&fmt, mod_count);
    return false;
}

static int
OpenGLConverter(vlc_object_t *obj)
{
    opengl_tex_converter_t * const tc = (opengl_tex_converter_t *)obj;
    int rv = VLC_EGENERIC;

    // Do we know what to do with this?
    if (!check_chroma(tc))
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

