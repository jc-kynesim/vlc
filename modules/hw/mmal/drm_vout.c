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

#include <vlc_common.h>

#include <vlc_codec.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include <libavutil/buffer.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../codec/avcodec/drm_pic.h"

#define TRACE_ALL 1

#define DRM_MODULE "vc4"
#define ERRSTR strerror(errno)

struct drm_setup {
    int conId;
    uint32_t crtcId;
    int crtcIdx;
    uint32_t planeId;
    unsigned int out_fourcc;
    struct {
        int x, y, width, height;
    } compose;
};

#define HOLD_SIZE 3

typedef struct vout_display_sys_t {
    vlc_decoder_device *dec_dev;

    int drm_fd;
    uint32_t con_id;
    struct drm_setup setup;

    unsigned int hold_n;
    struct display_hold_s {
        unsigned int fb_handle;
        picture_t * pic;
    } hold[HOLD_SIZE];
} vout_display_sys_t;

static const vlc_fourcc_t drm_subpicture_chromas[] = { VLC_CODEC_RGBA, VLC_CODEC_BGRA, VLC_CODEC_ARGB, 0 };

static void hold_uninit(vout_display_sys_t *const de, struct display_hold_s * const dh)
{
    if (dh->fb_handle != 0)
    {
        drmModeRmFB(de->drm_fd, dh->fb_handle);
        dh->fb_handle = 0;
    }

    if (dh->pic != NULL) {
        picture_Release(dh->pic);
        dh->pic = NULL;
    }
}


static int find_crtc(vout_display_t *const vd, const int drmfd,
                     struct drm_setup *const s, uint32_t *const pConId)
{
    int ret = -1;
    int i;
    drmModeRes * const res = drmModeGetResources(drmfd);
    drmModeConnector *c;

    if (!res) {
        msg_Err(vd, "drmModeGetResources failed: %s", ERRSTR);
        return -1;
    }

    if (res->count_crtcs <= 0) {
        msg_Err(vd, "drm: no crts");
        goto fail_res;
    }

    if (!s->conId) {
        msg_Info(vd, "No connector ID specified.  Choosing default from list:");

        for (i = 0; i < res->count_connectors; i++) {
            drmModeConnector *con =
                drmModeGetConnector(drmfd, res->connectors[i]);
            drmModeEncoder *enc = NULL;
            drmModeCrtc *crtc = NULL;

            if (con->encoder_id) {
                enc = drmModeGetEncoder(drmfd, con->encoder_id);
                if (enc->crtc_id) {
                    crtc = drmModeGetCrtc(drmfd, enc->crtc_id);
                }
            }

            if (!s->conId && crtc) {
                s->conId = con->connector_id;
                s->crtcId = crtc->crtc_id;
            }

            msg_Info(vd, "Connector %d (crtc %d): type %d, %dx%d%s",
                     con->connector_id,
                     crtc ? crtc->crtc_id : 0,
                     con->connector_type,
                     crtc ? crtc->width : 0,
                     crtc ? crtc->height : 0,
                     (s->conId == (int)con->connector_id ?
                      " (chosen)" : ""));
        }

        if (!s->conId) {
            msg_Err(vd, "No suitable enabled connector found.");
            return -1;;
        }
    }

    s->crtcIdx = -1;

    for (i = 0; i < res->count_crtcs; ++i) {
        if (s->crtcId == res->crtcs[i]) {
            s->crtcIdx = i;
            break;
        }
    }

    if (s->crtcIdx == -1) {
        msg_Warn(vd, "drm: CRTC %u not found", s->crtcId);
        goto fail_res;
    }

    if (res->count_connectors <= 0) {
        msg_Warn(vd, "drm: no connectors");
        goto fail_res;
    }

    c = drmModeGetConnector(drmfd, s->conId);
    if (!c) {
        msg_Warn(vd, "drmModeGetConnector failed: %s", ERRSTR);
        goto fail_res;
    }

    if (!c->count_modes) {
        msg_Warn(vd, "connector supports no mode");
        goto fail_conn;
    }

    {
        drmModeCrtc *crtc = drmModeGetCrtc(drmfd, s->crtcId);
        s->compose.x = crtc->x;
        s->compose.y = crtc->y;
        s->compose.width = crtc->width;
        s->compose.height = crtc->height;
        drmModeFreeCrtc(crtc);
    }

    if (pConId)
        *pConId = c->connector_id;
    ret = 0;

fail_conn:
    drmModeFreeConnector(c);

fail_res:
    drmModeFreeResources(res);

    return ret;
}

// *** Defer this
static int find_plane(vout_display_t *const vd, int drmfd, struct drm_setup *s, const uint32_t req_format)
{
    drmModePlaneResPtr planes;
    drmModePlanePtr plane;
    unsigned int i;
    unsigned int j;
    int ret = 0;

    planes = drmModeGetPlaneResources(drmfd);
    if (!planes) {
        msg_Warn(vd, "drmModeGetPlaneResources failed: %s", ERRSTR);
        return -1;
    }

    for (i = 0; i < planes->count_planes; ++i) {
        plane = drmModeGetPlane(drmfd, planes->planes[i]);
        if (!planes) {
            msg_Warn(vd, "drmModeGetPlane failed: %s\n", ERRSTR);
            break;
        }

        if (!(plane->possible_crtcs & (1 << s->crtcIdx))) {
            drmModeFreePlane(plane);
            continue;
        }

        for (j = 0; j < plane->count_formats; ++j) {
            char tbuf[5];
            msg_Info(vd, "Plane %d: %s", j, str_fourcc(tbuf, plane->formats[j]));
            if (plane->formats[j] == req_format)
                break;
        }

        if (j == plane->count_formats) {
            drmModeFreePlane(plane);
            continue;
        }

        s->planeId = plane->plane_id;
        s->out_fourcc = req_format;
        drmModeFreePlane(plane);
        break;
    }

    if (i == planes->count_planes)
        ret = -1;

    drmModeFreePlaneResources(planes);
    return ret;
}

static void vd_drm_prepare(vout_display_t *vd, picture_t *p_pic,
                           subpicture_t *subpicture, vlc_tick_t date)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(p_pic);
    VLC_UNUSED(subpicture);
    VLC_UNUSED(date);

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
}

static void vd_drm_display(vout_display_t *vd, picture_t *p_pic)
{
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(p_pic);
    vout_display_sys_t *const de = vd->sys;
    struct display_hold_s *const dh = de->hold + de->hold_n;
    const uint32_t format = desc->layers[0].format;
    unsigned int fb_handle = 0;
    int ret = 0;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: fd=%d", __func__, desc->objects[0].fd);
#endif

    if (de->setup.out_fourcc != format)
    {
        if (find_plane(vd, de->drm_fd, &de->setup, format))
        {
            msg_Warn(vd, "No plane for format: %s", fourcc2str(format));
            return;
        }
    }

#if 0
    {
        drmVBlank vbl = {
            .request = {
                .type = DRM_VBLANK_RELATIVE,
                .sequence = 1
            }
        };

        while (drmWaitVBlank(de->drm_fd, &vbl))
        {
            if (errno != EINTR)
            {
                av_log(s, AV_LOG_WARNING, "drmWaitVBlank failed: %s\n", ERRSTR);
                break;
            }
        }
    }
#endif

    {
        uint32_t pitches[4] = { 0 };
        uint32_t offsets[4] = { 0 };
        uint64_t modifiers[4] = { 0 };
        uint32_t bo_object_handles[4] = { 0 };
        uint32_t bo_handles[4] = { 0 };
        int i, j, n;

        for (i = 0; i < desc->nb_objects; ++i)
        {
            if (drmPrimeFDToHandle(de->drm_fd, desc->objects[i].fd, bo_object_handles + i) != 0)
            {
                msg_Warn(vd, "drmPrimeFDToHandle[%d](%d) failed: %s", i, desc->objects[i].fd, ERRSTR);
                return;
            }
        }

        n = 0;
        for (i = 0; i < desc->nb_layers; ++i)
        {
            for (j = 0; j < desc->layers[i].nb_planes; ++j)
            {
                const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
                const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;
                pitches[n] = p->pitch;
                offsets[n] = p->offset;
                modifiers[n] = obj->format_modifier;
                bo_handles[n] = bo_object_handles[p->object_index];
                ++n;
            }
        }

#if 0
        av_log(s, AV_LOG_DEBUG, "%dx%d, fmt: %x, boh=%d,%d,%d,%d, pitch=%d,%d,%d,%d,"
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

        if (drmModeAddFB2WithModifiers(de->drm_fd,
                                       p_pic->format.i_visible_width,
                                       p_pic->format.i_visible_height,
                                       desc->layers[0].format, bo_handles,
                                       pitches, offsets, modifiers,
                                       &fb_handle, DRM_MODE_FB_MODIFIERS /** 0 if no mods */) != 0)
        {
            msg_Err(vd, "drmModeAddFB2WithModifiers failed: %s\n", ERRSTR);
            return;
        }
    }

    ret = drmModeSetPlane(de->drm_fd, de->setup.planeId, de->setup.crtcId,
                          fb_handle, 0,
                          de->setup.compose.x, de->setup.compose.y,
                          de->setup.compose.width,
                          de->setup.compose.height,
                          0, 0,
                          p_pic->format.i_visible_width << 16,
                          p_pic->format.i_visible_height << 16);

    if (ret != 0)
    {
        msg_Err(vd, "drmModeSetPlane failed: %s", ERRSTR);
    }


    hold_uninit(de, dh);
    dh->fb_handle = fb_handle;
    dh->pic = picture_Hold(p_pic);
    de->hold_n = de->hold_n + 1 >= HOLD_SIZE ? 0 : de->hold_n + 1;

    return;
}

static int vd_drm_control(vout_display_t *vd, int query)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(query);
    return VLC_SUCCESS;
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

    for (i = 0; i != HOLD_SIZE; ++i)
        hold_uninit(sys, sys->hold + i);

    if (sys->dec_dev)
        vlc_decoder_device_Release(sys->dec_dev);

    free(sys);
}

static const struct vlc_display_operations ops = {
    .close =            CloseDrmVout,
    .prepare =          vd_drm_prepare,
    .display =          vd_drm_display,
    .control =          vd_drm_control,
    .reset_pictures =   vd_drm_reset_pictures,
    .set_viewpoint =    NULL,
};

static int OpenDrmVout(vout_display_t *vd, const vout_display_cfg_t *cfg,
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

    if (vctx) {
        sys->dec_dev = vlc_video_context_HoldDevice(vctx);
        if (sys->dec_dev && sys->dec_dev->type != VLC_DECODER_DEVICE_DRM_PRIME) {
            vlc_decoder_device_Release(sys->dec_dev);
            sys->dec_dev = NULL;
        }
    }

    if (sys->dec_dev == NULL)
        sys->dec_dev = vlc_decoder_device_Create(VLC_OBJECT(vd), cfg->window);
    if (sys->dec_dev == NULL || sys->dec_dev->type != VLC_DECODER_DEVICE_DRM_PRIME) {
        msg_Err(vd, "Missing decoder device");
        goto fail;
    }

    if ((sys->drm_fd = drmOpen(DRM_MODULE, NULL)) < 0) {
        msg_Err(vd, "Failed to drmOpen %s", DRM_MODULE);
        return -1;
    }

    if (find_crtc(vd, sys->drm_fd, &sys->setup, &sys->con_id) != 0) {
        msg_Err(vd, "failed to find valid mode");
        return -1;
    }

    if (find_plane(vd, sys->drm_fd, &sys->setup, DRM_FORMAT_NV12) != 0) {
        msg_Err(vd, "failed to find compatible plane");
        return -1;
    }

    vd->info = (vout_display_info_t) {
        .subpicture_chromas = drm_subpicture_chromas
    };

    vd->ops = &ops;
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

set_callback_display(OpenDrmVout, 16)  // 1 point better than ASCII art
vlc_module_end()


