/*****************************************************************************
 * drm_vout.c: DRM based output device
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
#include "drmu_dmabuf.h"
#include "drmu_fmts.h"
#include "drmu_log.h"
#include "drmu_output.h"
#include "drmu_pool.h"
#include "drmu_scan.h"
#include "drmu_util.h"
#include "drmu_vlc.h"

#include <vlc_common.h>

#include <vlc_codec.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>

#define TRACE_ALL 0
#define PIC_POOL_FB 1

#define SUBPICS_MAX 4

#define DRM_MODULE "vc4"


#define DRM_VOUT_SOURCE_MODESET_NAME "drm-vout-source-modeset"
#define DRM_VOUT_SOURCE_MODESET_TEXT N_("Attempt to match display to source")
#define DRM_VOUT_SOURCE_MODESET_LONGTEXT N_("Attempt to match display resolution and refresh rate to source.\
 Defaults to the 'preferred' mode if no good enough match found. \
 If unset then resolution & refresh will not be set.")

#define DRM_VOUT_MODE_NAME "drm-vout-mode"
#define DRM_VOUT_MODE_TEXT N_("Set this mode for display")
#define DRM_VOUT_MODE_LONGTEXT N_("arg: <w>x<h>@<hz> Force mode to arg")

#define DRM_VOUT_NO_MODESET_NAME "drm-vout-no-modeset"
#define DRM_VOUT_NO_MODESET_TEXT N_("Do not modeset")
#define DRM_VOUT_NO_MODESET_LONGTEXT N_("Do no operation that would cause a modeset.\
 This overrides the operation of all other flags.")

#define DRM_VOUT_NO_MAX_BPC "drm-vout-no-max-bpc"
#define DRM_VOUT_NO_MAX_BPC_TEXT N_("Do not set bpc on output")
#define DRM_VOUT_NO_MAX_BPC_LONGTEXT N_("Do not try to switch from 8-bit RGB to 12-bit YCC on UHD frames.\
 12 bit is dependant on kernel and display support so may not be availible")

#define DRM_VOUT_WINDOW_NAME "drm-vout-window"
#define DRM_VOUT_WINDOW_TEXT N_("Display window for Rpi fullscreen")
#define DRM_VOUT_WINDOW_LONGTEXT N_("Display window for Rpi fullscreen."\
"fullscreen|<width>x<height>+<x>+<y>")

#define DRM_VOUT_DISPLAY_NAME "drm-vout-display"
#define DRM_VOUT_DISPLAY_TEXT N_("Output device for Rpi fullscreen.")
#define DRM_VOUT_DISPLAY_LONGTEXT N_("Output device for Rpi fullscreen. " \
"Valid values are HDMI-1,HDMI-2 or a drm connector name.  By default if qt-fullscreen-screennumber " \
"is specified (or set by Fullscreen Output Device in Preferences) " \
"HDMI-<qt-fullscreen-screennumber+1> will be used, otherwise HDMI-1.")

#define DRM_VOUT_MODULE_NAME "drm-vout-module"
#define DRM_VOUT_MODULE_TEXT N_("DRM module to use")
#define DRM_VOUT_MODULE_LONGTEXT N_("DRM module for Rpi fullscreen")

#define DRM_VOUT_POOL_DMABUF_NAME "drm-vout-pool-dmabuf"
#define DRM_VOUT_POOL_DMABUF_TEXT N_("Use dmabufs for pic pool")
#define DRM_VOUT_POOL_DMABUF_LONGTEXT N_("Use dmabufs for pic pool. Saves a frame copy on output but may use up limited dmabuf resource.")


typedef struct subpic_ent_s {
    drmu_fb_t * fb;
    drmu_rect_t pos;
    drmu_rect_t space;  // display space of pos
    picture_t * pic;
    unsigned int alpha; // out of 0xff * 0xff
} subpic_ent_t;

typedef struct vout_display_sys_t {
    drmu_env_t * du;
    drmu_output_t * dout;
    drmu_plane_t * dp;
    drmu_pool_t * pic_pool;
    drmu_pool_t * sub_fb_pool;
    drmu_plane_t * subplanes[SUBPICS_MAX];
    subpic_ent_t subpics[SUBPICS_MAX];
    vlc_fourcc_t * subpic_chromas;

    drmu_atomic_t * display_set;

    vout_display_place_t req_win;
    vout_display_place_t spu_rect;
    vout_display_place_t dest_rect;
    vout_display_place_t win_rect;
    vout_display_place_t display_rect;

    video_transform_t display_transform;
    video_transform_t video_transform;
    video_transform_t dest_transform;

    bool pool_try_fb;
    bool pool_is_fb;
    bool output_simple;
    uint32_t con_id;
    int mode_id;

    picture_pool_t * vlc_pic_pool;
} vout_display_sys_t;

#define PIC_SYS_SIG VLC_FOURCC('D', 'R', 'M', 'U')


// pic->p_sys when we are allocating our own pics
struct picture_sys_t {
    uint32_t sig;
    drmu_fb_t * fb;
};

static drmu_fb_t *
copy_pic_to_fb(vout_display_t *const vd, drmu_pool_t *const pool, picture_t *const src)
{
    uint64_t mod;
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format, &mod);
    drmu_fb_t * fb;
    int i;

    if (drm_fmt == 0 || mod != DRM_FORMAT_MOD_LINEAR) {
        msg_Warn(vd, "Failed vlc->drm format for copy_pic: %s", drmu_log_fourcc(src->format.i_chroma));
        return NULL;
    }

    fb = drmu_pool_fb_new(pool, src->format.i_width, src->format.i_height, drm_fmt, mod);
    if (fb == NULL) {
        msg_Warn(vd, "Failed alloc for copy_pic: %dx%d", src->format.i_width, src->format.i_height);
        return NULL;
    }

    drmu_fb_write_start(fb);
    for (i = 0; i != src->i_planes; ++i) {
        plane_t dst_plane;
        dst_plane = drmu_fb_vlc_plane(fb, i);
        plane_CopyPixels(&dst_plane, src->p + i);
    }
    drmu_fb_write_end(fb);

    drmu_fb_vlc_pic_set_metadata(fb, src);

    return fb;
}

static void
create_box(drmu_fb_t * const fb, const unsigned int layer_no)
{
    const drmu_fmt_info_t *const f = drmu_fb_format_info_get(fb);
    unsigned int hdiv = drmu_fmt_info_hdiv(f, layer_no);
    unsigned int wdiv = drmu_fmt_info_wdiv(f, layer_no);
    const unsigned int pby = (drmu_fmt_info_pixel_bits(f) + 7) / 8;
    const uint32_t pitch_n = drmu_fb_pitch(fb, layer_no);
    const drmu_rect_t crop = drmu_rect_shr16_rnd(drmu_fb_crop_frac(fb));
    const drmu_rect_t active = drmu_fb_active(fb);

    uint8_t * const p0 = drmu_fb_data(fb, layer_no);
    uint8_t * p1 = p0;
    uint8_t * p2;

    // Assumes RGB as currently no better idea (and it is RGB in the case we expect)
    const unsigned int c = 0;

    // Top
    p2 = p1 + pitch_n * (crop.y / hdiv) + (crop.x / wdiv) * pby;
    if (p1 != p2)
        memset(p1, c, p2 - p1);
    if (active.w == crop.w) {
        p1 = p2 + (crop.h / hdiv) * pitch_n; // We expect crop.x == 0
    }
    else {
        unsigned int i;
        const unsigned int vis_pitch = (crop.w / wdiv) * pby;
        for (i = 1; i < (crop.h / hdiv); ++i) {
            p1 = p2 + vis_pitch;
            p2 = p2 + pitch_n;
            memset(p1, c, p2 - p1);
        }
        p1 = p2 + vis_pitch;
    }
    p2 = p0 + pitch_n * (active.h / hdiv);
    if (p1 != p2)
        memset(p1, c, p2 - p1);
}

static drmu_fb_t *
copy_pic_to_fixed_fb(vout_display_t * const vd, vout_display_sys_t * const sys,
                     drmu_pool_t *const pool, picture_t *const src)
{
    uint64_t mod;
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format, &mod);
    drmu_fb_t * fb;
    int i;

    if (drm_fmt == 0 || mod != DRM_FORMAT_MOD_LINEAR) {
        msg_Warn(vd, "Failed vlc->drm format for copy_pic: %s", drmu_log_fourcc(src->format.i_chroma));
        return NULL;
    }

    fb = drmu_pool_fb_new(pool, sys->display_rect.width, sys->display_rect.height, drm_fmt, mod);
    if (fb == NULL) {
        msg_Warn(vd, "Failed alloc for copy_pic_fixed: %dx%d", sys->display_rect.width, sys->display_rect.height);
        return NULL;
    }

    drmu_fb_crop_frac_set(fb, drmu_rect_shl16(drmu_rect_vlc_place(&sys->dest_rect)));

    {
        const drmu_fmt_info_t *const f = drmu_fb_format_info_get(fb);
        const drmu_rect_t crop = drmu_rect_shr16_rnd(drmu_fb_crop_frac(fb));
        const unsigned int bypp = (drmu_fmt_info_pixel_bits(f) + 7) / 8;

        drmu_fb_write_start(fb);
        for (i = 0; i != src->i_planes; ++i) {
            // It would seem more logical to use src->format than to use vd->fmt
            // for the source rect but src->fmt doesn't have offset_x/y set (bug?)
            drmu_memcpy_rect(drmu_fb_data(fb, i), drmu_fb_pitch(fb, i),
                             drmu_rect_div_xy(crop, drmu_fmt_info_wdiv(f, i), drmu_fmt_info_hdiv(f, i)),
                             src->p[i].p_pixels, src->p[i].i_pitch,
                             drmu_rect_vlc_format_crop(&vd->fmt),
                             bypp);
            create_box(fb, i);
        }
        drmu_fb_write_end(fb);
    }

    // Reset crop for display after we've used it for copy
    drmu_fb_crop_frac_set(fb, drmu_rect_shl16(drmu_fb_active(fb)));

    drmu_fb_vlc_pic_set_metadata(fb, src);

    return fb;
}


static vout_display_place_t str_to_rect(const char * s)
{
    vout_display_place_t rect = {0};
    rect.width = strtoul(s, (char**)&s, 0);
    if (*s == '\0')
        return rect;
    if (*s++ != 'x')
        goto fail;
    rect.height = strtoul(s, (char**)&s, 0);
    if (*s == '\0')
        return rect;
    if (*s++ != '+')
        goto fail;
    rect.x = strtoul(s, (char**)&s, 0);
    if (*s == '\0')
        return rect;
    if (*s++ != '+')
        goto fail;
    rect.y = strtoul(s, (char**)&s, 0);
    if (*s != '\0')
        goto fail;
    return rect;

fail:
    return (vout_display_place_t){0,0,0,0};
}

// MMAL headers comment these (getting 2 a bit wrong) but do not give
// defines
#define VXF_H_SHIFT 0  // Hflip
#define VXF_V_SHIFT 1  // Vflip
#define VXF_T_SHIFT 2  // Transpose
#define VXF_H_BIT   (1 << VXF_H_SHIFT)
#define VXF_V_BIT   (1 << VXF_V_SHIFT)
#define VXF_T_BIT   (1 << VXF_T_SHIFT)

static inline bool
is_vxf_transpose(const video_transform_t t)
{
    return ((unsigned int)t & VXF_T_BIT) != 0;
}

static inline bool
is_vxf_hflip(const video_transform_t t)
{
    return ((unsigned int)t & VXF_H_BIT) != 0;
}

static inline bool
is_vxf_vflip(const video_transform_t t)
{
    return ((unsigned int)t & VXF_V_BIT) != 0;
}

static inline video_transform_t
swap_vxf_hv(const video_transform_t x)
{
    return (((x >> VXF_H_SHIFT) & 1) << VXF_V_SHIFT) |
           (((x >> VXF_V_SHIFT) & 1) << VXF_H_SHIFT) |
           (x & VXF_T_BIT);
}

static inline video_transform_t
vxf_inverse(const video_transform_t x)
{
    return is_vxf_transpose(x) ? swap_vxf_hv(x) : x;
}

// Transform generated by A then B
// All ops are self inverse so can simply be XORed on their own
// H & V flips after a transpose need to be swapped
static inline video_transform_t
combine_vxf(const video_transform_t a, const video_transform_t b)
{
    return a ^ (is_vxf_transpose(a) ? swap_vxf_hv(b) : b);
}

static inline vout_display_place_t
vplace_transpose(const vout_display_place_t s)
{
    return (vout_display_place_t){
        .x      = s.y,
        .y      = s.x,
        .width  = s.height,
        .height = s.width
    };
}

// hflip s in c
static inline vout_display_place_t vplace_hflip(const vout_display_place_t s, const vout_display_place_t c)
{
    return (vout_display_place_t){
        .x = c.x + (c.x + c.width) - (s.x + s.width),
        .y = s.y,
        .width = s.width,
        .height = s.height
    };
}

// vflip s in c
static inline vout_display_place_t vplace_vflip(const vout_display_place_t s, const vout_display_place_t c)
{
    return (vout_display_place_t){
        .x = s.x,
        .y = (c.y + c.height) - (s.y - c.y) - s.height,
        .width = s.width,
        .height = s.height
    };
}

static vout_display_place_t
place_out(const vout_display_cfg_t * cfg,
          const video_format_t * fmt,
          const vout_display_place_t r)
{
    video_format_t tfmt;
    vout_display_cfg_t tcfg;
    vout_display_place_t place;

    // Fix SAR if unknown
    if (fmt->i_sar_den == 0 || fmt->i_sar_num == 0) {
        tfmt = *fmt;
        tfmt.i_sar_den = 1;
        tfmt.i_sar_num = 1;
        fmt = &tfmt;
    }

    // Override what VLC thinks might be going on with display size
    // if we know better
    if (r.width != 0 && r.height != 0)
    {
        tcfg = *cfg;
        tcfg.display.width = r.width;
        tcfg.display.height = r.height;
        cfg = &tcfg;
    }

    vout_display_PlacePicture(&place, fmt, cfg, false);

    place.x += r.x;
    place.y += r.y;

    return place;
}

static vout_display_place_t
rect_transform(vout_display_place_t s, const vout_display_place_t c, const video_transform_t t)
{
    if (is_vxf_transpose(t))
        s = vplace_transpose(s);
    if (is_vxf_hflip(t))
        s = vplace_hflip(s, c);
    if (is_vxf_vflip(t) != 0)
        s = vplace_vflip(s, c);
    return s;
}

static void
place_dest_rect(vout_display_sys_t * const sys,
          const vout_display_cfg_t * const cfg,
          const video_format_t * fmt)
{
    sys->dest_rect = rect_transform(place_out(cfg, fmt, sys->win_rect),
                                    sys->display_rect, sys->dest_transform);
}

static void
place_spu_rect(vout_display_sys_t * const sys,
               const vout_display_cfg_t * const cfg,
               const video_format_t * fmt)
{
    static const vout_display_place_t r0 = {0};

    sys->spu_rect = place_out(cfg, fmt, r0);
    sys->spu_rect.x = 0;
    sys->spu_rect.y = 0;

    // Copy place override logic for spu pos from video_output.c
    // This info doesn't appear to reside anywhere natively

    if (fmt->i_width * fmt->i_height >= (unsigned int)(sys->spu_rect.width * sys->spu_rect.height)) {
        sys->spu_rect.width  = fmt->i_visible_width;
        sys->spu_rect.height = fmt->i_visible_height;
    }

    if (ORIENT_IS_SWAP(fmt->orientation))
        sys->spu_rect = vplace_transpose(sys->spu_rect);
}

static void
place_rects(vout_display_sys_t * const sys,
            const vout_display_cfg_t * const cfg,
            const video_format_t * fmt)
{
    place_dest_rect(sys, cfg, fmt);
    place_spu_rect(sys, cfg, fmt);
}

static int configure_display(const vout_display_t *vd, vout_display_sys_t *const sys,
                             const vout_display_cfg_t *cfg, const video_format_t *fmt)
{
    if (!cfg && !fmt)
    {
        msg_Err(vd, "%s: Missing cfg & fmt", __func__);
        return -EINVAL;
    }

    if (!fmt)
        fmt = &vd->source;

    if (!cfg)
        cfg = vd->cfg;

    sys->video_transform = combine_vxf((video_transform_t)fmt->orientation, sys->display_transform);

    place_rects(sys, cfg, fmt);
    return 0;
}

static void set_display_windows(vout_display_t *const vd, vout_display_sys_t *const sys)
{
    const drmu_mode_simple_params_t * const mode = drmu_output_mode_simple_params(sys->dout);
    VLC_UNUSED(vd);

    sys->display_rect = (vout_display_place_t) {0, 0, mode->width, mode->height};

    sys->win_rect = (sys->req_win.width != 0) ?
            sys->req_win :
         is_vxf_transpose(sys->display_transform) ?
            vplace_transpose(sys->display_rect) : sys->display_rect;
}

static void vd_drm_prepare(vout_display_t *vd, picture_t *pic,
                       subpicture_t *subpicture)
{
    vout_display_sys_t * const sys = vd->sys;
    unsigned int n = 0;
    drmu_atomic_t * da = drmu_atomic_new(sys->du);
    drmu_fb_t * dfb = NULL;
    drmu_rect_t r;
    unsigned int i;
    int ret;

    if (da == NULL)
        goto fail;

    if (sys->display_set != NULL) {
        msg_Warn(vd, "sys->display_set != NULL");
        drmu_atomic_unref(&sys->display_set);
    }

    // * Mode (currently) doesn't change whilst running so no need to set here

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
            // We keep a ref to the previous pic to ensure that the same picture
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
                drmu_fb_pixel_blend_mode_set(dst->fb, DRMU_FB_PIXEL_BLEND_COVERAGE);

                dst->pic = picture_Hold(src);
            }
            drmu_fb_crop_frac_set(dst->fb, drmu_rect_shl16(drmu_rect_vlc_format_crop(&sreg->fmt)));

            // *** More transform required
            dst->pos = (drmu_rect_t){
                .x = sreg->i_x,
                .y = sreg->i_y,
                .w = sreg->fmt.i_visible_width,
                .h = sreg->fmt.i_visible_height,
            };
            dst->alpha = spic->i_alpha * sreg->i_alpha;

//            msg_Info(vd, "Orig: %dx%d, (%d,%d) %dx%d; offset %d,%d", spic->i_original_picture_width, spic->i_original_picture_height,
//                     sreg->i_x, sreg->i_y, src->format.i_visible_width, src->format.i_visible_height,
//                     sreg->fmt.i_x_offset, sreg->fmt.i_y_offset);
            dst->space = drmu_rect_vlc_place(&sys->spu_rect);

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

    r = sys->output_simple ? drmu_rect_vlc_place(&sys->display_rect): drmu_rect_vlc_place(&sys->dest_rect);

#if 0
    {
        static int z = 0;
        if (--z < 0) {
            z = 200;
            msg_Info(vd, "Pic: %d,%d %dx%d/%dx%d %d/%d Fmt: %d,%d %dx%d/%dx%d %d/%d Src: %d,%d %dx%d/%dx%d %d/%d Display: %dx%d %d/%d Place: %d,%d %dx%d",
                     pic->format.i_x_offset, pic->format.i_y_offset,
                     pic->format.i_width, pic->format.i_height,
                     pic->format.i_visible_width, pic->format.i_visible_height,
                     pic->format.i_sar_num, pic->format.i_sar_den,
                     vd->fmt.i_x_offset, vd->fmt.i_y_offset,
                     vd->fmt.i_width, vd->fmt.i_height,
                     vd->fmt.i_visible_width, vd->fmt.i_visible_height,
                     vd->fmt.i_sar_num, vd->fmt.i_sar_den,
                     vd->source.i_x_offset, vd->source.i_y_offset,
                     vd->source.i_width, vd->source.i_height,
                     vd->source.i_visible_width, vd->source.i_visible_height,
                     vd->source.i_sar_num, vd->source.i_sar_den,
                     vd->cfg->display.width,   vd->cfg->display.height,
                     vd->cfg->display.sar.num, vd->cfg->display.sar.den,
                     r.x, r.y, r.w, r.h);
        }
    }
#endif

#if HAS_ZC_CMA
    if (drmu_format_vlc_to_drm_cma(&pic->format, NULL) != 0) {
        dfb = drmu_fb_vlc_new_pic_cma_attach(sys->du, pic);
    }
    else
#endif
#if HAS_DRMPRIME
    if (drmu_format_vlc_to_drm_prime(&pic->format, NULL) != 0) {
        dfb = drmu_fb_vlc_new_pic_attach(sys->du, pic);
    }
    else
#endif

    if (sys->pool_is_fb && pic->p_sys != NULL && pic->p_sys->sig == PIC_SYS_SIG) {
        dfb = drmu_fb_ref(pic->p_sys->fb);
    }
    else if (sys->output_simple) {
        dfb = copy_pic_to_fixed_fb(vd, sys, sys->pic_pool, pic);
    }
    else {
        dfb = copy_pic_to_fb(vd, sys->pic_pool, pic);
    }

    if (dfb == NULL) {
        msg_Err(vd, "Failed to create frme buffer from pic");
        return;
    }
    // * Maybe scale cropping by vd->fmt.i_width/height / vd->source.i_width/height
    //   to get pic coord cropping
    //   Wait until we have a bad test case before doing this as I'm worried
    //   that we may get unexpected w/h mismatches that do unwanted scaling
#if 0
    drmu_fb_crop_frac_set(dfb,
        drmu_rect_rescale(
            drmu_rect_vlc_format_crop(&vd->source),
            drmu_rect_shl16(drmu_rect_wh(vd->fmt.i_width, vd->fmt.i_height)),
            drmu_rect_wh(vd->source.i_width, vd->source.i_height)));
#else
    if (!sys->output_simple)
        drmu_fb_crop_frac_set(dfb, drmu_rect_shl16(drmu_rect_vlc_format_crop(&vd->source)));
#endif
    drmu_output_fb_info_set(sys->dout, dfb);

    ret = drmu_atomic_plane_add_fb(da, sys->dp, dfb, r);
    drmu_atomic_output_add_props(da, sys->dout);
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
        if (sys->subplanes[i])
        {
            if ((ret = drmu_atomic_plane_add_fb(da, sys->subplanes[i], spe->fb,
                                  drmu_rect_rescale(spe->pos, r, spe->space))) != 0) {
                 msg_Err(vd, "drmModeSetPlane for subplane %d failed: %s", i, strerror(-ret));
            }
            drmu_atomic_plane_add_alpha(da, sys->subplanes[i], (spe->alpha * DRMU_PLANE_ALPHA_OPAQUE) / (0xff * 0xff));
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

static void vd_drm_display(vout_display_t *vd, picture_t *p_pic,
                subpicture_t *subpicture)
{
    vout_display_sys_t *const sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif

    drmu_atomic_queue(&sys->display_set);

    if (subpicture)
        subpicture_Delete(subpicture);
    picture_Release(p_pic);
    return;
}

static void
destroy_drmu_pic(picture_t * pic)
{
    drmu_fb_unref(&pic->p_sys->fb);
    free(pic->p_sys);
    free(pic);
}

static picture_t *
alloc_drmu_pic(vout_display_t * const vd, drmu_pool_t *const pool)
{
    const video_format_t * const fmt = &vd->fmt;
    uint64_t mod;
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(fmt, &mod);
    const drmu_fmt_info_t * fmti;
    drmu_fb_t * fb;
    unsigned int layers;
    unsigned int i;
    picture_t * pic;
    picture_resource_t res = {
        .p_sys = NULL,
        .pf_destroy = destroy_drmu_pic,
    };

    if (drm_fmt == 0 || mod != DRM_FORMAT_MOD_LINEAR) {
        msg_Warn(vd, "Failed vlc->drm format for copy_pic: %s", drmu_log_fourcc(fmt->i_chroma));
        return NULL;
    }

    fb = drmu_pool_fb_new(pool, fmt->i_width, fmt->i_height, drm_fmt, mod);
    if (fb == NULL) {
        msg_Warn(vd, "Failed alloc for copy_pic: %dx%d", fmt->i_width, fmt->i_height);
        return NULL;
    }

    if ((res.p_sys = calloc(1, sizeof(*res.p_sys))) == NULL)
        goto fail;

    res.p_sys->sig = PIC_SYS_SIG;
    res.p_sys->fb = fb;

    fmti = drmu_fb_format_info_get(fb);
    layers = drmu_fmt_info_plane_count(fmti);

    for (i = 0; i != layers; ++i) {
        res.p[i].p_pixels = drmu_fb_data(fb, i);
        res.p[i].i_lines = drmu_fb_height(fb) / drmu_fmt_info_hdiv(fmti, i);
        res.p[i].i_pitch = drmu_fb_pitch(fb, i);
    }

    if ((pic = picture_NewFromResource(fmt, &res)) == NULL)
        goto fail;

    return pic;

fail:
    drmu_fb_unref(&fb);
    free(res.p_sys);
    return NULL;
}

static void subpic_cache_flush(vout_display_sys_t * const sys)
{
    for (unsigned int i = 0; i != SUBPICS_MAX; ++i) {
        if (sys->subpics[i].pic != NULL) {
            picture_Release(sys->subpics[i].pic);
            sys->subpics[i].pic = NULL;
        }
        drmu_fb_unref(&sys->subpics[i].fb);
    }
}

static void kill_pool(vout_display_sys_t * const sys)
{
    // Drop all cached subpics
    subpic_cache_flush(sys);

    if (sys->vlc_pic_pool != NULL) {
        picture_pool_Release(sys->vlc_pic_pool);
        sys->vlc_pic_pool = NULL;
    }
}

static picture_pool_t *
make_fb_pool(vout_display_t * const vd, vout_display_sys_t * const sys, const unsigned int count)
{
    picture_t * pics[40];
    unsigned int pics_alloc;
    picture_pool_t * pool;

    if (count > ARRAY_SIZE(pics))
        return NULL;

    for (pics_alloc = 0; pics_alloc != count; ++pics_alloc) {
        if ((pics[pics_alloc] = alloc_drmu_pic(vd, sys->pic_pool)) == NULL) {
            msg_Err(vd, "Failed to alloc pic pool entry %u", pics_alloc);
            goto fail;
        }
    }

    if ((pool = picture_pool_New(pics_alloc, pics)) == NULL) {
        msg_Err(vd, "Failed to alloc picture pool");
        goto fail;
    }

    return pool;

fail:
    while (pics_alloc != 0)
        picture_Release(pics[--pics_alloc]);
    return NULL;
}

// Actual picture pool for MMAL opaques is just a set of trivial containers
static picture_pool_t *vd_drm_pool(vout_display_t *vd, unsigned count)
{
    vout_display_sys_t * const sys = vd->sys;

    msg_Dbg(vd, "%s: fmt:%dx%d,sar:%d/%d; source:%dx%d, count=%d", __func__,
            vd->fmt.i_width, vd->fmt.i_height, vd->fmt.i_sar_num, vd->fmt.i_sar_den,
            vd->source.i_width, vd->source.i_height, count);

    if (sys->vlc_pic_pool != NULL) {
        msg_Dbg(vd, "Pool exists");
        return sys->vlc_pic_pool;
    }

    if (sys->pool_try_fb && drmu_format_vlc_to_drm_prime(&vd->fmt, NULL) == 0) {
        if ((sys->vlc_pic_pool = make_fb_pool(vd, sys, count)) != NULL) {
            msg_Dbg(vd, "Pool allocated using dmabufs");
            return sys->vlc_pic_pool;
        }
        msg_Warn(vd, "Pool failed dmabuf allocation");
    }

    msg_Dbg(vd, "Pool allocation from main memory");
    sys->vlc_pic_pool = picture_pool_NewFromFormat(&vd->fmt, count);
    return sys->vlc_pic_pool;
}

static const drmu_vlc_fmt_info_t *
find_fmt_fallback(const vout_display_t * const vd, const vout_display_sys_t * const sys, const vlc_fourcc_t * fallback)
{
    VLC_UNUSED(vd);

    for (; *fallback; ++fallback) {
        const video_frame_format_t vf = {.i_chroma = *fallback};
        const drmu_vlc_fmt_info_t * fi;

        for (fi = drmu_vlc_fmt_info_find_vlc(&vf);
             fi != NULL;
             fi = drmu_vlc_fmt_info_find_vlc_next(&vf, fi))
        {
            if (drmu_plane_format_check(sys->dp, drmu_vlc_fmt_info_drm_pixelformat(fi), drmu_vlc_fmt_info_drm_modifier(fi)))
                return fi;
        }
    }
    return NULL;
}


// Adjust *fmtp to fix format for display (tweak chroma)
static int
set_format(const vout_display_t * const vd, vout_display_sys_t * const sys, video_format_t *const fmtp)
{
    const drmu_vlc_fmt_info_t * fi = drmu_vlc_fmt_info_find_vlc(fmtp);
    const uint64_t drm_mod = drmu_vlc_fmt_info_drm_modifier(fi);
    const uint32_t drm_fmt = drmu_vlc_fmt_info_drm_pixelformat(fi);

    msg_Dbg(vd, "%s: %s -> %s (%#"PRIx64"): prime: %d", __func__,
            drmu_log_fourcc(fmtp->i_chroma), drmu_log_fourcc(drm_fmt), drm_mod,
            drmu_vlc_fmt_info_is_drmprime(fi));

#if HAS_ZC_CMA
    if (fmtp->i_chroma == VLC_CODEC_MMAL_OPAQUE) {
        // Can't deal directly with opaque - but we can always convert it to ZC I420
        fmtp->i_chroma = VLC_CODEC_MMAL_ZC_I420;
    }
    else
#endif
    if (drmu_plane_format_check(sys->dp, drm_fmt, drm_mod)) {
        // DRMP or it is a format where simple byte copying works
    }
    else {
        const vlc_fourcc_t *fallback = vlc_fourcc_IsYUV(fmtp->i_chroma) ?
            vlc_fourcc_GetYUVFallback(fmtp->i_chroma) :
            vlc_fourcc_GetRGBFallback(fmtp->i_chroma);
        static const vlc_fourcc_t fallback2[] = {
            VLC_CODEC_I420,
            VLC_CODEC_RGB32,
            0
        };

        if ((fi = find_fmt_fallback(vd, sys, fallback)) == NULL &&
            (fi = find_fmt_fallback(vd, sys, fallback2)) == NULL)
            return VLC_EGENERIC;

        fmtp->i_chroma = drmu_vlc_fmt_info_vlc_chroma(fi);
        drmu_vlc_fmt_info_vlc_rgb_masks(fi, &fmtp->i_rmask, &fmtp->i_gmask, &fmtp->i_bmask);

        msg_Dbg(vd, "%s: Fallback %s/%x/%x/%x -> %s %"PRIx64, __func__,
                drmu_log_fourcc(fmtp->i_chroma),
                fmtp->i_rmask, fmtp->i_gmask, fmtp->i_bmask,
                drmu_log_fourcc(drmu_vlc_fmt_info_drm_pixelformat(fi)),
                drmu_vlc_fmt_info_drm_modifier(fi));
    }
    return 0;
}

static void
set_simple_format_size(video_format_t * const dst_fmt, const video_format_t * const src_fmt, const drmu_rect_t dst_rect)
{
#if 0
    // Create a full pic with a centre cropping region
    const drmu_rect_t fmt_crop = drmu_rect_vlc_format_crop(src_fmt);
    const drmu_rect_t src_rect = drmu_rect_resize(drmu_rect_wh(src_fmt->i_width, src_fmt->i_height), dst_rect, fmt_crop);
    const drmu_rect_t crop_rect = drmu_rect_resize(fmt_crop, dst_rect, fmt_crop);

    dst_fmt->i_width = src_rect.w;
    dst_fmt->i_height = src_rect.h;
    dst_fmt->i_visible_width = crop_rect.w;
    dst_fmt->i_visible_height = crop_rect.h;
    dst_fmt->i_x_offset = crop_rect.x;
    dst_fmt->i_y_offset = crop_rect.y;
#else
    // Just give us the cropped bit
    VLC_UNUSED(src_fmt);
    dst_fmt->i_width = dst_rect.w;
    dst_fmt->i_height = dst_rect.h;
    dst_fmt->i_visible_width = dst_rect.w;
    dst_fmt->i_visible_height = dst_rect.h;
    dst_fmt->i_x_offset = 0;
    dst_fmt->i_y_offset = 0;
#endif
}

// Updates sys but shouldn't touch vd
// Sets fmt
static int
reconfigure_display(const vout_display_t * const vd, vout_display_sys_t * const sys,
                    const vout_display_cfg_t * const cfg, video_format_t * const fmt)
{
    int ret;
    *fmt = vd->source;
    if ((ret = set_format(vd, sys, fmt)) != 0)
        return ret;
    configure_display(vd, sys, cfg, fmt);
    if (sys->output_simple)
        set_simple_format_size(fmt, &vd->source, drmu_rect_vlc_place(&sys->dest_rect));
    return 0;
}

static int vd_drm_control(vout_display_t *vd, int query, va_list args)
{
    vout_display_sys_t * const sys = vd->sys;
    video_format_t fmt;
    int ret = VLC_EGENERIC;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: query=%d", __func__, query);
#endif

    switch (query) {
        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
            if ((ret = reconfigure_display(vd, sys, NULL, &fmt)) != 0)
                break;
            if (!video_format_IsSimilar(&vd->fmt, &fmt)) {
                if (vd->info.has_pictures_invalid)
                    vout_display_SendEventPicturesInvalid(vd);
                else
                    msg_Err(vd, "Wanted Pic Invalid but not allowed");
            }
            break;

        case VOUT_DISPLAY_CHANGE_ZOOM:
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
            if ((ret = reconfigure_display(vd, sys, va_arg(args, const vout_display_cfg_t *), &fmt)) != 0)
                break;
            if (!video_format_IsSimilar(&vd->fmt, &fmt)) {
                if (vd->info.has_pictures_invalid)
                    vout_display_SendEventPicturesInvalid(vd);
                else
                    msg_Err(vd, "Wanted Pic Invalid but not allowed");
            }
            break;

        case VOUT_DISPLAY_RESET_PICTURES:
            kill_pool(sys);

            if ((ret = reconfigure_display(vd, sys, NULL, &fmt)) != 0)
                break;
            vd->fmt = fmt;
            break;

        default:
            msg_Warn(vd, "Unknown control query %d", query);
            break;
    }

    return ret;
}

static void CloseDrmVout(vout_display_t *vd)
{
    vout_display_sys_t *const sys = vd->sys;
    unsigned int i;

    msg_Dbg(vd, "<<< %s", __func__);

    drmu_pool_kill(&sys->sub_fb_pool);
    drmu_pool_kill(&sys->pic_pool);

    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_plane_unref(sys->subplanes + i);

    kill_pool(sys);

    drmu_plane_unref(&sys->dp);
    drmu_output_unref(&sys->dout);
    drmu_env_kill(&sys->du);

    free(sys->subpic_chromas);
    vd->info.subpicture_chromas = NULL;

    vd->sys = NULL;
    free(sys);
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}

// VLC will take a list of subpic formats but it then ignores the fact it is a
// list and picks the 1st one whether it is 'best' or indeed whether or not it
// can use it.  So we have to sort ourselves & have checked usablity.
// Higher number, higher priority. 0 == Do not use.
static int subpic_fourcc_usability(const vlc_fourcc_t fcc)
{
    switch (fcc) {
        case VLC_CODEC_ARGB:
            return 20;
        case VLC_CODEC_RGBA:
            return 22;
        case VLC_CODEC_BGRA:
            return 21;
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
        if ((*p = drmu_vlc_fmt_info_vlc_chroma(drmu_vlc_fmt_info_find_drm(drm_chromas[j], 0))) != 0)
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
test_simple_plane_set(vout_display_t * const vd, vout_display_sys_t * const sys,
                      const video_format_t * const fmt,
                      unsigned int w, unsigned int h,
                      const drmu_rect_t dst_rect)
{
    drmu_atomic_t *da = drmu_atomic_new(sys->du);
    drmu_fb_t *fb;
    int rv = -ENOMEM;
    const drmu_vlc_fmt_info_t * const fi = drmu_vlc_fmt_info_find_vlc(fmt);

    if (fi == NULL) {
        msg_Err(vd, "Can't find chroma format");
        goto fail;
    }

    if (da == NULL) {
        msg_Warn(vd, "Failed to alloc test atomic");
        goto fail;
    }

    if ((fb = drmu_pool_fb_new(sys->sub_fb_pool, w, h,
                               drmu_vlc_fmt_info_drm_pixelformat(fi),
                               drmu_vlc_fmt_info_drm_modifier(fi))) == NULL) {
        msg_Warn(vd, "Failed to alloc test FB");
        goto fail;
    }

    if ((rv = drmu_atomic_plane_add_fb(da, sys->dp, fb, dst_rect)) != 0) {
        msg_Warn(vd, "Failed to add test FB to atomic");
        goto fail;
    }

    if ((rv = drmu_atomic_commit(da, DRM_MODE_ATOMIC_TEST_ONLY)) != 0) {
        msg_Warn(vd, "Failed to commit test FB");
        goto fail;
    }

fail:
    drmu_atomic_unref(&da);
    drmu_fb_unref(&fb);
    return rv;
}

static int OpenDrmVout(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
//    video_format_t * const fmtp = &vd->fmt;
    video_format_t out_fmt = vd->source;
    const video_format_t *const src_fmt = &vd->source;
    const uint32_t src_chroma = src_fmt->i_chroma;
    vout_display_sys_t *sys;
    char * display_name = NULL;
    int ret = VLC_EGENERIC;
    int rv;
    msg_Info(vd, "<<< %s: Fmt=%4.4s", __func__, (const char *)&src_fmt->i_chroma);

//    if (!var_InheritBool(vd, "fullscreen")) {
//        msg_Dbg(vd, ">>> %s: Not fullscreen", __func__);
//        return ret;
//    }

    sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;
    vd->sys = sys;

    sys->mode_id = -1;

    display_name = var_InheritString(vd, DRM_VOUT_DISPLAY_NAME);

    {
        int qt_num = var_InheritInteger(vd, "qt-fullscreen-screennumber");
        const char * conn_name = qt_num == 0 ? "HDMI-A-1" :  qt_num == 1 ? "HDMI-A-2" : NULL;
        const char * dname;
        const drmu_log_env_t log = {
            .fn = drmu_log_vlc_cb,
            .v = vd,
            .max_level = DRMU_LOG_LEVEL_ALL
        };

        if (display_name && strcasecmp(display_name, "auto") != 0) {
            if (strcasecmp(display_name, "hdmi-1") == 0)
                conn_name = "HDMI-A-1";
            else if (strcasecmp(display_name, "hdmi-2") == 0)
                conn_name = "HDMI-A-2";
            else
                conn_name = display_name;
        }

        dname = conn_name != NULL ? conn_name : "<auto>";

        sys->du = drmu_env_new_xlease(&log);

        if (sys->du == NULL) {
            if (drmu_scan_output(conn_name, &log, &sys->du, &sys->dout) == 0)
                msg_Dbg(vd, "Using conn %s", dname);
        }

        if (sys->du == NULL) {
            char * module_name = var_InheritString(vd, DRM_VOUT_MODULE_NAME);
            if (module_name != NULL) {
                sys->du = drmu_env_new_open(module_name, &log);
                free(module_name);
                if (sys->du == NULL)
                    goto fail;
            }
        }

        if (sys->dout == NULL) {
            if ((sys->dout = drmu_output_new(sys->du)) == NULL) {
                msg_Err(vd, "Failed to allocate new drmu output");
                goto fail;
            }

            if ((rv = drmu_output_add_output(sys->dout, conn_name)) != 0)
                msg_Err(vd, "Failed to find output %s: %s", dname, strerror(-rv));
            else
                msg_Dbg(vd, "Using conn %s", dname);

            if (rv != 0)
                goto fail;
        }
    }

    drmu_env_restore_enable(sys->du);

    drmu_output_modeset_allow(sys->dout, !var_InheritBool(vd, DRM_VOUT_NO_MODESET_NAME));
    drmu_output_max_bpc_allow(sys->dout, !var_InheritBool(vd, DRM_VOUT_NO_MAX_BPC));

    // Get frame buffer pools - try generic dmabufs first for cached
    // buffers that are faster than uncached BOs
    if ((sys->sub_fb_pool = drmu_pool_new_dmabuf_video(sys->du, 10)) == NULL &&
        (sys->sub_fb_pool = drmu_pool_new_dumb(sys->du, 10)) == NULL)
        goto fail;
    if ((sys->pic_pool = drmu_pool_new_dmabuf_video(sys->du, 40)) == NULL &&
        (sys->pic_pool = drmu_pool_new_dumb(sys->du, 40)) == NULL)
        goto fail;

    // This wants to be the primary
    if ((sys->dp = drmu_output_plane_ref_primary(sys->dout)) == NULL)
        goto fail;

    for (unsigned int i = 0; i != SUBPICS_MAX; ++i) {
        if ((sys->subplanes[i] = drmu_output_plane_ref_other(sys->dout)) == NULL) {
            msg_Warn(vd, "Cannot allocate subplane %d", i);
            break;
        }
        if (sys->subpic_chromas == NULL) {
            unsigned int n;
            const uint32_t * const drm_chromas = drmu_plane_formats(sys->subplanes[i], &n);
            sys->subpic_chromas = subpic_make_chromas_from_drm(drm_chromas, n);
        }
    }

    if (set_format(vd, sys, &out_fmt)) {
        msg_Warn(vd, "Failed to find compatible output format");
        goto fail;
    }

    sys->mode_id = -1;

    char * mode_name = NULL;
    const char * modestr;

    if (var_InheritBool(vd, DRM_VOUT_SOURCE_MODESET_NAME))
        modestr = "source";
    else {
        mode_name = var_InheritString(vd, DRM_VOUT_MODE_NAME);
        modestr = mode_name;
    }

    if (modestr != NULL && strcmp(modestr, "none") != 0) {
        drmu_mode_simple_params_t pick = {
            .width = src_fmt->i_visible_width,
            .height = src_fmt->i_visible_height,
            .hz_x_1000 = src_fmt->i_frame_rate_base == 0 ? 0 :
                (unsigned int)(((uint64_t)src_fmt->i_frame_rate * 1000) / src_fmt->i_frame_rate_base),
        };

        if (strcmp(modestr, "source") != 0) {
            unsigned int w, h, hz;
            if (*drmu_util_parse_mode(modestr, &w, &h, &hz) != 0) {
                msg_Err(vd, "Bad mode string: '%s'", modestr);
                free(mode_name);
                ret = VLC_EGENERIC;
                goto fail;
            }
            if (w && h) {
                pick.width = w;
                pick.height = h;
            }
            if (hz)
                pick.hz_x_1000 = hz;
        }

        sys->mode_id = drmu_output_mode_pick_simple(sys->dout, drmu_mode_pick_simple_cb, &pick);

        msg_Dbg(vd, "Mode id=%d", sys->mode_id);

        // This will set the mode on the crtc var but won't actually change the output
        if (sys->mode_id >= 0) {
            const drmu_mode_simple_params_t * mode;

            drmu_output_mode_id_set(sys->dout, sys->mode_id);
            mode = drmu_output_mode_simple_params(sys->dout);
                msg_Info(vd, "Mode %d: %dx%d@%d.%03d %d/%d - req %dx%d@%d.%d", sys->mode_id,
                        mode->width, mode->height, mode->hz_x_1000 / 1000, mode->hz_x_1000 % 1000,
                        mode->sar.num, mode->sar.den, pick.width, pick.height, pick.hz_x_1000 / 1000, pick.hz_x_1000 % 1000);
        }
    }
    free(mode_name);

    {
        char * const window_str = var_InheritString(vd, DRM_VOUT_WINDOW_NAME);
        if (strcmp(window_str, "fullscreen") == 0) {
            /* Leave req_win null */
            msg_Dbg(vd, "Window: fullscreen");
        }
        else {
            sys->req_win = str_to_rect(window_str);
            if (sys->req_win.width != 0)
                msg_Dbg(vd, "Window: %dx%d @ %d,%d",
                        sys->req_win.width, sys->req_win.height,
                        sys->req_win.x, sys->req_win.y);
            else
                msg_Warn(vd, "Window: '%s': cannot parse (usage: <w>x<h>+<x>+<y>) - using fullscreen", window_str);
        }
        free(window_str);
    }

    if (src_chroma != out_fmt.i_chroma)
        msg_Warn(vd, "Cannot display %s directly trying %s", drmu_log_fourcc(src_chroma), drmu_log_fourcc(out_fmt.i_chroma));

    set_display_windows(vd, sys);

    {
        const unsigned int w = sys->display_rect.width;
        const unsigned int h = sys->display_rect.height;
        const drmu_rect_t sr = {.x = w / 5, .y = h / 5, .w = w / 3, .h = h / 3};
        if (test_simple_plane_set(vd, sys, &out_fmt, w, h, drmu_rect_wh(w, h)) != 0) {
            msg_Warn(vd, "Failed simple pic test for mode %dx%d", w, h);
            goto fail;
        }
        else {
            msg_Dbg(vd, "OK simple pic test for mode %dx%d", w, h);
        }

        // Test for full scale & position capability - the incoming stream
        // might not need it but if anything changes it is better to be sure we
        // can cope with it
        if (test_simple_plane_set(vd, sys, &out_fmt, vd->source.i_visible_width, vd->source.i_visible_height, sr) != 0) {
            msg_Warn(vd, "Failed scale pic test for %dx%d->%dx%d", vd->source.i_visible_width, vd->source.i_visible_height, sr.w, sr.h);
            sys->output_simple = true;
        }
        else {
            msg_Dbg(vd, "OK source pic test for %dx%d->%dx%d", vd->source.i_visible_width, vd->source.i_visible_height, sr.w, sr.h);
        }
    }

    configure_display(vd, sys, vd->cfg, &vd->source);

    if (sys->output_simple)
        set_simple_format_size(&out_fmt, src_fmt, drmu_rect_vlc_place(&sys->dest_rect));

    // Simple does not work usefully with dmabuf input
    sys->pool_try_fb = !sys->output_simple && var_InheritBool(vd, DRM_VOUT_POOL_DMABUF_NAME);

    // All setup done - no possibility of error from here on
    // Do final config setup & cleanup

    free(display_name);

    vd->fmt = out_fmt;

    vd->info = (vout_display_info_t){
        .is_slow = false,
        .has_double_click = false,
        .needs_hide_mouse = false,
        .has_pictures_invalid = sys->output_simple,
        .subpicture_chromas = sys->subpic_chromas
    };

    vd->pool = vd_drm_pool;
    vd->prepare = vd_drm_prepare;
    vd->display = vd_drm_display;
    vd->control = vd_drm_control;

    {
        const drmu_mode_simple_params_t * const mode = drmu_output_mode_simple_params(sys->dout);
        if (vd->cfg->display.width != mode->width || vd->cfg->display.height != mode->height) {
            msg_Dbg(vd, "Set display size to %ux%u", mode->width, mode->height);
            vout_display_SendEventDisplaySize(vd, mode->width, mode->height);
        }
    }

    msg_Dbg(vd, ">>> %s", __func__);
    return VLC_SUCCESS;

fail:
    CloseDrmVout(vd);
    free(display_name);
    return ret;
}

vlc_module_begin()
    set_shortname(N_("DRM vout"))
    set_description(N_("DRM vout plugin"))
    set_capability("vout display", 32)  // 2 points better than fb(30), ascii(15)
    add_shortcut("drm-vout")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)

    add_bool(DRM_VOUT_SOURCE_MODESET_NAME, false, DRM_VOUT_SOURCE_MODESET_TEXT, DRM_VOUT_SOURCE_MODESET_LONGTEXT, false)
    add_bool(DRM_VOUT_NO_MODESET_NAME,     false, DRM_VOUT_NO_MODESET_TEXT, DRM_VOUT_NO_MODESET_LONGTEXT, false)
    add_bool(DRM_VOUT_NO_MAX_BPC,          false, DRM_VOUT_NO_MAX_BPC_TEXT, DRM_VOUT_NO_MAX_BPC_LONGTEXT, false)
    add_bool(DRM_VOUT_POOL_DMABUF_NAME,    false, DRM_VOUT_POOL_DMABUF_TEXT, DRM_VOUT_POOL_DMABUF_LONGTEXT, false)
    add_string(DRM_VOUT_MODE_NAME,         "none", DRM_VOUT_MODE_TEXT, DRM_VOUT_MODE_LONGTEXT, false)
    add_string(DRM_VOUT_WINDOW_NAME,       "fullscreen", DRM_VOUT_WINDOW_TEXT, DRM_VOUT_WINDOW_LONGTEXT, false)
    add_string(DRM_VOUT_DISPLAY_NAME,      "auto", DRM_VOUT_DISPLAY_TEXT, DRM_VOUT_DISPLAY_LONGTEXT, false)
    add_string(DRM_VOUT_MODULE_NAME,       DRM_MODULE, DRM_VOUT_MODULE_TEXT, DRM_VOUT_MODULE_LONGTEXT, false)

    set_callbacks(OpenDrmVout, CloseDrmVout)
vlc_module_end()

