#include "drmu_vlc.h"
#include "drmu_int.h"
#include "../codec/avcodec/drm_pic.h"

#include <libavutil/buffer.h>
#include <libavutil/hwcontext_drm.h>

// N.B. DRM seems to order its format descriptor names the opposite way round to VLC
// DRM is hi->lo within a little-endian word, VLC is byte order

uint32_t
drmu_format_vlc_to_drm(const video_frame_format_t * const vf_vlc)
{
    switch (vf_vlc->i_chroma) {
        case VLC_CODEC_RGB32:
        {
            // VLC RGB32 aka RV32 means we have to look at the mask values
            const uint32_t r = vf_vlc->i_rmask;
            const uint32_t g = vf_vlc->i_gmask;
            const uint32_t b = vf_vlc->i_bmask;
            if (r == 0xff0000 && g == 0xff00 && b == 0xff)
                return DRM_FORMAT_XRGB8888;
            if (r == 0xff && g == 0xff00 && b == 0xff0000)
                return DRM_FORMAT_XBGR8888;
            if (r == 0xff000000 && g == 0xff0000 && b == 0xff00)
                return DRM_FORMAT_RGBX8888;
            if (r == 0xff00 && g == 0xff0000 && b == 0xff000000)
                return DRM_FORMAT_BGRX8888;
            break;
        }
        case VLC_CODEC_RGB16:
        {
            // VLC RGB16 aka RV16 means we have to look at the mask values
            const uint32_t r = vf_vlc->i_rmask;
            const uint32_t g = vf_vlc->i_gmask;
            const uint32_t b = vf_vlc->i_bmask;
            if (r == 0xf800 && g == 0x7e0 && b == 0x1f)
                return DRM_FORMAT_RGB565;
            if (r == 0x1f && g == 0x7e0 && b == 0xf800)
                return DRM_FORMAT_BGR565;
            break;
        }
        case VLC_CODEC_RGBA:
            return DRM_FORMAT_ABGR8888;
        case VLC_CODEC_BGRA:
            return DRM_FORMAT_ARGB8888;
        case VLC_CODEC_ARGB:
            return DRM_FORMAT_BGRA8888;
        // VLC_CODEC_ABGR does not exist in VLC
        case VLC_CODEC_VUYA:
            return DRM_FORMAT_AYUV;
        // AYUV appears to be the only DRM YUVA-like format
        case VLC_CODEC_VYUY:
            return DRM_FORMAT_YUYV;
        case VLC_CODEC_UYVY:
            return DRM_FORMAT_YVYU;
        case VLC_CODEC_YUYV:
            return DRM_FORMAT_VYUY;
        case VLC_CODEC_YVYU:
            return DRM_FORMAT_UYVY;
        case VLC_CODEC_NV12:
            return DRM_FORMAT_NV12;
        case VLC_CODEC_NV21:
            return DRM_FORMAT_NV21;
        case VLC_CODEC_I420:
            return DRM_FORMAT_YUV420;
        default:
            break;
    }
    return 0;
}

vlc_fourcc_t
drmu_format_vlc_to_vlc(const uint32_t vf_drm)
{
    switch (vf_drm) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
            return VLC_CODEC_RGB32;
        case DRM_FORMAT_BGR565:
        case DRM_FORMAT_RGB565:
            return VLC_CODEC_RGB16;
        case DRM_FORMAT_ABGR8888:
            return VLC_CODEC_RGBA;
        case DRM_FORMAT_ARGB8888:
            return VLC_CODEC_BGRA;
        case DRM_FORMAT_BGRA8888:
            return VLC_CODEC_ARGB;
        // VLC_CODEC_ABGR does not exist in VLC
        case DRM_FORMAT_AYUV:
            return VLC_CODEC_VUYA;
        case DRM_FORMAT_YUYV:
            return VLC_CODEC_VYUY;
        case DRM_FORMAT_YVYU:
            return VLC_CODEC_UYVY;
        case DRM_FORMAT_VYUY:
            return VLC_CODEC_YUYV;
        case DRM_FORMAT_UYVY:
            return VLC_CODEC_YVYU;
        case DRM_FORMAT_NV12:
            return VLC_CODEC_NV12;
        case DRM_FORMAT_NV21:
            return VLC_CODEC_NV21;
        case DRM_FORMAT_YUV420:
            return VLC_CODEC_I420;
        default:
            break;
    }
    return 0;
}

typedef struct fb_aux_pic_s {
    picture_t * pic;
} fb_aux_pic_t;

static void
pic_fb_delete_cb(drmu_fb_t * dfb, void * v)
{
    fb_aux_pic_t * const aux = v;
    VLC_UNUSED(dfb);

    picture_Release(aux->pic);
    free(aux);
}

static uint8_t
pic_transfer_to_eotf(const video_transfer_func_t vtf)
{
    switch (vtf) {
        case TRANSFER_FUNC_SMPTE_ST2084:
            return HDMI_EOTF_SMPTE_ST2084;
        case TRANSFER_FUNC_ARIB_B67:
            return HDMI_EOTF_BT_2100_HLG;
        default:
            break;
    }
    // ?? Trad HDR ??
    return HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
}

static struct hdr_output_metadata
pic_hdr_metadata(const struct video_format_t * const fmt)
{
    struct hdr_output_metadata m;
    struct hdr_metadata_infoframe * const inf = &m.hdmi_metadata_type1;
    unsigned int i;

    memset(&m, 0, sizeof(m));
    m.metadata_type = HDMI_STATIC_METADATA_TYPE1;

    inf->eotf = pic_transfer_to_eotf(fmt->transfer);
    inf->metadata_type = HDMI_STATIC_METADATA_TYPE1;

    // VLC & HDMI use the same scales for everything but max_luma
    for (i = 0; i != 3; ++i) {
        inf->display_primaries[i].x = fmt->mastering.primaries[i * 2 + 0];
        inf->display_primaries[i].y = fmt->mastering.primaries[i * 2 + 1];
    }
    inf->white_point.x = fmt->mastering.white_point[0];
    inf->white_point.y = fmt->mastering.white_point[1];
    inf->max_display_mastering_luminance = (uint16_t)(fmt->mastering.max_luminance / 10000);
    inf->min_display_mastering_luminance = (uint16_t)fmt->mastering.min_luminance;

    inf->max_cll = fmt->lighting.MaxCLL;
    inf->max_fall = fmt->lighting.MaxFALL;

    return m;
}


// VLC specific helper fb fns
// *** If we make a lib from the drmu fns this should be separated to avoid
//     unwanted library dependancies - For the general case we will need to
//     think harder about how we split this

static const char *
fb_vlc_color_encoding(const video_format_t * const fmt)
{
    switch (fmt->space)
    {
        case COLOR_SPACE_BT2020:
            return "ITU-R BT.2020 YCbCr";
        case COLOR_SPACE_BT601:
            return "ITU-R BT.601 YCbCr";
        case COLOR_SPACE_BT709:
            return "ITU-R BT.709 YCbCr";
        case COLOR_SPACE_UNDEF:
        default:
            break;
    }

    return (fmt->i_visible_width > 1024 || fmt->i_visible_height > 600) ?
        "ITU-R BT.709 YCbCr" :
        "ITU-R BT.601 YCbCr";
}

static const char *
fb_vlc_color_range(const video_format_t * const fmt)
{
    switch (fmt->color_range)
    {
        case COLOR_RANGE_FULL:
            return "YCbCr full range";
        case COLOR_RANGE_UNDEF:
        case COLOR_RANGE_LIMITED:
        default:
            break;
    }
    return "YCbCr limited range";
}


static const char *
fb_vlc_colorspace(const video_format_t * const fmt)
{
    switch (fmt->space) {
        case COLOR_SPACE_BT2020:
            return "BT2020_RGB";
        default:
            break;
    }
    return "Default";
}

// Create a new fb from a VLC DRM_PRIME picture.
// Picture is held reffed by the fb until the fb is deleted
drmu_fb_t *
drmu_fb_vlc_new_pic_attach(drmu_env_t * const du, picture_t * const pic)
{
    uint64_t modifiers[4] = { 0 };
    uint32_t bo_handles[4] = { 0 };
    int i, j, n;
    drmu_fb_t * const dfb = drmu_fb_int_alloc(du);
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
    fb_aux_pic_t * aux = NULL;

    if (dfb == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }

    if (desc == NULL) {
        drmu_err(du, "%s: Missing descriptor", __func__);
        goto fail;
    }
    if (desc->nb_objects > 4) {
        drmu_err(du, "%s: Bad descriptor", __func__);
        goto fail;
    }

    dfb->format  = desc->layers[0].format;
    dfb->width   = pic->format.i_width;
    dfb->height  = pic->format.i_height;
    dfb->cropped = (drmu_rect_t){
        .x = pic->format.i_x_offset,
        .y = pic->format.i_y_offset,
        .w = pic->format.i_visible_width,
        .h = pic->format.i_visible_height
    };

    dfb->color_encoding = fb_vlc_color_encoding(&pic->format);
    dfb->color_range    = fb_vlc_color_range(&pic->format);
    dfb->colorspace     = fb_vlc_colorspace(&pic->format);

    // Set delete callback & hold this pic
    // Aux attached to dfb immediately so no fail cleanup required
    if ((aux = calloc(1, sizeof(*aux))) == NULL) {
        drmu_err(du, "%s: Aux alloc failure", __func__);
        goto fail;
    }
    aux->pic = picture_Hold(pic);

    dfb->on_delete_v = aux;
    dfb->on_delete_fn = pic_fb_delete_cb;

    for (i = 0; i < desc->nb_objects; ++i)
    {
        if ((dfb->bo_list[i] = drmu_bo_new_fd(du, desc->objects[i].fd)) == NULL)
            goto fail;
    }

    n = 0;
    for (i = 0; i < desc->nb_layers; ++i)
    {
        for (j = 0; j < desc->layers[i].nb_planes; ++j)
        {
            const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
            const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;
            dfb->pitches[n] = p->pitch;
            dfb->offsets[n] = p->offset;
            modifiers[n] = obj->format_modifier;
            bo_handles[n] = dfb->bo_list[p->object_index]->handle;
            ++n;
        }
    }

    if (pic->format.mastering.max_luminance == 0) {
        dfb->hdr_metadata_isset = DRMU_ISSET_NULL;
    }
    else {
        dfb->hdr_metadata_isset = DRMU_ISSET_SET;
        dfb->hdr_metadata = pic_hdr_metadata(&pic->format);
    }

#if 0
    drmu_debug(du, "%dx%d, fmt: %x, boh=%d,%d,%d,%d, pitch=%d,%d,%d,%d,"
           " offset=%d,%d,%d,%d, mod=%llx,%llx,%llx,%llx\n",
           av_frame_cropped_width(frame),
           av_frame_cropped_height(frame),
           desc->layers[0].format,
           bo_handles[0],
           bo_handles[1],
           bo_handles[2],
           bo_handles[3],
           pitches[0],
           pitches[1],
           pitches[2],
           pitches[3],
           offsets[0],
           offsets[1],
           offsets[2],
           offsets[3],
           (long long)modifiers[0],
           (long long)modifiers[1],
           (long long)modifiers[2],
           (long long)modifiers[3]
          );
#endif

    if (drmModeAddFB2WithModifiers(du->fd,
                                   dfb->width, dfb->height, dfb->format,
                                   bo_handles, dfb->pitches, dfb->offsets, modifiers,
                                   &dfb->handle, DRM_MODE_FB_MODIFIERS /** 0 if no mods */) != 0)
    {
        drmu_err(du, "drmModeAddFB2WithModifiers failed: %s", strerror(errno));
        goto fail;
    }

    return dfb;

fail:
    drmu_fb_int_free(dfb);
    return NULL;
}

plane_t
drmu_fb_vlc_plane(drmu_fb_t * const dfb, const unsigned int plane_n)
{
    const unsigned int bpp = drmu_fb_pixel_bits(dfb);
    unsigned int hdiv = 1;
    unsigned int wdiv = 1;

    if (plane_n > 4 || dfb->pitches[plane_n] == 0) {
        return (plane_t) {.p_pixels = NULL };
    }

    // Slightly kludgy derivation of height & width divs
    if (plane_n > 0) {
        wdiv = dfb->pitches[0] / dfb->pitches[plane_n];
        hdiv = 2;
    }

    return (plane_t){
        .p_pixels = (uint8_t *)dfb->map_ptr + dfb->offsets[plane_n],
        .i_lines = dfb->height / hdiv,
        .i_pitch = dfb->pitches[plane_n],
        .i_pixel_pitch = bpp / 8,
        .i_visible_lines = dfb->cropped.h / hdiv,
        .i_visible_pitch = (dfb->cropped.w * bpp / 8) / wdiv
    };
}

