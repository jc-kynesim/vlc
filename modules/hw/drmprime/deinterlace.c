#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

static picture_t *
DrmpDeinterlace(filter_t *filter, picture_t *src)
{
    VLC_UNUSED(filter);
    src->b_progressive = true;
    return src;
}

static void DrmpFlush(filter_t *filter)
{
    VLC_UNUSED(filter);
}

static void DrmpClose(filter_t *filter)
{
    vlc_video_context_Release(filter->vctx_out);
}

static const struct vlc_filter_operations filter_ops = {
    .filter_video = DrmpDeinterlace,
    .close = DrmpClose,
    .flush = DrmpFlush,
};

static int OpenDrmpDeinterlace(filter_t * filter)
{
    if ( filter->vctx_in == NULL ||
        vlc_video_context_GetType(filter->vctx_in) != VLC_VIDEO_CONTEXT_DRM_PRIME)
        return VLC_EGENERIC;
    if (filter->fmt_in.video.i_chroma != VLC_CODEC_DRM_PRIME_I420)
        return VLC_EGENERIC;
    if (!video_format_IsSimilar(&filter->fmt_in.video, &filter->fmt_out.video))
        return VLC_EGENERIC;

    filter->ops = &filter_ops;
    filter->fmt_out.video.i_frame_rate *= 2;
    filter->vctx_out = vlc_video_context_Hold(filter->vctx_in);
    return VLC_SUCCESS;
}

vlc_module_begin()
    set_shortname(N_("DRMPrime deinterlace"))
    set_description(N_("DRMPrime-based deinterlace (NULL) filter plugin"))
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_deinterlace_callback(OpenDrmpDeinterlace)
vlc_module_end()

