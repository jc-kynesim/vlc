#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>

#include "../../codec/avcodec/drm_pic.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>

#define TRACE_ALL 1

typedef struct filter_sys_t {
    AVFilterGraph *filter_graph;
    AVFilterContext *buffersink_ctx;  // Allocated within graph - no explicit free
    AVFilterContext *buffersrc_ctx;   // Allocated within graph - no explicit free
    bool has_out;
    AVFrame *out_frame;
} filter_sys_t;

static void drmp_av_flush(filter_t * filter)
{
    // Nothing to do
    VLC_UNUSED(filter);

#if TRACE_ALL
    msg_Dbg(filter, "<<< %s", __func__);
#endif
}

static picture_t * drmp_av_deinterlace(filter_t * filter, picture_t * in_pic)
{
    filter_sys_t *const sys = filter->p_sys;
    AVFrame * frame = av_frame_alloc();
    drm_prime_video_sys_t * const pctx = (drm_prime_video_sys_t *)in_pic->context;
    picture_t * out_pic = NULL;
    picture_t ** pp_pic = &out_pic;
    int ret;

#if TRACE_ALL
    msg_Dbg(filter, "<<< %s", __func__);
#endif

    if (!frame) {
        msg_Err(filter, "Frame alloc failure");
        goto fail;
    }

    frame->format      = AV_PIX_FMT_DRM_PRIME;
    frame->buf[0]      = av_buffer_ref(pctx->buf);
    frame->data[0]     = (uint8_t *)pctx->desc;
    frame->hw_frames_ctx = av_buffer_ref(pctx->hw_frames_ctx);
    frame->width       = in_pic->format.i_width;
    frame->height      = in_pic->format.i_height;
    frame->crop_left   = in_pic->format.i_x_offset;
    frame->crop_top    = in_pic->format.i_y_offset;
    frame->crop_right  = frame->width -  in_pic->format.i_visible_width -  frame->crop_left;
    frame->crop_bottom = frame->height - in_pic->format.i_visible_height - frame->crop_top;
    frame->interlaced_frame = !in_pic->b_progressive;
    frame->top_field_first  = in_pic->b_top_field_first;
    frame->pts         = (in_pic->date == VLC_TS_INVALID) ? AV_NOPTS_VALUE : in_pic->date;

    picture_Release(in_pic);
    in_pic = NULL;

    if ((ret = av_buffersrc_add_frame_flags(sys->buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        msg_Err(filter, "Failed to feed filtergraph: %s", av_err2str(ret));
        goto fail;
    }
    av_frame_unref(frame);

    while (sys->has_out || (ret = av_buffersink_get_frame(sys->buffersink_ctx, sys->out_frame)) == 0) {
        picture_t *const pic = filter_NewPicture(filter);
        sys->has_out = true;
        // Failure to get an output pic happens quite often, just keep the
        // frame for next time
        if (!pic)
            break;

        if (drm_prime_attach_buf_to_pic(pic, sys->out_frame) != VLC_SUCCESS) {
            msg_Err(filter, "Failed to attach frame to out pic");
            picture_Release(pic);
            goto fail;
        }
        pic->date =  sys->out_frame->pts == AV_NOPTS_VALUE ? VLC_TS_INVALID : sys->out_frame->pts;
        av_frame_unref(sys->out_frame);
        sys->has_out = false;

        *pp_pic = pic;
        pp_pic = &pic->p_next;
    }

    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        msg_Err(filter, "Failed to get frame: %s", av_err2str(ret));
        goto fail;
    }

    // Even if we get an error we may have processed some pics and we need to return them
fail:
    if (in_pic)
        picture_Release(in_pic);
    av_frame_free(&frame);

#if TRACE_ALL
    msg_Dbg(filter, ">>> %s: %p", __func__, out_pic);
#endif

    return out_pic;
}

static void CloseDrmpAvDeinterlace(filter_t *filter)
{
    filter_sys_t * const sys = filter->p_sys;

#if TRACE_ALL
    msg_Dbg(filter, "<<< %s", __func__);
#endif

    if (sys == NULL)
        return;

    av_frame_free(&sys->out_frame);
    avfilter_graph_free(&sys->filter_graph);
    free(sys);
}


// Copied almost directly from ffmpeg filtering_video.c example
static int init_filters(filter_t * const filter,
                        const char * const filters_descr)
{
    filter_sys_t *const sys = filter->p_sys;
    const video_format_t * const fmt = &filter->fmt_in.video;
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE };

    sys->out_frame = av_frame_alloc();
    sys->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !sys->filter_graph || !sys->out_frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             fmt->i_visible_width, fmt->i_visible_height, AV_PIX_FMT_DRM_PRIME,
             1, (int)CLOCK_FREQ,
             fmt->i_sar_num, fmt->i_sar_den);

    ret = avfilter_graph_create_filter(&sys->buffersrc_ctx, buffersrc, "in",
                                       args, NULL, sys->filter_graph);
    if (ret < 0) {
        msg_Err(filter, "Cannot create buffer source");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&sys->buffersink_ctx, buffersink, "out",
                                       NULL, NULL, sys->filter_graph);
    if (ret < 0) {
        msg_Err(filter, "Cannot create buffer sink");
        goto end;
    }

    ret = av_opt_set_int_list(sys->buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        msg_Err(filter, "Cannot set output pixel format");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = sys->buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = sys->buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(sys->filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(sys->filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret == 0 ? VLC_SUCCESS : ret == AVERROR(ENOMEM) ? VLC_ENOMEM : VLC_EGENERIC;
}

static bool is_fmt_valid_in(const vlc_fourcc_t fmt)
{
    return fmt == VLC_CODEC_DRM_PRIME_I420 ||
            fmt == VLC_CODEC_DRM_PRIME_NV12 ||
            fmt == VLC_CODEC_DRM_PRIME_SAND8;
}

static int OpenDrmpAvDeinterlace(filter_t *filter)
{
    filter_sys_t *sys;
    int ret;

    msg_Dbg(filter, "<<< %s", __func__);

    if (!is_fmt_valid_in(filter->fmt_in.video.i_chroma) ||
        filter->fmt_out.video.i_chroma != filter->fmt_in.video.i_chroma)
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(filter_sys_t));
    if (!sys)
        return VLC_ENOMEM;
    filter->p_sys = sys;

    if ((ret = init_filters(filter, "deinterlace_v4l2m2m")) != 0)
        goto fail;

    filter->pf_video_filter = drmp_av_deinterlace;
    filter->pf_flush = drmp_av_flush;

    return VLC_SUCCESS;

fail:
    CloseDrmpAvDeinterlace(filter);
    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname(N_("DRM PRIME deinterlace"))
    set_description(N_("libav-based DRM_PRIME deinterlace filter plugin"))
    set_capability("video filter", 902)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_callbacks(OpenDrmpAvDeinterlace, CloseDrmpAvDeinterlace)
    add_shortcut("deinterlace")
vlc_module_end()

