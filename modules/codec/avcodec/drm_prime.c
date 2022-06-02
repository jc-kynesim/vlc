#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_picture_pool.h>

#include <libavcodec/avcodec.h>

#include "avcodec.h"
#include "va.h"
#include "drm_pic.h"

typedef struct vlc_drm_prime_sys_s {
    vlc_video_context * vctx;
} vlc_drm_prime_sys_t;

static const AVCodecHWConfig* find_hw_config(const AVCodecContext * const ctx)
{
  const AVCodecHWConfig* config = NULL;
  for (int n = 0; (config = avcodec_get_hw_config(ctx->codec, n)); n++)
  {
    if (config->pix_fmt != AV_PIX_FMT_DRM_PRIME)
      continue;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        config->device_type == AV_HWDEVICE_TYPE_DRM)
      return config;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
      return config;
  }

  return NULL;
}

static int DrmPrimeGet(vlc_va_t *va, picture_t *pic, AVCodecContext * avctx, AVFrame * frame)
{
    msg_Info(va, "%s: frame=%p", __func__, frame);

    if (avcodec_default_get_buffer2(avctx, frame, 0))
    {
        msg_Err(va, "%s: HW alloc failure", __func__);
        return VLC_EGENERIC;
    }

    return drm_prime_attach_buf_to_pic(NULL, pic, frame);

#if 0
    vlc_va_surface_t *va_surface = va_pool_Get(sys->va_pool);
    if (unlikely(va_surface == NULL))
        return VLC_ENOITEM;
    vaapi_dec_pic_context *vaapi_ctx = malloc(sizeof(*vaapi_ctx));
    if (unlikely(vaapi_ctx == NULL))
    {
        va_surface_Release(va_surface);
        return VLC_ENOMEM;
    }
    vaapi_ctx->ctx.s = (picture_context_t) {
        vaapi_dec_pic_context_destroy, vaapi_dec_pic_context_copy,
        sys->vctx,
    };
    vaapi_ctx->ctx.surface = sys->render_targets[va_surface_GetIndex(va_surface)];
    vaapi_ctx->ctx.va_dpy = sys->hw_ctx.display;
    vaapi_ctx->va_surface = va_surface;
    vlc_vaapi_PicSetContext(pic, &vaapi_ctx->ctx);
    data[3] = (void *) (uintptr_t) vaapi_ctx->ctx.surface;

    return VLC_SUCCESS;
#endif
}

static void DrmPrimeDelete(vlc_va_t *va)
{
    vlc_drm_prime_sys_t * const sys = (vlc_drm_prime_sys_t *)va->sys;

    if (!sys)
        return;

    va->sys = NULL;
    va->ops = NULL;
    if (sys->vctx)
        vlc_video_context_Release(sys->vctx);
//    va_pool_Close(sys->va_pool);
    free(sys);
}

// *** Probably wrong but it doesn't matter
#define VLC_TIME_BASE 1000000

static int DrmPrimeCreate(vlc_va_t *va, AVCodecContext *ctx, enum AVPixelFormat hwfmt, const AVPixFmtDescriptor *desc,
                  const es_format_t *fmt_in, vlc_decoder_device *dec_device,
                  video_format_t *fmt_out, vlc_video_context **vtcx_out)
{
    VLC_UNUSED(desc);
    VLC_UNUSED(fmt_in);

    msg_Dbg(va, "<<< %s: hwfmt=%d, dec_device=%p, type=%d, ctx fmt=%d/%d", __func__, hwfmt, dec_device, dec_device ? (int)dec_device->type : -1,
            ctx->pix_fmt, ctx->sw_pix_fmt);

    if ( hwfmt != AV_PIX_FMT_DRM_PRIME || dec_device == NULL ||
        dec_device->type != VLC_DECODER_DEVICE_DRM_PRIME)
        return VLC_EGENERIC;

    vlc_drm_prime_sys_t *sys = malloc(sizeof *sys);
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    va->sys = sys;
    memset(sys, 0, sizeof (*sys));

    const AVCodecHWConfig* pConfig = find_hw_config(ctx);

    if (pConfig && (pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        pConfig->device_type == AV_HWDEVICE_TYPE_DRM)
    {
        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_DRM,
                                 NULL, NULL, 0) < 0)
        {
            msg_Err(va, "%s: unable to create hwdevice context", __func__);
            return VLC_EGENERIC;
        }
#if 0
        if ((ctx->hw_frames_ctx = av_hwframe_ctx_alloc(ctx->hw_device_ctx)) == NULL)
        {
            msg_Err(va, "%s: unable to create hwframe context", __func__);
            return VLC_EGENERIC;
        }
#endif
    }

#if 0
    m_pCodecContext->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    m_pCodecContext->opaque = static_cast<void*>(this);
    m_pCodecContext->get_format = GetFormat;
    m_pCodecContext->get_buffer2 = GetBuffer;
#endif
    ctx->extra_hw_frames = 6;  // set blocking frame alloc for Pi H265 - unlikely to do anything elsewhere
    ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    ctx->time_base.num = 1;
    ctx->time_base.den = VLC_TIME_BASE;
    ctx->pkt_timebase.num = 1;
    ctx->pkt_timebase.den = VLC_TIME_BASE;

    if ((sys->vctx = vlc_video_context_Create(dec_device, VLC_VIDEO_CONTEXT_DRM_PRIME, 0, NULL)) == NULL)
        goto error;

    {
        static const struct vlc_va_operations ops = {
            .get = DrmPrimeGet,
            .close = DrmPrimeDelete
        };
        va->ops = &ops;
    }

    // Ctx pix fmt is our best guess
    // In general it won't matter if we get it wrong as we pull actual info for
    // the format in the drm prime descriptor
    switch (ctx->sw_pix_fmt)
    {
        case AV_PIX_FMT_YUV420P:
            fmt_out->i_chroma = VLC_CODEC_DRM_PRIME_I420;
            break;
        case AV_PIX_FMT_RPI4_8:
            fmt_out->i_chroma = VLC_CODEC_DRM_PRIME_SAND8;
            break;
        case AV_PIX_FMT_RPI4_10:
        case AV_PIX_FMT_YUV420P10LE:  // When probing this is the swfmt given
            fmt_out->i_chroma = VLC_CODEC_DRM_PRIME_SAND30;
            break;
        default:
            msg_Warn(va, "Unexpected sw_pix_fmt: %d", ctx->sw_pix_fmt);
            /* FALLTHRU */
        case AV_PIX_FMT_NV12:
            fmt_out->i_chroma = VLC_CODEC_DRM_PRIME_NV12;
            break;
    }

    *vtcx_out = sys->vctx;

    return VLC_SUCCESS;

error:
    DrmPrimeDelete(va);
    return VLC_EGENERIC;
}


static void
DrmPrimeDecoderDeviceClose(vlc_decoder_device *device)
{
    msg_Dbg(device, "<<< %s", __func__);
}

static const struct vlc_decoder_device_operations dev_ops = {
    .close = DrmPrimeDecoderDeviceClose,
};

static int
DrmPrimeDecoderDeviceOpen(vlc_decoder_device *device, vout_window_t *window)
{
    if (!window)
        return VLC_EGENERIC;

    msg_Dbg(device, "<<< %s", __func__);

    device->ops = &dev_ops;
    device->type = VLC_DECODER_DEVICE_DRM_PRIME;
    device->opaque = NULL;
    return VLC_SUCCESS;
}


vlc_module_begin ()
    set_description( N_("DRM-PRIME video decoder") )
    set_va_callback( DrmPrimeCreate, 100 )
    add_shortcut( "drm_prime" )
    set_subcategory( SUBCAT_INPUT_VCODEC )

    add_submodule()
    set_callback_dec_device(DrmPrimeDecoderDeviceOpen, 300)

vlc_module_end ()

