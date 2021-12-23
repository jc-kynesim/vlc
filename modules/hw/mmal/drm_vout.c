/*****************************************************************************
 * mmal.c: MMAL-based vout plugin for Raspberry Pi
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 *
 * Authors: Dennis Hamester <dennis.hamester@gmail.com>
 *          Julian Scheel <julian@jusst.de>
 *          John Cox <jc@kynesim.co.uk>
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
#include "config.h"
#endif

#include <errno.h>
#include <pthread.h>

#include "drmu.h"
#include "drmu_vlc.h"

#include <vlc_common.h>

#include <vlc_codec.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_VOUT_SOURCE_MODESET_NAME "drm-vout-source-modeset"
#define DRM_VOUT_SOURCE_MODESET_TEXT N_("Attempt to match display to source")
#define DRM_VOUT_SOURCE_MODESET_LONGTEXT N_("Attempt to match display resolution and refresh rate to source.\
 Defaults to the 'preferred' mode if no good enough match found. \
 If unset then resolution & refresh will not be set.")

#define DRM_VOUT_NO_MODESET_NAME "drm-vout-no-modeset"
#define DRM_VOUT_NO_MODESET_TEXT N_("Do not modeset")
#define DRM_VOUT_NO_MODESET_LONGTEXT N_("Do no operation that would cause a modeset.\
 This overrides the operation of all other flags.")

#define DRM_VOUT_NO_MAX_BPC "drm-vout-no-max-bpc"
#define DRM_VOUT_NO_MAX_BPC_TEXT N_("Do not set bpc on output")
#define DRM_VOUT_NO_MAX_BPC_LONGTEXT N_("Do not try to switch from 8-bit RGB to 12-bit YCC on UHD frames.\
 12 bit is dependant on kernel and display support so may not be availible")


#define TRACE_ALL 0

#define SUBPICS_MAX 4

#define DRM_MODULE "vc4"

typedef struct subpic_ent_s {
    drmu_fb_t * fb;
    drmu_rect_t pos;
    drmu_rect_t space;  // display space of pos
    picture_t * pic;
} subpic_ent_t;

typedef struct vout_display_sys_t {
    vlc_decoder_device *dec_dev;

    drmu_env_t * du;
    drmu_crtc_t * dc;
    drmu_plane_t * dp;
    drmu_pool_t * pic_pool;
    drmu_pool_t * sub_fb_pool;
    drmu_plane_t * subplanes[SUBPICS_MAX];
    subpic_ent_t subpics[SUBPICS_MAX];
    vlc_fourcc_t * subpic_chromas;

    drmu_atomic_t * display_set;

    uint32_t con_id;
    int mode_id;
} vout_display_sys_t;

static drmu_fb_t *
copy_pic_to_fb(vout_display_t *vd, drmu_pool_t * const pool, picture_t * const src)
{
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format);
    drmu_fb_t * fb;
    int i;

    if (drm_fmt == 0) {
        msg_Warn(vd, "Failed drm format copy_pic: %#x", src->format.i_chroma);
        return NULL;
    }

    fb = drmu_pool_fb_new_dumb(pool, src->format.i_width, src->format.i_height, drm_fmt);
    if (fb == NULL) {
        msg_Warn(vd, "Failed alloc for copy_pic: %dx%d", src->format.i_width, src->format.i_height);
        return NULL;
    }

    for (i = 0; i != src->i_planes; ++i) {
        plane_t dst_plane;
        dst_plane = drmu_fb_vlc_plane(fb, i);
        plane_CopyPixels(&dst_plane, src->p + i);
    }

    return fb;
}

static void vd_drm_prepare(vout_display_t *vd, picture_t *pic,
                           subpicture_t *subpicture, vlc_tick_t date)
{
    vout_display_sys_t * const sys = vd->sys;
    unsigned int n = 0;
    drmu_atomic_t * da = drmu_atomic_new(sys->du);;
    drmu_fb_t * dfb = NULL;
    drmu_rect_t r;
    unsigned int i;
    int ret;

    VLC_UNUSED(date);

    if (da == NULL)
        goto fail;

    if (sys->display_set != NULL) {
        msg_Warn(vd, "sys->display_set != NULL");
        drmu_atomic_unref(&sys->display_set);
    }

    // Set mode early so w/h are correct
    drmu_atomic_crtc_mode_id_set(da, sys->dc, sys->mode_id);

    // Attempt to import the subpics
    for (subpicture_t * spic = subpicture; spic != NULL; spic = spic->p_next)
    {
        for (subpicture_region_t *sreg = spic->p_region; sreg != NULL; sreg = sreg->p_next) {
            picture_t * const src = sreg->p_picture;
            subpic_ent_t * const dst = sys->subpics + n;

            // If we've run out of subplanes we could allocate - give up now
            if (!sys->subplanes[n])
                goto subpics_done;

            // If the same picture then assume the same contents
            // We keep a ref to the previous pic to ensure that teh same picture
            // structure doesn't get reused and confuse us.
            if (src != dst->pic) {
                drmu_fb_unref(&dst->fb);
                if (dst->pic != NULL) {
                    picture_Release(dst->pic);
                    dst->pic = NULL;
                }

                dst->fb = copy_pic_to_fb(vd, sys->sub_fb_pool, src);
                if (dst->fb == NULL)
                    continue;

                dst->pic = picture_Hold(src);
            }

            // *** More transform required
            dst->pos = (drmu_rect_t){
                .x = sreg->i_x,
                .y = sreg->i_y,
                .w = src->format.i_visible_width,
                .h = src->format.i_visible_height,
            };

//            msg_Info(vd, "Orig: %dx%d", spic->i_original_picture_width, spic->i_original_picture_height);
            dst->space = drmu_rect_wh(spic->i_original_picture_width, spic->i_original_picture_height);

            if (++n == SUBPICS_MAX)
                goto subpics_done;
        }
    }
subpics_done:

    // Clear any other entries
    for (; n != SUBPICS_MAX; ++n) {
        subpic_ent_t * const dst = sys->subpics + n;
        if (dst->pic != NULL) {
            picture_Release(dst->pic);
            dst->pic = NULL;
        }
        drmu_fb_unref(&dst->fb);
    }

    {
        vout_display_place_t place;
        vout_display_cfg_t cfg = *vd->cfg;

        cfg.display.width  = drmu_crtc_width(sys->dc);
        cfg.display.height = drmu_crtc_height(sys->dc);
        cfg.display.sar    = drmu_ufrac_vlc_to_rational(drmu_crtc_sar(sys->dc));

        vout_display_PlacePicture(&place, &pic->format, &cfg);
        r = drmu_rect_vlc_place(&place);

#if 0
        {
            static int z = 0;
            if (--z < 0) {
                z = 200;
                msg_Info(vd, "Cropped: %d,%d %dx%d %d/%d Cfg: %dx%d %d/%d Display: %dx%d %d/%d Place: %d,%d %dx%d",
                         pic->format.i_x_offset, pic->format.i_y_offset,
                         pic->format.i_visible_width, pic->format.i_visible_height,
                         pic->format.i_sar_num, pic->format.i_sar_den,
                         vd->cfg->display.width,   vd->cfg->display.height,
                         vd->cfg->display.sar.num, vd->cfg->display.sar.den,
                         cfg.display.width,   cfg.display.height,
                         cfg.display.sar.num, cfg.display.sar.den,
                         r.x, r.y, r.w, r.h);
            }
        }
#endif
    }

    if (pic->format.i_chroma != VLC_CODEC_DRM_PRIME_OPAQUE) {
        dfb = copy_pic_to_fb(vd, sys->pic_pool, pic);
    }
    else {
        dfb = drmu_fb_vlc_new_pic_attach(sys->du, pic);
    }

    if (dfb == NULL) {
        msg_Err(vd, "Failed to create frme buffer from pic");
        return;
    }

    ret = drmu_atomic_plane_set(da, sys->dp, dfb, r);
    drmu_fb_unref(&dfb);

    if (ret != 0) {
        msg_Err(vd, "Failed to set video plane: %s", strerror(-ret));
        goto fail;
    }

    for (i = 0; i != SUBPICS_MAX; ++i) {
        subpic_ent_t * const spe = sys->subpics + i;

//        msg_Info(vd, "pic=%dx%d @ %d,%d, r=%dx%d @ %d,%d, space=%dx%d @ %d,%d",
//                 spe->pos.w, spe->pos.h, spe->pos.x, spe->pos.y,
//                 r.w, r.h, r.x, r.y,
//                 spe->space.w, spe->space.h, spe->space.x, spe->space.y);

        // Rescale from sub-space
        if (sys->subplanes[i] &&
            (ret = drmu_atomic_plane_set(da, sys->subplanes[i], spe->fb,
                                  drmu_rect_rescale(spe->pos, r, spe->space))) != 0) {
            msg_Err(vd, "drmModeSetPlane for subplane %d failed: %s", i, strerror(-ret));
        }
    }

    sys->display_set = da;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    return;

fail:
    drmu_fb_unref(&dfb);
    drmu_atomic_unref(&da);
}

static void vd_drm_display(vout_display_t *vd, picture_t *p_pic)
{
    vout_display_sys_t *const sys = vd->sys;
    VLC_UNUSED(p_pic);

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif

    drmu_atomic_queue(&sys->display_set);
    return;
}

static int vd_drm_control(vout_display_t *vd, int query)
{
    int ret = VLC_EGENERIC;

    switch (query) {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        case VOUT_DISPLAY_CHANGE_ZOOM:
            msg_Warn(vd, "Unsupported control query %d", query);
            ret = VLC_SUCCESS;
            break;

        default:
            msg_Warn(vd, "Unknown control query %d", query);
            break;
    }

    return ret;
}

static int vd_drm_reset_pictures(vout_display_t *vd, video_format_t *fmt)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(fmt);
    return VLC_SUCCESS;
}

static void CloseDrmVout(vout_display_t *vd)
{
    vout_display_sys_t *const sys = vd->sys;
    unsigned int i;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif

    drmu_pool_delete(&sys->sub_fb_pool);

    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_plane_delete(sys->subplanes + i);
    for (i = 0; i != SUBPICS_MAX; ++i) {
        if (sys->subpics[i].pic != NULL)
            picture_Release(sys->subpics[i].pic);
        drmu_fb_unref(&sys->subpics[i].fb);
    }

    drmu_plane_delete(&sys->dp);
    drmu_crtc_delete(&sys->dc);
    drmu_env_delete(&sys->du);

    if (sys->dec_dev)
        vlc_decoder_device_Release(sys->dec_dev);

    free(sys->subpic_chromas);
    vd->info.subpicture_chromas = NULL;

    free(sys);
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}

static const struct vlc_display_operations ops = {
    .close =            CloseDrmVout,
    .prepare =          vd_drm_prepare,
    .display =          vd_drm_display,
    .control =          vd_drm_control,
    .reset_pictures =   vd_drm_reset_pictures,
    .set_viewpoint =    NULL,
};

// VLC will take a list of subpic formats but it then ignores the fact it is a
// list and picks the 1st one whether it is 'best' or indeed whether or not it
// can use it.  So we have to sort ourselves & have checked usablity.
// Higher number, higher priority. 0 == Do not use.
static int subpic_fourcc_usability(const vlc_fourcc_t fcc)
{
    switch (fcc) {
        case VLC_CODEC_ARGB:
            return 22;
        case VLC_CODEC_RGBA:
            return 21;
        case VLC_CODEC_BGRA:
            return 20;
        case VLC_CODEC_YUVA:
            return 40;
        default:
            break;
    }
    return 0;
}

// Sort in descending priority number
static int subpic_fourcc_sort_cb(const void * a, const void * b)
{
    return subpic_fourcc_usability(*(vlc_fourcc_t *)b) - subpic_fourcc_usability(*(vlc_fourcc_t *)a);
}

static vlc_fourcc_t *
subpic_make_chromas_from_drm(const uint32_t * const drm_chromas, const unsigned int n)
{
    vlc_fourcc_t * const c = (n == 0) ? NULL : calloc(n + 1, sizeof(*c));
    vlc_fourcc_t * p = c;

    if (c == NULL)
        return NULL;

    for (unsigned int j = 0; j != n; ++j) {
        if ((*p = drmu_format_vlc_to_vlc(drm_chromas[j])) != 0)
            ++p;
    }

    // Sort for our preferred order & remove any that would confuse VLC
    qsort(c, p - c, sizeof(*c), subpic_fourcc_sort_cb);
    while (p != c) {
        if (subpic_fourcc_usability(p[-1]) != 0)
            break;
        *--p = 0;
    }

    if (p == c) {
        free(c);
        return NULL;
    }

    return c;
}

static int
mode_pick_cb(void * v, const drmModeModeInfo * mode)
{
    const video_format_t * const fmt = v;

    const int pref = (mode->type & DRM_MODE_TYPE_PREFERRED) != 0;
    const unsigned int r_m = (uint32_t)(((uint64_t)mode->clock * 1000000) / (mode->htotal * mode->vtotal));
    const unsigned int r_f = fmt->i_frame_rate_base == 0 ? 0 :
        (uint32_t)(((uint64_t)fmt->i_frame_rate * 1000) / fmt->i_frame_rate_base);

    printf("Fmt %dx%d @ %d, Mode %dx%d @ %d/%d flags %#x, pref %d\n",
           fmt->i_visible_width, fmt->i_visible_height, r_f,
           mode->hdisplay, mode->vdisplay, mode->vrefresh, r_m, mode->flags, pref);

    // We don't understand interlace
    if ((mode->flags & DRM_MODE_FLAG_INTERLACE) != 0)
        return -1;

    if (fmt->i_visible_width == mode->hdisplay &&
        fmt->i_visible_height == mode->vdisplay)
    {
        // Prefer a good match to 29.97 / 30 but allow the other
        if ((r_m + 10 >= r_f && r_m <= r_f + 10))
            return 100;
        if ((r_m + 100 >= r_f && r_m <= r_f + 100))
            return 95;
        // Double isn't bad
        if ((r_m + 10 >= r_f * 2 && r_m <= r_f * 2 + 10))
            return 90;
        if ((r_m + 100 >= r_f * 2 && r_m <= r_f * 2 + 100))
            return 85;
    }

    if (pref)
        return 50;

    return -1;
}

static int OpenDrmVout(vout_display_t *vd,
                        video_format_t *fmtp, vlc_video_context *vctx)
{
    vout_display_sys_t *sys;
    int ret = VLC_EGENERIC;

    msg_Info(vd, "<<< %s: Fmt=%4.4s, fmtp_chroma=%4.4s", __func__,
             (const char *)&vd->fmt->i_chroma, (const char *)&fmtp->i_chroma);

    sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    vd->sys = sys;

    sys->mode_id = -1;

    if (vctx) {
        sys->dec_dev = vlc_video_context_HoldDevice(vctx);
        if (sys->dec_dev && sys->dec_dev->type != VLC_DECODER_DEVICE_DRM_PRIME) {
            vlc_decoder_device_Release(sys->dec_dev);
            sys->dec_dev = NULL;
        }
    }

    if (sys->dec_dev == NULL)
        sys->dec_dev = vlc_decoder_device_Create(VLC_OBJECT(vd), vd->cfg->window);
    if (sys->dec_dev == NULL || sys->dec_dev->type != VLC_DECODER_DEVICE_DRM_PRIME) {
        msg_Err(vd, "Missing decoder device");
        goto fail;
    }

    ;

    if ((sys->du = drmu_env_new_xlease(VLC_OBJECT(vd))) == NULL &&
        (sys->du = drmu_env_new_open(VLC_OBJECT(vd), DRM_MODULE)) == NULL)
        goto fail;

    drmu_env_modeset_allow(sys->du, !var_InheritBool(vd, DRM_VOUT_NO_MODESET_NAME));

    if ((sys->dc = drmu_crtc_new_find(sys->du)) == NULL)
        goto fail;

    drmu_crtc_max_bpc_allow(sys->dc, !var_InheritBool(vd, DRM_VOUT_NO_MAX_BPC));

    if ((sys->sub_fb_pool = drmu_pool_new(sys->du, 10)) == NULL)
        goto fail;
    if ((sys->pic_pool = drmu_pool_new(sys->du, 5)) == NULL)
        goto fail;

    // **** Plane selection needs noticable improvement
    // This wants to be the primary
    if ((sys->dp = drmu_plane_new_find(sys->dc, DRM_FORMAT_NV12)) == NULL)
        goto fail;

    for (unsigned int i = 0; i != SUBPICS_MAX; ++i) {
        if ((sys->subplanes[i] = drmu_plane_new_find(sys->dc, DRM_FORMAT_ARGB8888)) == NULL) {
            msg_Warn(vd, "Cannot allocate subplane %d", i);
            break;
        }
        if (sys->subpic_chromas == NULL) {
            unsigned int n;
            const uint32_t * const drm_chromas = drmu_plane_formats(sys->subplanes[i], &n);
            sys->subpic_chromas = subpic_make_chromas_from_drm(drm_chromas, n);
        }
    }
    vd->info = (vout_display_info_t) {
// We can scale but as it stands it looks like VLC is confused about coord
// systems s.t. system messages are in display space and subs are in source
// with no way of distinguishing so we don't know what to scale by :-(
//        .can_scale_spu = true,
        .subpicture_chromas = sys->subpic_chromas
    };

    vd->ops = &ops;

    if (!var_InheritBool(vd, DRM_VOUT_SOURCE_MODESET_NAME)) {
        sys->mode_id = -1;
    }
    else {
        sys->mode_id = drmu_crtc_mode_pick(sys->dc, mode_pick_cb, fmtp);

        msg_Dbg(vd, "Mode id=%d", sys->mode_id);

        // This will set the mode on the crtc var but won't actually change the output
        if (sys->mode_id >= 0) {
            drmu_atomic_t * da = drmu_atomic_new(sys->du);
            if (da != NULL) {
                drmu_atomic_crtc_mode_id_set(da, sys->dc, sys->mode_id);
                drmu_atomic_unref(&da);
            }
        }
    }

    vout_display_SetSizeAndSar(vd, drmu_crtc_width(sys->dc), drmu_crtc_height(sys->dc),
                               drmu_ufrac_vlc_to_rational(drmu_crtc_sar(sys->dc)));
    return VLC_SUCCESS;

fail:
    CloseDrmVout(vd);
    return ret;
}

vlc_module_begin()
    set_shortname(N_("DRM vout"))
    set_description(N_("DRM vout plugin"))
    add_shortcut("drm_vout")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)

    add_bool(DRM_VOUT_SOURCE_MODESET_NAME, false, DRM_VOUT_SOURCE_MODESET_TEXT, DRM_VOUT_SOURCE_MODESET_LONGTEXT)
    add_bool(DRM_VOUT_NO_MODESET_NAME,     false, DRM_VOUT_NO_MODESET_TEXT, DRM_VOUT_NO_MODESET_LONGTEXT)
    add_bool(DRM_VOUT_NO_MAX_BPC,          false, DRM_VOUT_NO_MAX_BPC_TEXT, DRM_VOUT_NO_MAX_BPC_LONGTEXT)

    set_callback_display(OpenDrmVout, 16)  // 1 point better than ASCII art
vlc_module_end()

