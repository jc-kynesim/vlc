#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>

#include "../../codec/avcodec/drm_pic.h"

#include <assert.h>

#define TRACE_ALL 0

//----------------------------------------------------------------------------
//
// Simple copy in to ZC

typedef struct to_nv12_sys_s {
    int dummy;
} to_nv12_sys_t;

static vlc_fourcc_t
dst_fourcc_vlc_to_av(const vlc_fourcc_t av)
{
    switch (av) {
    case VLC_CODEC_I420:
        return AV_PIX_FMT_YUV420P;
    case VLC_CODEC_NV12:
        return AV_PIX_FMT_NV12;
    case VLC_CODEC_I420_10L:
        return AV_PIX_FMT_YUV420P10LE;
    }
    return 0;
}

static void
pic_buf_free(void *opaque, uint8_t *data)
{
    VLC_UNUSED(data);
    picture_Release(opaque);
}

static AVBufferRef *
mk_buf_from_pic(picture_t * const pic, uint8_t * const data, const size_t size)
{
    return av_buffer_create(data, size, pic_buf_free, picture_Hold(pic), 0);
}

static picture_t *
to_nv12_filter(filter_t *p_filter, picture_t *in_pic)
{
    to_nv12_sys_t * const sys = (to_nv12_sys_t *)p_filter->p_sys;
#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif
    AVFrame * frame_in = av_frame_alloc();
    AVFrame * frame_out = av_frame_alloc();
    drm_prime_video_sys_t * const pctx = (drm_prime_video_sys_t *)in_pic->context;
    int rv;

    VLC_UNUSED(sys);

    if (!frame_in || !frame_out || !pctx)
        goto fail0;

    picture_t * const out_pic = filter_NewPicture(p_filter);
    if (out_pic == NULL)
        goto fail0;

    frame_in->format      = AV_PIX_FMT_DRM_PRIME;
    frame_in->buf[0]      = av_buffer_ref(pctx->buf);
    frame_in->data[0]     = (uint8_t *)pctx->desc;
    frame_in->hw_frames_ctx = av_buffer_ref(pctx->hw_frames_ctx);
    frame_in->width       = in_pic->format.i_width;
    frame_in->height      = in_pic->format.i_height;
    frame_in->crop_left   = in_pic->format.i_x_offset;
    frame_in->crop_top    = in_pic->format.i_y_offset;
    frame_in->crop_right  = frame_in->width - in_pic->format.i_visible_width - frame_in->crop_left;
    frame_in->crop_bottom = frame_in->height - in_pic->format.i_visible_height - frame_in->crop_top;

    frame_out->format     = dst_fourcc_vlc_to_av(p_filter->fmt_out.video.i_chroma);
    frame_out->width      = out_pic->format.i_width;
    frame_out->height     = out_pic->format.i_height;
    for (int i = 0; i != out_pic->i_planes; ++i) {
        frame_out->buf[i] = mk_buf_from_pic(out_pic, out_pic->p[i].p_pixels, out_pic->p[i].i_lines * out_pic->p[i].i_pitch);
        if (!frame_out->buf[i]) {
            msg_Err(p_filter, "Failed to make buf from pic");
            goto fail1;
        }
        frame_out->data[i] = out_pic->p[i].p_pixels;
        frame_out->linesize[i] = out_pic->p[i].i_pitch;
    }

    if ((rv = av_hwframe_transfer_data(frame_out, frame_in, 0)) != 0) {
        msg_Err(p_filter, "Failed to transfer data: %s", av_err2str(rv));
        goto fail1;
    }

    av_frame_free(&frame_in);
    av_frame_free(&frame_out);
    picture_Release(in_pic);
    return out_pic;

fail1:
    picture_Release(out_pic);
fail0:
    av_frame_free(&frame_in);
    av_frame_free(&frame_out);
    picture_Release(in_pic);
    return NULL;
}

static void to_nv12_flush(filter_t * p_filter)
{
    VLC_UNUSED(p_filter);
}

static void CloseConverterToNv12(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    to_nv12_sys_t * const sys = (to_nv12_sys_t *)p_filter->p_sys;

    if (sys == NULL)
        return;

    p_filter->p_sys = NULL;

    free(sys);
}

static bool to_nv12_validate_fmt(const video_format_t * const f_in, const video_format_t * const f_out)
{
    if (f_in->i_height != f_out->i_height ||
        f_in->i_width  != f_out->i_width)
    {
        return false;
    }

    if (f_in->i_chroma == VLC_CODEC_DRM_PRIME_SAND8 &&
        (f_out->i_chroma == VLC_CODEC_I420 || f_out->i_chroma == VLC_CODEC_NV12))
        return true;

    if (f_in->i_chroma == VLC_CODEC_DRM_PRIME_I420 &&
        f_out->i_chroma == VLC_CODEC_I420)
        return true;

    if (f_in->i_chroma == VLC_CODEC_DRM_PRIME_NV12 &&
        f_out->i_chroma == VLC_CODEC_NV12)
        return true;

    if (f_in->i_chroma == VLC_CODEC_DRM_PRIME_SAND30 &&
        (f_out->i_chroma == VLC_CODEC_I420_10L || f_out->i_chroma == VLC_CODEC_NV12))
        return true;

    return false;
}

static int OpenConverterToNv12(vlc_object_t * obj)
{
    int ret = VLC_EGENERIC;
    filter_t * const p_filter = (filter_t *)obj;

    if (!to_nv12_validate_fmt(&p_filter->fmt_in.video, &p_filter->fmt_out.video))
        goto fail;

    {
        msg_Dbg(p_filter, "%s: %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d->%s,%dx%d [(%d,%d) %dx%d] rgb:%#x:%#x:%#x sar:%d/%d", __func__,
                fourcc2str(p_filter->fmt_in.video.i_chroma),
                p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                p_filter->fmt_in.video.i_sar_num, p_filter->fmt_in.video.i_sar_den,
                fourcc2str(p_filter->fmt_out.video.i_chroma),
                p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height,
                p_filter->fmt_out.video.i_rmask, p_filter->fmt_out.video.i_gmask, p_filter->fmt_out.video.i_bmask,
                p_filter->fmt_out.video.i_sar_num, p_filter->fmt_out.video.i_sar_den);
    }

    to_nv12_sys_t * const sys = calloc(1, sizeof(*sys));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    p_filter->p_sys = (filter_sys_t *)sys;

    p_filter->pf_video_filter = to_nv12_filter;
    p_filter->pf_flush = to_nv12_flush;
    return VLC_SUCCESS;

fail:
    CloseConverterToNv12(obj);
    return ret;
}

vlc_module_begin()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("DRMPRIME to s/w"))
    set_description(N_("DRMPRIME-to software picture filter"))
    add_shortcut("drmprime_to_sw")
    set_capability( "video converter", 50 )
    set_callbacks(OpenConverterToNv12, CloseConverterToNv12)
vlc_module_end()

