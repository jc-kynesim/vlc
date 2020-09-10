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

#include <vlc_common.h>
#include <vlc_vout_window.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>

#include "gl_api.h"
#include "interop.h"
#include "../codec/avcodec/drm_pic.h"

/* From https://www.khronos.org/registry/OpenGL/extensions/OES/OES_EGL_image.txt
 * The extension is an OpenGL ES extension but can (and usually is) available on
 * OpenGL implementations. */
#ifndef GL_OES_EGL_image
#define GL_OES_EGL_image 1
typedef void *GLeglImageOES;
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
#endif

#define DRM_FORMAT_MOD_VENDOR_NONE    0
#define DRM_FORMAT_RESERVED           ((1ULL << 56) - 1)

#define fourcc_mod_code(vendor, val) \
        ((((EGLuint64KHR)DRM_FORMAT_MOD_VENDOR_## vendor) << 56) | ((val) & 0x00ffffffffffffffULL))

#define DRM_FORMAT_MOD_INVALID  fourcc_mod_code(NONE, DRM_FORMAT_RESERVED)


struct priv
{
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    unsigned fourcc;

    struct {
        picture_t *                 pic;
        EGLImageKHR image;
    } last;
};

static inline bool
vlc_drm_prime_IsChromaOpaque(const int i_vlc_chroma)
{
    return i_vlc_chroma == VLC_CODEC_DRM_PRIME_OPAQUE;
}

static const AVDRMFrameDescriptor *
drm_prime_get_desc(picture_t *pic)
{
    const drm_prime_video_sys_t * const vsys =
        vlc_video_context_GetPrivate(pic->context->vctx, VLC_VIDEO_CONTEXT_DRM_PRIME);
    if (!vsys)
        return NULL;
    return (const AVDRMFrameDescriptor *)vsys->buf->data;
}

static void release_last(const struct vlc_gl_interop *interop, struct priv *priv)
{
    if (priv->last.pic != NULL)
        picture_Release(priv->last.pic);
    if (priv->last.image)
        interop->gl->egl.destroyImageKHR(interop->gl, priv->last.image);
    priv->last.pic = NULL;
    priv->last.image = NULL;
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
    EGLint attribs[64] = {0};
    EGLint *a = attribs;

    msg_Info(o, "<<< %s", __func__);

    if (!desc)
    {
        msg_Err(o, "%s: No DRM Frame desriptor found", __func__);
        return VLC_EGENERIC;
    }

    EGLImageKHR image = NULL;

    *a++ = EGL_WIDTH;
    *a++ = tex_width[0];
    *a++ = EGL_HEIGHT;
    *a++ = tex_height[0];
    *a++ = EGL_LINUX_DRM_FOURCC_EXT;
    *a++ = desc->layers[0].format;

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
            if (obj->format_modifier != DRM_FORMAT_MOD_INVALID)
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

    for (int i = 0; attribs[i] != EGL_NONE; i += 2)
    {
        msg_Dbg(o, "a[%2d]: %4x: %d", i, attribs[i], attribs[i+1]);
    }

    image = interop->gl->egl.createImageKHR(interop->gl, EGL_LINUX_DMA_BUF_EXT,
                                           NULL, attribs);
    if (!image)
    {
        msg_Err(o, "Failed create image KHR");
        return VLC_EGENERIC;
    }

    interop->vt->BindTexture(interop->tex_target, textures[0]);
    priv->glEGLImageTargetTexture2DOES(interop->tex_target, image);

    if (pic != priv->last.pic)
    {
        release_last(interop, priv);
        priv->last.pic = picture_Hold(pic);
        priv->last.image = image;
    }

    return VLC_SUCCESS;
}

static void
Close(struct vlc_gl_interop *interop)
{
    struct priv *priv = interop->priv;

    msg_Info(interop, "Close DRM_PRIME");

    release_last(interop, priv);
    free(priv);
}

static int
Open(vlc_object_t *obj)
{
    struct vlc_gl_interop *interop = (void *) obj;

    msg_Info(obj, "Try DRM_PRIME: Chroma=%#x", interop->fmt_in.i_chroma);

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
        vlc_decoder_device_Release(dec_device);
        return VLC_EGENERIC;
    }

    const char *eglexts = interop->gl->egl.queryString(interop->gl, EGL_EXTENSIONS);
    if (eglexts == NULL || !vlc_gl_StrHasToken(eglexts, "EGL_EXT_image_dma_buf_import"))
    {
        vlc_decoder_device_Release(dec_device);
        return VLC_EGENERIC;
    }

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

    /* The pictures are uploaded upside-down */
    video_format_TransformBy(&interop->fmt_out, TRANSFORM_VFLIP);
#endif
    priv->glEGLImageTargetTexture2DOES =
        vlc_gl_GetProcAddress(interop->gl, "glEGLImageTargetTexture2DOES");
    if (priv->glEGLImageTargetTexture2DOES == NULL) {
        msg_Err(obj, "glEGLImageTargetTexture2DOES missing");
        goto error;
    }

    int ret = opengl_interop_init(interop, GL_TEXTURE_2D, VLC_CODEC_I420,
                                  interop->fmt_in.space);
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


