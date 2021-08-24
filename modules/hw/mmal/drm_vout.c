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

#define drmu_err_log(...)       msg_Err(__VA_ARGS__)
#define drmu_warn_log(...)      msg_Warn(__VA_ARGS__)
#define drmu_info_log(...)      msg_Info(__VA_ARGS__)
#define drmu_debug_log(...)     msg_Dbg(__VA_ARGS__)

#define drmu_err(_du, ...)      drmu_err_log((_du)->log, __VA_ARGS__)
#define drmu_warn(_du, ...)     drmu_warn_log((_du)->log, __VA_ARGS__)
#define drmu_info(_du, ...)     drmu_info_log((_du)->log, __VA_ARGS__)
#define drmu_debug(_du, ...)    drmu_debug_log((_du)->log, __VA_ARGS__)

typedef struct drmu_env_s {
    vlc_object_t * log;
    int fd;
    uint32_t plane_count;
    drmModePlanePtr * planes;
    drmModeResPtr res;
} drmu_env_t;

typedef struct drmu_crtc_s {
    drmu_env_t * du;
    drmModeCrtcPtr crtc;
    drmModeEncoderPtr enc;
    drmModeConnectorPtr con;
    int crtc_idx;
} drmu_crtc_t;

typedef struct drmu_plane_s {
    drmu_env_t * du;
    drmu_crtc_t * dc;
    const drmModePlane * plane;
} drmu_plane_t;

static void
uninit_crtc(drmu_crtc_t * const dc)
{
    if (dc->crtc != NULL)
        drmModeFreeCrtc(dc->crtc);
    if (dc->enc != NULL)
        drmModeFreeEncoder(dc->enc);
    if (dc->con != NULL)
        drmModeFreeConnector(dc->con);
    dc->crtc = NULL;
    dc->enc = NULL;
    dc->con = NULL;
}

static void
free_crtc(drmu_crtc_t * const dc)
{
    uninit_crtc(dc);
    free(dc);
}

static uint32_t
drmu_crtc_id(const drmu_crtc_t * const dc)
{
    return dc->crtc->crtc_id;
}

static void
drmu_crtc_delete(drmu_crtc_t ** ppdc)
{
    drmu_crtc_t * const dc = * ppdc;

    if (dc == NULL)
        return;
    *ppdc = NULL;

    free_crtc(dc);
}

static inline uint32_t
drmu_crtc_x(const drmu_crtc_t * const dc)
{
    return dc->crtc->x;
}
static inline uint32_t
drmu_crtc_y(const drmu_crtc_t * const dc)
{
    return dc->crtc->y;
}
static inline uint32_t
drmu_crtc_width(const drmu_crtc_t * const dc)
{
    return dc->crtc->width;
}
static inline uint32_t
drmu_crtc_height(const drmu_crtc_t * const dc)
{
    return dc->crtc->height;
}

static inline uint32_t
drmu_plane_id(const drmu_plane_t * const dp)
{
    return dp->plane->plane_id;
}

static void
drmu_plane_delete(drmu_plane_t ** const ppdp)
{
    drmu_plane_t * const dp = *ppdp;

    if (dp == NULL)
        return;
    *ppdp = NULL;

    free(dp);
}

static drmu_plane_t *
drmu_plane_new_find(drmu_crtc_t * const dc, const uint32_t fmt)
{
    uint32_t i;
    drmu_env_t * const du = dc->du;
    drmu_plane_t * dp;
    const drmModePlane * plane = NULL;
    const uint32_t crtc_mask = (uint32_t)1 << dc->crtc_idx;

    for (i = 0; i != du->plane_count && plane == NULL; ++i) {
        uint32_t j;
        const drmModePlane * const p = du->planes[i];

        if ((p->possible_crtcs & crtc_mask) == 0)
            continue;

        for (j = 0; j != p->count_formats; ++j) {
            if (p->formats[j] == fmt) {
                plane = p;
                break;
            }
        }
    }
    if (i == du->plane_count) {
        drmu_err(du, "%s: No plane (count=%d) found for fmt %#x", __func__, du->plane_count, fmt);
        return NULL;
    }

    if ((dp = calloc(1, sizeof(*dp))) == NULL) {
        drmu_err(du, "%s: Alloc fail", __func__);
        return NULL;
    }

    dp->du = du;
    dp->dc = dc;
    dp->plane = plane;
    return dp;
}

static drmu_crtc_t *
crtc_from_con_id(drmu_env_t * const du, const uint32_t con_id)
{
    drmu_crtc_t * const dc = calloc(1, sizeof(*dc));
    int i;

    if (dc == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }
    dc->du = du;
    dc->crtc_idx = -1;

    if ((dc->con = drmModeGetConnector(du->fd, con_id)) == NULL) {
        drmu_err(du, "%s: Failed to find connector %d", __func__, con_id);
        goto fail;
    }

    if (dc->con->encoder_id == 0) {
        drmu_debug(du, "%s: Connector %d has no encoder", __func__, con_id);
        goto fail;
    }

    if ((dc->enc = drmModeGetEncoder(du->fd, dc->con->encoder_id)) == NULL) {
        drmu_err(du, "%s: Failed to find encoder %d", __func__, dc->con->encoder_id);
        goto fail;
    }

    if (dc->enc->crtc_id == 0) {
        drmu_debug(du, "%s: Connector %d has no encoder", __func__, con_id);
        goto fail;
    }

    for (i = 0; i <= du->res->count_connectors; ++i) {
        if (du->res->crtcs[i] == dc->enc->crtc_id) {
            dc->crtc_idx = i;
            break;
        }
    }
    if (dc->crtc_idx < 0) {
        drmu_err(du, "%s: Crtc id %d not in resource list", __func__, dc->enc->crtc_id);
        goto fail;
    }

    if ((dc->crtc = drmModeGetCrtc(du->fd, dc->enc->crtc_id)) == NULL) {
        drmu_err(du, "%s: Failed to find crtc %d", __func__, dc->enc->crtc_id);
        goto fail;
    }

    return dc;

fail:
    free_crtc(dc);
    return NULL;
}

static drmu_crtc_t *
drmu_crtc_new_find(drmu_env_t * const du)
{
    int i;
    drmu_crtc_t * dc;

    if (du->res->count_crtcs <= 0) {
        drmu_err(du, "%s: no crts", __func__);
        return NULL;
    }

    i = 0;
    do {
        if (i >= du->res->count_connectors) {
            drmu_err(du, "%s: No suitable crtc found in %d connectors", __func__, du->res->count_connectors);
            break;
        }

        dc = crtc_from_con_id(du, du->res->connectors[i]);

        ++i;
    } while (dc == NULL);

    return dc;
}

static void
free_planes(drmu_env_t * const du)
{
    uint32_t i;
    for (i = 0; i != du->plane_count; ++i)
        drmModeFreePlane(du->planes[i]);
    free(du->planes);
    du->plane_count = 0;
    du->planes = NULL;
}

static int
drmu_env_planes_populate(drmu_env_t * const du)
{
    int err;
    drmModePlaneResPtr res;
    uint32_t i;

    if ((res = drmModeGetPlaneResources(du->fd)) == NULL) {
        err = errno;
        drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
        goto fail0;
    }

    if ((du->planes = calloc(res->count_planes, sizeof(*du->planes))) == NULL) {
        err = ENOMEM;
        drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
        goto fail1;
    }

    for (i = 0; i != res->count_planes; ++i) {
        if ((du->planes[i] = drmModeGetPlane(du->fd, res->planes[i])) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
            goto fail2;
        }
        du->plane_count = i;
    }

    return 0;

fail2:
    free_planes(du);
fail1:
    drmModeFreePlaneResources(res);
fail0:
    return -err;
}

#if 0
static int
drmu_env_resources_populate(drmu_env_t * const du)
{
    int err;
    drmModeResPtr res;
    uint32_t i;

    if ((res = drmModeGetResources(du->fd)) == NULL) {
        err = errno;
        drmu_err(du, "%s: drmModeGetResources failed: %s", __func__, strerror(err));
        goto fail0;
    }

    if ((du->planes = calloc(res->count_planes, sizeof(*du->planes))) == NULL) {
        err = ENOMEM;
        drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
        goto fail1;
    }

    for (i = 0; i != res->count_planes; ++i) {
        if ((du->planes[i] = drmModeGetPlane(du->fd, res->planes[i])) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
            goto fail2;
        }
    }

    return 0;

fail2:
    free_planes(du);
fail1:
    drmModeFreePlaneResources(res);
fail0:
    return -err;
}
#endif

static inline int
drmu_fd(const drmu_env_t * const du)
{
    return du->fd;
}

static void
drmu_env_delete(drmu_env_t ** const ppdu)
{
    drmu_env_t * const du = *ppdu;

    if (!du)
        return;
    *ppdu = NULL;

    if (du->res != NULL)
        drmModeFreeResources(du->res);
    free_planes(du);

    close(du->fd);
    free(du);
}

// Closes fd on failure
static drmu_env_t *
drmu_env_new_fd(vlc_object_t * const log, const int fd)
{
    drmu_env_t * du = calloc(1, sizeof(*du));
    if (!du) {
        drmu_err_log(log, "Failed to create du: No memory");
        close(fd);
        return NULL;
    }

    du->log = log;
    du->fd = fd;

    if (drmu_env_planes_populate(du) != 0)
        goto fail1;

    if ((du->res = drmModeGetResources(du->fd)) == NULL) {
        drmu_err(du, "%s: Failed to get resources", __func__);
        goto fail1;
    }

    return du;

fail1:
    drmu_env_delete(&du);
    return NULL;
}

static drmu_env_t *
drmu_env_new_open(vlc_object_t * const log, const char * name)
{
    int fd = drmOpen(name, NULL);
    if (fd == -1) {
        drmu_err_log(log, "Failed to open %s", name);
        return NULL;
    }
    return drmu_env_new_fd(log, fd);
}


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

    drmu_env_t * du;
    drmu_crtc_t * dc;
    drmu_plane_t * dp;

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
        drmModeRmFB(drmu_fd(de->du), dh->fb_handle);
        dh->fb_handle = 0;
    }

    if (dh->pic != NULL) {
        picture_Release(dh->pic);
        dh->pic = NULL;
    }
}

#if 0
static void free_crtcx(vout_display_sys_t * const de)
{
    if (de->crtc) {
        drmModeFreeCrtc((drmModeCrtc *)de->crtc);
        de->crtc = NULL;
    }
}

static int find_crtc(vout_display_t *const vd, const int drmfd,
                     struct drm_setup *const s, uint32_t *const pConId)
{
    vout_display_sys_t *const de = vd->sys;
    int ret = -1;
    int i;
    drmModeRes * const res = drmModeGetResources(drmfd);
    drmModeConnector *c;

    free_crtcx(de);

    if (!res) {
        msg_Err(vd, "drmModeGetResources failed: %s", ERRSTR);
        return -1;
    }

    if (res->count_crtcs <= 0) {
        msg_Err(vd, "drm: no crts");
        goto fail_res;
    }

    if (!s->conId) {
        msg_Info(vd, "No connector ID specified.  Choosing default from list (%d):", res->count_connectors);

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
            return -1;
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
        de->crtc = crtc;
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
#endif

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
//    const uint32_t format = desc->layers[0].format;
    unsigned int fb_handle = 0;
    int ret = 0;
    vout_display_place_t place;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: fd=%d", __func__, desc->objects[0].fd);
#endif

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
            if (drmPrimeFDToHandle(drmu_fd(de->du), desc->objects[i].fd, bo_object_handles + i) != 0)
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

        if (drmModeAddFB2WithModifiers(drmu_fd(de->du),
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

    {
        vout_display_cfg_t tcfg = *vd->cfg;
        tcfg.is_display_filled = true;
        tcfg.display.width = drmu_crtc_width(de->dc);
        tcfg.display.height = drmu_crtc_height(de->dc);
        vout_display_PlacePicture(&place, vd->source, &tcfg);
    }

    // *** Make drmu_plane_set
    ret = drmModeSetPlane(drmu_fd(de->du),
                          drmu_plane_id(de->dp),
                          drmu_crtc_id(de->dc),
                          fb_handle, 0,
                          place.x, place.y,
                          place.width, place.height,
                          p_pic->format.i_x_offset,
                          p_pic->format.i_y_offset,
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

    drmu_plane_delete(&sys->dp);
    drmu_crtc_delete(&sys->dc);
    drmu_env_delete(&sys->du);

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

    if ((sys->du = drmu_env_new_open(VLC_OBJECT(vd), DRM_MODULE)) == NULL)
        goto fail;;

    if ((sys->dc = drmu_crtc_new_find(sys->du)) == NULL)
        goto fail;

    if ((sys->dp = drmu_plane_new_find(sys->dc, DRM_FORMAT_NV12)) == NULL)
        goto fail;

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


