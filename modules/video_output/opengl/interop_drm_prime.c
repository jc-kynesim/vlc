/*****************************************************************************
 * converter_vaapi.c: OpenGL VAAPI opaque converter
 *****************************************************************************
 * Copyright (C) 2017 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>

#include <vlc_common.h>
#include <vlc_vout_window.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>

#include "gl_api.h"
#include "interop.h"
#include "../codec/avcodec/drm_pic.h"

#define OPT_MULTIPLANE 0
/* From https://www.khronos.org/registry/OpenGL/extensions/OES/OES_EGL_image.txt
 * The extension is an OpenGL ES extension but can (and usually is) available on
 * OpenGL implementations. */
#ifndef GL_OES_EGL_image
#define GL_OES_EGL_image 1
typedef void *GLeglImageOES;
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
#endif

#define IMAGES_MAX 4
struct priv
{
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    unsigned fourcc;

    struct {
        picture_t *                 pic;
        EGLImageKHR images[IMAGES_MAX];
    } last;
};

static inline bool
vlc_drm_prime_IsChromaOpaque(const int i_vlc_chroma)
{
    return i_vlc_chroma == VLC_CODEC_DRM_PRIME_OPAQUE;
}

static void destroy_images(const struct vlc_gl_interop *interop, EGLImageKHR imgs[IMAGES_MAX])
{
    unsigned int i;
    for (i = 0; i != IMAGES_MAX; ++i)
    {
        const EGLImageKHR img = imgs[i];
        imgs[i] = NULL;
        if (img)
            interop->gl->egl.destroyImageKHR(interop->gl, img);
    }
}

static void release_last(const struct vlc_gl_interop *interop, struct priv *priv)
{
    if (priv->last.pic != NULL)
        picture_Release(priv->last.pic);
    priv->last.pic = NULL;
    destroy_images(interop, priv->last.images);
}

static int
tc_vaegl_update(const struct vlc_gl_interop *interop, GLuint *textures,
                const GLsizei *tex_width, const GLsizei *tex_height,
                picture_t *pic, const size_t *plane_offset)
{
    (void) plane_offset;
    struct priv *priv = interop->priv;
    vlc_object_t *o = VLC_OBJECT(interop->gl);
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
    EGLImageKHR images[IMAGES_MAX] = {NULL};
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
        msg_Err(o, "%s: No DRM Frame desriptor found", __func__);
        return VLC_EGENERIC;
    }

#if OPT_MULTIPLANE
    static const uint32_t fourcc_i420_8[4] = {
        DRM_FORMAT_R8, DRM_FORMAT_R8, DRM_FORMAT_R8, 0
    };
    const uint32_t * fourccs = fourcc_i420_8;
    unsigned int n = 0;

    for (int i = 0; i < desc->nb_layers; ++i)
    {
        const AVDRMLayerDescriptor * const layer = desc->layers + i;
        for (int j = 0; j != layer->nb_planes; ++j)
        {
            const AVDRMPlaneDescriptor * const plane = layer->planes + j;
            const AVDRMObjectDescriptor * const obj = desc->objects + plane->object_index;
            const EGLint * ext = plane_exts;
            EGLint *a = attribs;

            *a++ = EGL_WIDTH;
            *a++ = tex_width[n];
            *a++ = EGL_HEIGHT;
            *a++ = tex_height[n];
            *a++ = EGL_LINUX_DRM_FOURCC_EXT;
            *a++ = fourccs[n];

            *a++ = *ext++; // FD
            *a++ = obj->fd;
            *a++ = *ext++; // OFFSET
            *a++ = plane->offset;
            *a++ = *ext++; // PITCH
            *a++ = plane->pitch;
            if (obj->format_modifier && obj->format_modifier != DRM_FORMAT_MOD_INVALID)
            {
                *a++ = *ext++; // MODIFIER_LO
                *a++ = (EGLint)(obj->format_modifier & 0xffffffff);
                *a++ = *ext++; // MODIFIER_HI
                *a++ = (EGLint)(obj->format_modifier >> 32);
            }
            *a++ = EGL_NONE;
            *a++ = 0;

            if ((images[n] = interop->gl->egl.createImageKHR(interop->gl, EGL_LINUX_DMA_BUF_EXT,
                                                   NULL, attribs)) == NULL)
            {
                msg_Err(o, "Failed create %08x image %d KHR %dx%d fd=%d, offset=%d, pitch=%d, mod=%#" PRIx64 ": err=%#x",
                        fourccs[n], n, tex_width[n], tex_height[n],
                        obj->fd, plane->offset, plane->pitch, obj->format_modifier, interop->gl->egl.getError());
                goto fail;
            }

            interop->vt->BindTexture(interop->tex_target, textures[n]);
            priv->glEGLImageTargetTexture2DOES(interop->tex_target, images[n]);

            ++n;
        }
    }
#else
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

    if ((images[0] = interop->gl->egl.createImageKHR(interop->gl, EGL_LINUX_DMA_BUF_EXT,
                                           NULL, attribs)) == NULL)
    {
        msg_Err(o, "Failed create image KHR");
        goto fail;
    }

    interop->vt->BindTexture(interop->tex_target, textures[0]);
    priv->glEGLImageTargetTexture2DOES(interop->tex_target, images[0]);
#endif

    if (pic != priv->last.pic)
    {
        release_last(interop, priv);
        priv->last.pic = picture_Hold(pic);
        memcpy(priv->last.images, images, sizeof(images));
    }

    return VLC_SUCCESS;

fail:
    destroy_images(interop, images);
    return VLC_EGENERIC;
}

static void
Close(struct vlc_gl_interop *interop)
{
    struct priv *priv = interop->priv;

    msg_Info(interop, "Close DRM_PRIME");

    release_last(interop, priv);
    free(priv);
}

static void egl_err_cb(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message)
{
    VLC_UNUSED(threadLabel);
    VLC_UNUSED(objectLabel);
    fprintf(stderr, "::: EGL: Err=%#x, Cmd='%s', Type=%#x, Msg='%s'\n", error, command, messageType, message);
}

static int
Open(vlc_object_t *obj)
{
    struct vlc_gl_interop *interop = (void *) obj;

    msg_Info(obj, "Try DRM_PRIME: Chroma=%s", fourcc2str(interop->fmt_in.i_chroma));

    if (!interop->gl->egl.debugMessageControlKHR)
    {
        msg_Err(obj, "No EGL debug");
    }
    else
    {
        static const EGLAttrib atts[] = {
         EGL_DEBUG_MSG_CRITICAL_KHR, 1,
         EGL_DEBUG_MSG_ERROR_KHR, 1,
         EGL_DEBUG_MSG_WARN_KHR, 1,
         EGL_DEBUG_MSG_INFO_KHR, 1,
         EGL_NONE, 0
        };
        interop->gl->egl.debugMessageControlKHR((void *)egl_err_cb, atts);
    }

    if (interop->vctx == NULL) {
        msg_Info(obj, "DRM PRIME no context");
        return VLC_EGENERIC;
    }
    vlc_decoder_device *dec_device = vlc_video_context_HoldDevice(interop->vctx);
    if (dec_device->type != VLC_DECODER_DEVICE_DRM_PRIME
     || !vlc_drm_prime_IsChromaOpaque(interop->fmt_in.i_chroma)
     || interop->gl->ext != VLC_GL_EXT_EGL
     || interop->gl->egl.createImageKHR == NULL
     || interop->gl->egl.destroyImageKHR == NULL)
    {
        msg_Err(obj, "DRM_PRIME no interop - device=%d, gl=%d", dec_device->type, interop->gl->ext);
        vlc_decoder_device_Release(dec_device);
        return VLC_EGENERIC;
    }

    if (!vlc_gl_StrHasToken(interop->api->extensions, "GL_OES_EGL_image"))
    {
        msg_Err(obj, "GL missing GL_OES_EGL_image");
        vlc_decoder_device_Release(dec_device);
        return VLC_EGENERIC;
    }

#if 1
    const char *eglexts = interop->gl->egl.queryString(interop->gl, EGL_EXTENSIONS);
    if (eglexts == NULL || !vlc_gl_StrHasToken(eglexts, "EGL_EXT_image_dma_buf_import"))
    {
        if (eglexts == NULL)
            msg_Err(obj, "No EGL extensions");
        else
            msg_Err(obj, "GL missing EGL_EXT_image_dma_buf_import");

        vlc_decoder_device_Release(dec_device);
        return VLC_EGENERIC;
    }
#endif

    msg_Info(obj, "DRM_PRIME looks good");

    struct priv *priv = interop->priv = calloc(1, sizeof(struct priv));
    if (unlikely(priv == NULL))
        goto error;
#if 0
    priv->fourcc = 0;

    int va_fourcc;
    int vlc_sw_chroma;
    switch (interop->fmt_in.i_chroma)
    {
        case VLC_CODEC_VAAPI_420:
            va_fourcc = VA_FOURCC_NV12;
            vlc_sw_chroma = VLC_CODEC_NV12;
            break;
        case VLC_CODEC_VAAPI_420_10BPP:
            va_fourcc = VA_FOURCC_P010;
            vlc_sw_chroma = VLC_CODEC_P010;
            break;
        default:
            vlc_assert_unreachable();
    }

    if (vaegl_init_fourcc(priv, va_fourcc))
        goto error;

    priv->vadpy = dec_device->opaque;
    assert(priv->vadpy != NULL);

    if (tc_va_check_interop_blacklist(interop, priv->vadpy))
        goto error;

    if (tc_va_check_derive_image(interop))
        goto error;

#endif
    priv->glEGLImageTargetTexture2DOES =
        vlc_gl_GetProcAddress(interop->gl, "glEGLImageTargetTexture2DOES");
    if (priv->glEGLImageTargetTexture2DOES == NULL) {
        msg_Err(obj, "glEGLImageTargetTexture2DOES missing");
        goto error;
    }

    /* The pictures are uploaded upside-down */
    video_format_TransformBy(&interop->fmt_out, TRANSFORM_VFLIP);

#if OPT_MULTIPLANE
    int ret = opengl_interop_init(interop, GL_TEXTURE_2D, VLC_CODEC_I420, interop->fmt_in.space);
#else
    // If using EXTERNAL_OES then color space must be UNDEFINED with VLCs
    // current shader code.  It doesn't do RGB->RGB colour conversions.
//    int ret = opengl_interop_init(interop, GL_TEXTURE_EXTERNAL_OES, VLC_CODEC_DRM_PRIME_OPAQUE, COLOR_SPACE_UNDEF);
    int ret = opengl_interop_init(interop, GL_TEXTURE_EXTERNAL_OES, VLC_CODEC_RGB24, COLOR_SPACE_UNDEF);
#endif
    if (ret != VLC_SUCCESS)
    {
        msg_Err(obj, "Interop Init failed");
        goto error;
    }
    static const struct vlc_gl_interop_ops ops = {
        .update_textures = tc_vaegl_update,
        .close = Close,
    };
    interop->ops = &ops;

    vlc_decoder_device_Release(dec_device);

    return VLC_SUCCESS;
error:
    vlc_decoder_device_Release(dec_device);
    free(priv);
    return VLC_EGENERIC;
}

vlc_module_begin ()
    set_description("DRM PRIME OpenGL surface converter")
    set_capability("glinterop", 1)
    set_callback(Open)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    add_shortcut("drm_prime")
vlc_module_end ()


