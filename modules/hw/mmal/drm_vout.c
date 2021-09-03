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
#include <stdatomic.h>

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
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../codec/avcodec/drm_pic.h"

#define TRACE_ALL 1

#define SUBPICS_MAX 4

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

struct drmu_fb_s;
struct drmu_crtc_s;
struct drmu_env_s;

typedef struct drmu_rect_s {
    uint32_t x, y;
    uint32_t w, h;
} drmu_rect_t;

typedef void (* drmu_fb_delete_fn)(struct drmu_fb_s * dfb, void * v);

typedef struct drmu_fb_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init

    struct drmu_env_s * du;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    drmu_rect_t cropped;
    unsigned int handle;

    void * map_ptr;
    size_t map_size;
    size_t map_pitch;

    void * on_delete_v;
    drmu_fb_delete_fn on_delete_fn;

} drmu_fb_t;

typedef struct drmu_plane_s {
    struct drmu_env_s * du;
    struct drmu_crtc_s * dc;    // NULL if not in use
    const drmModePlane * plane;
} drmu_plane_t;

typedef struct drmu_crtc_s {
    struct drmu_env_s * du;
    drmModeCrtcPtr crtc;
    drmModeEncoderPtr enc;
    drmModeConnectorPtr con;
    int crtc_idx;
} drmu_crtc_t;

typedef struct drmu_env_s {
    vlc_object_t * log;
    int fd;
    uint32_t plane_count;
    drmu_plane_t * planes;
    drmModeResPtr res;
} drmu_env_t;

static inline drmu_rect_t
drmu_rect_vlc_format_crop(const video_frame_format_t * const format)
{
    return (drmu_rect_t){
        .x = format->i_x_offset,
        .y = format->i_y_offset,
        .w = format->i_visible_width,
        .h = format->i_visible_height};
}

static inline drmu_rect_t
drmu_rect_vlc_pic_crop(const picture_t * const pic)
{
    return drmu_rect_vlc_format_crop(&pic->format);
}

static void
free_fb(drmu_fb_t * const dfb)
{
    drmu_env_t * const du = dfb->du;

    if (dfb->handle != 0)
        drmModeRmFB(du->fd, dfb->handle);

    if (dfb->map_ptr != NULL && dfb->map_ptr != MAP_FAILED)
        munmap(dfb->map_ptr, dfb->map_size);

    // Call on_delete last so we have stopped using anything that might be
    // freed by it
    if (dfb->on_delete_fn)
        dfb->on_delete_fn(dfb, dfb->on_delete_v);

    free(dfb);
}

static drmu_fb_t *
alloc_fb(drmu_env_t * const du)
{
    drmu_fb_t * const dfb = calloc(1, sizeof(*dfb));
    if (dfb == NULL)
        return NULL;

    dfb->du = du;
    return dfb;
}

static void
drmu_fb_unref(drmu_fb_t ** const ppdfb)
{
    drmu_fb_t * const dfb = *ppdfb;

    if (dfb == NULL)
        return;
    *ppdfb = NULL;

    if (atomic_fetch_sub(&dfb->ref_count, 1) > 0)
        return;

    free_fb(dfb);
}

static drmu_fb_t *
drmu_fb_ref(drmu_fb_t * const dfb)
{
    atomic_fetch_add(&dfb->ref_count, 1);
    return dfb;
}

static void
pic_fb_delete_cb(drmu_fb_t * dfb, void * v)
{
    VLC_UNUSED(dfb);

    picture_Release(v);
}

typedef struct fb_dumb_s {
    uint32_t handle;
} fb_dumb_t;

static void
dumb_fb_free_cb(drmu_fb_t * dfb, void * v)
{
    fb_dumb_t * const aux = v;
    struct drm_mode_destroy_dumb destroy_env = {
        .handle = aux->handle
    };
    drmIoctl(dfb->du->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_env);
    free(aux);
}


static drmu_fb_t *
drmu_fb_new_dumb(drmu_env_t * const du, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_fb_t * const dfb = alloc_fb(du);
    fb_dumb_t * aux;
    uint32_t bpp;

    if (dfb == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }
    dfb->width = (w + 63) & ~63;
    dfb->height = (h + 63) & ~63;
    dfb->cropped = (drmu_rect_t) {.w = w, .h = h};
    dfb->format = format;

    switch (format) {
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_ARGB8888:
            bpp = 32;
            break;
        default:
            drmu_err(du, "%s: Unexpected format %#x", __func__, format);
            goto fail;
    }

    if ((aux = calloc(1, sizeof(*aux))) == NULL) {
        drmu_err(du, "%s: Aux alloc failure", __func__);
        goto fail;
    }

    {
        struct drm_mode_create_dumb dumb = {
            .height = dfb->height,
            .width = dfb->width,
            .bpp = bpp
        };
        if (drmIoctl(du->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0)
        {
            drmu_err(du, "%s: Create dumb %dx%dx%d failed: %s", __func__, w, h, bpp, strerror(errno));
            free(aux);  // After this point aux is bound to dfb and gets freed with it
            goto fail;
        }

        aux->handle = dumb.handle;
        dfb->map_pitch = dumb.pitch;
        dfb->map_size = (size_t)dumb.size;
        dfb->on_delete_v = aux;
        dfb->on_delete_fn = dumb_fb_free_cb;
    }

    {
        struct drm_mode_map_dumb map_dumb = {
            .handle = aux->handle
        };
        if (drmIoctl(du->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) != 0)
        {
            drmu_err(du, "%s: map dumb failed: %s", __func__, strerror(errno));
            goto fail;
        }

        if ((dfb->map_ptr = mmap(NULL, dfb->map_size,
                                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                 du->fd, (off_t)map_dumb.offset)) == MAP_FAILED) {
            drmu_err(du, "%s: mmap failed (size=%zd, fd=%d, off=%zd): %s", __func__,
                     dfb->map_size, du->fd, (size_t)map_dumb.offset, strerror(errno));
            goto fail;
        }
    }

    {
        uint32_t pitches[4] = { dfb->map_pitch };
        uint32_t offsets[4] = { 0 };
        uint32_t bo_handles[4] = { aux->handle };

        if (drmModeAddFB2WithModifiers(du->fd,
                                       dfb->width, dfb->height, dfb->format,
                                       bo_handles, pitches, offsets, NULL,
                                       &dfb->handle, 0) != 0)
        {
            drmu_err(du, "%s: drmModeAddFB2WithModifiers failed: %s\n", __func__, ERRSTR);
            goto fail;
        }
    }

    return dfb;

fail:
    free_fb(dfb);
    return NULL;
}


static drmu_fb_t *
drmu_fb_realloc_dumb(drmu_env_t * const du, drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format)
{
    if (dfb == NULL)
        return drmu_fb_new_dumb(du, w, h, format);

    if (w <= dfb->width && h <= dfb->height && format == dfb->format)
    {
        dfb->cropped = (drmu_rect_t){.w = w, .h = h};
        return dfb;
    }

    drmu_fb_unref(&dfb);
    return drmu_fb_new_dumb(du, w, h, format);
}

// VLC specific helper fb fns
// *** If we make a lib from the drmu fns this should be separated to avoid
//     unwanted library dependancies - For the general case we will need to
//     think harder about how we split this

// Create a new fb from a VLC DRM_PRIME picture.
// Picture is held reffed by the fb until the fb is deleted
static drmu_fb_t *
drmu_fb_vlc_new_pic_attach(drmu_env_t * const du, picture_t * const pic)
{

    uint32_t pitches[4] = { 0 };
    uint32_t offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    uint32_t bo_object_handles[4] = { 0 };
    uint32_t bo_handles[4] = { 0 };
    int i, j, n;
    drmu_fb_t * const dfb = alloc_fb(du);
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);

    if (dfb == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }

    if (desc == NULL) {
        drmu_err(du, "%s: Missing descriptor", __func__);
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

    // Set delete callback & hold this pic
    dfb->on_delete_v = picture_Hold(pic);
    dfb->on_delete_fn = pic_fb_delete_cb;

    // bo handles don't seem to have a close or unref
    for (i = 0; i < desc->nb_objects; ++i)
    {
        if (drmPrimeFDToHandle(du->fd, desc->objects[i].fd, bo_object_handles + i) != 0)
        {
            drmu_warn(du, "%s: drmPrimeFDToHandle[%d](%d) failed: %s", __func__,
                      i, desc->objects[i].fd, ERRSTR);
            goto fail;
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
                                   bo_handles, pitches, offsets, modifiers,
                                   &dfb->handle, DRM_MODE_FB_MODIFIERS /** 0 if no mods */) != 0)
    {
        drmu_err(du, "drmModeAddFB2WithModifiers failed: %s\n", ERRSTR);
        goto fail;
    }

    return dfb;

fail:
    free_fb(dfb);
    return NULL;
}

static plane_t
drmu_fb_vlc_plane(drmu_fb_t * const dfb, const unsigned int plane_n)
{
    if (plane_n != 0)
        return (plane_t){.p_pixels = NULL};

    return (plane_t){
        .p_pixels = dfb->map_ptr,
        .i_lines = dfb->height,
        .i_pitch = dfb->map_pitch,
        .i_pixel_pitch = 4,
        .i_visible_lines = dfb->cropped.h,
        .i_visible_pitch = dfb->cropped.w
    };
}

static void
free_crtc(drmu_crtc_t * const dc)
{
    if (dc->crtc != NULL)
        drmModeFreeCrtc(dc->crtc);
    if (dc->enc != NULL)
        drmModeFreeEncoder(dc->enc);
    if (dc->con != NULL)
        drmModeFreeConnector(dc->con);
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

static int
drmu_plane_set(drmu_plane_t * const dp,
    drmu_fb_t * const dfb, const uint32_t flags,
    const drmu_rect_t pos)
{
    int rv = drmModeSetPlane(dp->du->fd, dp->plane->plane_id, drmu_crtc_id(dp->dc),
        dfb->handle, flags,
        pos.x, pos.y, pos.w, pos.h,
        dfb->cropped.x << 16, dfb->cropped.y << 16, dfb->cropped.w << 16, dfb->cropped.h << 16);
    return rv != 0 ? -errno : 0;
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

    dp->dc = NULL;
}

static drmu_plane_t *
drmu_plane_new_find(drmu_crtc_t * const dc, const uint32_t fmt)
{
    uint32_t i;
    drmu_env_t * const du = dc->du;
    drmu_plane_t * dp = NULL;
    const uint32_t crtc_mask = (uint32_t)1 << dc->crtc_idx;

    for (i = 0; i != du->plane_count && dp == NULL; ++i) {
        uint32_t j;
        const drmModePlane * const p = du->planes[i].plane;

        // In use?
        if (du->planes[i].dc != NULL)
            continue;

        // Availible for this crtc?
        if ((p->possible_crtcs & crtc_mask) == 0)
            continue;

        // Has correct format?
        for (j = 0; j != p->count_formats; ++j) {
            if (p->formats[j] == fmt) {
                dp = du->planes + i;
                break;
            }
        }
    }
    if (dp == NULL) {
        drmu_err(du, "%s: No plane (count=%d) found for fmt %#x", __func__, du->plane_count, fmt);
        return NULL;
    }

    dp->dc = dc;
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

    for (i = 0; i <= du->res->count_crtcs; ++i) {
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
        drmModeFreePlane((drmModePlane*)du->planes[i].plane);
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
        if ((du->planes[i].plane = drmModeGetPlane(du->fd, res->planes[i])) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeGetPlaneResources failed: %s", __func__, strerror(err));
            goto fail2;
        }
        du->planes[i].du = du;

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

typedef struct subpic_ent_s {
    drmu_fb_t * fb;
    drmu_rect_t pos;
} subpic_ent_t;

typedef struct vout_display_sys_t {
    vlc_decoder_device *dec_dev;

    drmu_env_t * du;
    drmu_crtc_t * dc;
    drmu_plane_t * dp;
    drmu_plane_t * subplanes[SUBPICS_MAX];
    subpic_ent_t subpics[SUBPICS_MAX];

    uint32_t con_id;

    unsigned int hold_n;
    drmu_fb_t * hold[HOLD_SIZE];
} vout_display_sys_t;

static const vlc_fourcc_t drm_subpicture_chromas[] = { VLC_CODEC_RGBA, VLC_CODEC_BGRA, VLC_CODEC_ARGB, 0 };

static void vd_drm_prepare(vout_display_t *vd, picture_t *p_pic,
                           subpicture_t *subpicture, vlc_tick_t date)
{
    vout_display_sys_t * const sys = vd->sys;
    unsigned int n = 0;

    VLC_UNUSED(p_pic);
    VLC_UNUSED(date);

    // Attempt to import the subpics
    for (subpicture_t * spic = subpicture; spic != NULL; spic = spic->p_next)
    {
        for (subpicture_region_t *sreg = spic->p_region; sreg != NULL; sreg = sreg->p_next) {
            picture_t * const src = sreg->p_picture;
            subpic_ent_t * const dst = sys->subpics + n;
            plane_t dst_plane;

            dst->fb = drmu_fb_realloc_dumb(sys->du, dst->fb, src->format.i_width, src->format.i_height, DRM_FORMAT_ARGB8888);
            if (dst->fb == NULL) {
                msg_Warn(vd, "Failed alloc for subpic %d: %dx%d", n, src->format.i_width, src->format.i_height);
                continue;
            }

            dst_plane = drmu_fb_vlc_plane(dst->fb, 0);
            plane_CopyPixels(&dst_plane, src->p + 0);

            // *** More transform required
            dst->pos = (drmu_rect_t){
                .x = sreg->i_x,
                .y = sreg->i_y,
                .w = src->format.i_visible_width,
                .h = src->format.i_visible_height,
            };

            if (++n == SUBPICS_MAX)
                goto subpics_done;
        }
    }
subpics_done:

    // Clear any other entries
    for (; n != SUBPICS_MAX; ++n)
        drmu_fb_unref(&sys->subpics[n].fb);

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
}

static void vd_drm_display(vout_display_t *vd, picture_t *p_pic)
{
    vout_display_sys_t *const sys = vd->sys;
    drmu_fb_t ** const dh = sys->hold + sys->hold_n;
    int ret = 0;
    vout_display_place_t place;
    unsigned int i;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
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

    drmu_fb_unref(dh);

    if ((*dh = drmu_fb_vlc_new_pic_attach(sys->du, p_pic)) == NULL) {
        msg_Err(vd, "Failed to create frme buffer from pic");
        return;
    }

    {
        vout_display_cfg_t tcfg = *vd->cfg;
        tcfg.is_display_filled = true;
        tcfg.display.width = drmu_crtc_width(sys->dc);
        tcfg.display.height = drmu_crtc_height(sys->dc);
        vout_display_PlacePicture(&place, vd->source, &tcfg);
    }

    ret = drmu_plane_set(sys->dp, *dh, 0, drmu_rect_vlc_pic_crop(p_pic));

    if (ret != 0)
    {
        msg_Err(vd, "drmModeSetPlane failed: %s", ERRSTR);
    }

    for (i = 0; i != SUBPICS_MAX; ++i) {
        subpic_ent_t * const spe = sys->subpics + i;

        if (spe->fb == NULL)
            continue;

        if ((ret = drmu_plane_set(sys->subplanes[i], spe->fb, 0, spe->pos)) != 0)
        {
            msg_Err(vd, "drmModeSetPlane for subplane %d failed: %s", i, strerror(-ret));
        }
    }

    sys->hold_n = sys->hold_n + 1 >= HOLD_SIZE ? 0 : sys->hold_n + 1;
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
        drmu_fb_unref(sys->hold + i);

    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_plane_delete(sys->subplanes + i);
    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_fb_unref(&sys->subpics[i].fb);

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
        goto fail;

    if ((sys->dc = drmu_crtc_new_find(sys->du)) == NULL)
        goto fail;

    if ((sys->dp = drmu_plane_new_find(sys->dc, DRM_FORMAT_NV12)) == NULL)
        goto fail;

    for (unsigned int i = 0; i != SUBPICS_MAX; ++i) {
        if ((sys->subplanes[i] = drmu_plane_new_find(sys->dc, DRM_FORMAT_ARGB8888)) == NULL) {
            msg_Warn(vd, "Cannot allocate subplane %d", i);
            break;
        }
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


