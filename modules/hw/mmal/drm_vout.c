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
#include <pthread.h>

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
#include <poll.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "pollqueue.h"
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
    int32_t x, y;
    uint32_t w, h;
} drmu_rect_t;

typedef struct drmu_props_s {
    struct drmu_env_s * du;
    unsigned int prop_count;
    drmModePropertyPtr * props;
} drmu_props_t;

// Called pre delete.
// Zero returned means continue delete.
// Non-zero means stop delete - fb will have zero refs so will probably want a new ref
//   before next use
typedef int (* drmu_fb_pre_delete_fn)(struct drmu_fb_s * dfb, void * v);
typedef void (* drmu_fb_on_delete_fn)(struct drmu_fb_s * dfb, void * v);

typedef struct drmu_fb_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init
    struct drmu_fb_s * prev;
    struct drmu_fb_s * next;

    struct drmu_env_s * du;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    drmu_rect_t cropped;
    unsigned int handle;

    void * map_ptr;
    size_t map_size;
    size_t map_pitch;

    void * pre_delete_v;
    drmu_fb_pre_delete_fn pre_delete_fn;

    void * on_delete_v;
    drmu_fb_on_delete_fn on_delete_fn;

} drmu_fb_t;

typedef struct drmu_fb_list_s {
    drmu_fb_t * head;
    drmu_fb_t * tail;
} drmu_fb_list_t;

typedef struct drmu_pool_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init

    struct drmu_env_s * du;

    pthread_mutex_t lock;
    int dead;

    unsigned int fb_count;
    unsigned int fb_max;

    drmu_fb_list_t free_fbs;
} drmu_pool_t;

typedef struct drmu_plane_s {
    struct drmu_env_s * du;
    struct drmu_crtc_s * dc;    // NULL if not in use
    const drmModePlane * plane;

    struct {
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t crtc_h;
        uint32_t crtc_w;
        uint32_t crtc_x;
        uint32_t crtc_y;
        uint32_t src_h;
        uint32_t src_w;
        uint32_t src_x;
        uint32_t src_y;
    } pid;

    // Current, Prev FBs
    drmu_fb_t * fb_cur;
    drmu_fb_t * fb_last;
} drmu_plane_t;

typedef struct drmu_crtc_s {
    struct drmu_env_s * du;
    drmModeCrtcPtr crtc;
    drmModeEncoderPtr enc;
    drmModeConnectorPtr con;
    int crtc_idx;
} drmu_crtc_t;

typedef struct drmu_atomic_fb_op_s {
    drmu_plane_t * plane;
    drmu_fb_t * fb;
} drmu_atomic_fb_op_t;

typedef struct drmu_atomic_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init

    struct drmu_env_s * du;
    drmModeAtomicReqPtr a;

    unsigned int fb_op_count;
    drmu_atomic_fb_op_t * fb_ops;
} drmu_atomic_t;

typedef struct drmu_atomic_q_s {
    pthread_mutex_t lock;
    drmu_atomic_t * next_flip;
    drmu_atomic_t * cur_flip;
    drmu_atomic_t * last_flip;
} drmu_atomic_q_t;

typedef struct drmu_env_s {
    vlc_object_t * log;
    int fd;
    uint32_t plane_count;
    drmu_plane_t * planes;
    drmModeResPtr res;

    // atomic lock held whilst we accumulate atomic ops
    pthread_mutex_t atomic_lock;
    drmu_atomic_t * da;
    // global env for atomic flip
    drmu_atomic_q_t aq;

    struct pollqueue * pq;
    struct polltask * pt;
} drmu_env_t;

static void drmu_fb_unref(drmu_fb_t ** const ppdfb);
static drmu_fb_t * drmu_fb_ref(drmu_fb_t * const dfb);

static uint32_t
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
                return DRM_FORMAT_BGRX8888;
            if (r == 0xff && g == 0xff00 && b == 0xff0000)
                return DRM_FORMAT_RGBX8888;
            if (r == 0xff000000 && g == 0xff0000 && b == 0xff00)
                return DRM_FORMAT_XBGR8888;
            if (r == 0xff00 && g == 0xff0000 && b == 0xff000000)
                return DRM_FORMAT_XRGB8888;
            break;
        }
        case VLC_CODEC_RGB16:
        {
            // VLC RGB16 aka RV16 means we have to look at the mask values
            const uint32_t r = vf_vlc->i_rmask;
            const uint32_t g = vf_vlc->i_gmask;
            const uint32_t b = vf_vlc->i_bmask;
            if (r == 0xf800 && g == 0x7e0 && b == 0x1f)
                return DRM_FORMAT_BGR565;
            if (r == 0x1f && g == 0x7e0 && b == 0xf800)
                return DRM_FORMAT_RGB565;
            break;
        }
        case VLC_CODEC_RGBA:
            return DRM_FORMAT_RGBA8888;
        case VLC_CODEC_BGRA:
            return DRM_FORMAT_BGRA8888;
        case VLC_CODEC_ARGB:
            return DRM_FORMAT_ARGB8888;
        // VLC_CODEC_ABGR does not exist in VLC
        default:
            break;
    }
    return 0;
}

static vlc_fourcc_t
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
        case DRM_FORMAT_RGBA8888:
            return VLC_CODEC_RGBA;
        case DRM_FORMAT_BGRA8888:
            return VLC_CODEC_BGRA;
        case DRM_FORMAT_ARGB8888:
            return VLC_CODEC_ARGB;
        // VLC_CODEC_ABGR does not exist in VLC
        default:
            break;
    }
    return 0;
}


// Get cropping rectangle from a vlc format
static inline drmu_rect_t
drmu_rect_vlc_format_crop(const video_frame_format_t * const format)
{
    return (drmu_rect_t){
        .x = format->i_x_offset,
        .y = format->i_y_offset,
        .w = format->i_visible_width,
        .h = format->i_visible_height};
}

// Get cropping rectangle from a vlc pic
static inline drmu_rect_t
drmu_rect_vlc_pic_crop(const picture_t * const pic)
{
    return drmu_rect_vlc_format_crop(&pic->format);
}

// Get rect from vlc place
static inline drmu_rect_t
drmu_rect_vlc_place(const vout_display_place_t * const place)
{
    return (drmu_rect_t){
        .x = place->x,
        .y = place->y,
        .w = place->width,
        .h = place->height
    };
}

static inline int
rescale_1(int x, int mul, int div)
{
    return div == 0 ? x * mul : (x * mul + div/2) / div;
}

static inline drmu_rect_t
drmu_rect_rescale(const drmu_rect_t s, const drmu_rect_t mul, const drmu_rect_t div)
{
    return (drmu_rect_t){
        .x = rescale_1(s.x - div.x, mul.w, div.w) + mul.x,
        .y = rescale_1(s.y - div.y, mul.h, div.h) + mul.y,
        .w = rescale_1(s.w,         mul.w, div.w),
        .h = rescale_1(s.h,         mul.h, div.h)
    };
}

static inline drmu_rect_t
drmu_rect_add_xy(const drmu_rect_t a, const drmu_rect_t b)
{
    return (drmu_rect_t){
        .x = a.x + b.x,
        .y = a.y + b.y,
        .w = a.w,
        .h = a.h
    };
}

static inline drmu_rect_t
drmu_rect_wh(const unsigned int w, const unsigned int h)
{
    return (drmu_rect_t){
        .w = w,
        .h = h
    };
}

static void
drmu_atomic_fb_ops_flip(drmu_atomic_t * const da)
{
    drmu_atomic_fb_op_t *op = da->fb_ops;
    unsigned int i = 0;

    for (i = 0; i != da->fb_op_count; ++i, ++op) {
        drmu_plane_t * const dp = op->plane;
        drmu_fb_unref(&dp->fb_last);
        dp->fb_last = op->fb;

        op->plane = NULL;
        op->fb = NULL;
    }
}

static void
drmu_atomic_fb_ops_add(drmu_atomic_t * const da, drmu_plane_t * const dp, drmu_fb_t * const dfb)
{
    drmu_atomic_fb_op_t *op = da->fb_ops;
    unsigned int i = 0;

    for (i = 0; i != da->fb_op_count; ++i, ++op) {
        if (op->plane == dp) {
            drmu_fb_unref(&op->fb);
            break;
        }
    }
    if (i == da->fb_op_count)
        da->fb_op_count = i + 1;

    op->plane = dp;
    op->fb = drmu_fb_ref(dfb);
}

static void
drmu_atomic_free(drmu_atomic_t * const da)
{
    for (unsigned int i = 0; i != da->fb_op_count; ++i)
        drmu_fb_unref(&da->fb_ops[i].fb);
    free(da->fb_ops);
    if (da->a != NULL)
        drmModeAtomicFree(da->a);
    free(da);
}

static void
drmu_atomic_unref(drmu_atomic_t ** const ppda)
{
    drmu_atomic_t * const da = *ppda;

    if (da == NULL)
        return;
    *ppda = NULL;

    if (atomic_fetch_sub(&da->ref_count, 1) == 0)
        drmu_atomic_free(da);
}

static drmu_atomic_t *
drmu_atomic_ref(drmu_atomic_t * const da)
{
    atomic_fetch_add(&da->ref_count, 1);
    return da;
}


static int
drmu_atomic_commit(drmu_atomic_t * const da)
{
    drmu_env_t * const du = da->du;

    if (drmModeAtomicCommit(du->fd, da->a,
                            DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                            da) != 0) {
        const int err = errno;
        drmu_err(du, "%s: drmModeAtomicCommit failed: %s", __func__, strerror(err));
        return -err;
    }

    return 0;
}

static void
drmu_atomic_page_flip_cb(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, unsigned int crtc_id, void *user_data)
{
    drmu_atomic_t * const da = user_data;
    drmu_env_t * const du = da->du;
    drmu_atomic_q_t * const aq = &du->aq;

    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;
    (void)crtc_id;

    pthread_mutex_lock(&aq->lock);

    if (da != aq->cur_flip) {
        drmu_err(du, "%s: User data el (%p) != cur (%p)", __func__, da, aq->cur_flip);
    }

    drmu_atomic_unref(&aq->last_flip);
    aq->last_flip = aq->cur_flip;
    aq->cur_flip = NULL;

    if (aq->next_flip != NULL) {
        if (drmu_atomic_commit(aq->next_flip) == 0) {
            aq->cur_flip = aq->next_flip;
            aq->next_flip = NULL;

            // Sort out fb references
            drmu_atomic_fb_ops_flip(da);
        }
        else {
            drmu_atomic_unref(&aq->next_flip);
        }
    }

    pthread_mutex_unlock(&aq->lock);
}

static int
drmu_atomic_queue(drmu_atomic_t * da)
{
    int rv = 0;
    drmu_env_t * const du = da->du;
    drmu_atomic_q_t * const aq = &du->aq;

    pthread_mutex_lock(&aq->lock);

    if (aq->next_flip != NULL || aq->cur_flip != NULL) {
        drmu_atomic_unref(&aq->next_flip);
        aq->next_flip = drmu_atomic_ref(da);
    }
    else {
        // Mutex makes commit/asignment order safe
        if ((rv = drmu_atomic_commit(da)) == 0)
            aq->cur_flip = drmu_atomic_ref(da);
    }

    pthread_mutex_unlock(&aq->lock);
    return rv;
}

static void
drmu_atomic_q_uninit(drmu_atomic_q_t * const aq)
{
    pthread_mutex_destroy(&aq->lock);
}

static void
drmu_atomic_q_init(drmu_atomic_q_t * const aq)
{
    aq->next_flip = NULL;
    pthread_mutex_init(&aq->lock, NULL);
}

static int
drmu_atomic_add_prop(drmu_atomic_t * const da, const uint32_t obj_id, const uint32_t prop_id, const uint64_t value)
{
    if (drmModeAtomicAddProperty(da->a, obj_id, prop_id, value) < 0)
        drmu_warn(da->du, "%s: Failed to set obj_id=%#x, prop_id=%#x, val=%" PRId64, __func__,
                 obj_id, prop_id, value);
    return 0;
}

static drmu_atomic_t *
drmu_atomic_new(drmu_env_t * const du)
{
    drmu_atomic_t * const da = calloc(1, sizeof(*da));

    if (da == NULL) {
        drmu_err(du, "%s: Failed to alloc struct", __func__);
        return NULL;
    }
    da->du = du;

    if ((da->fb_ops = calloc(du->plane_count, sizeof(*da->fb_ops))) == NULL) {
        drmu_err(du, "%s: Failed to alloc ops", __func__);
        goto fail;
    }

    if ((da->a = drmModeAtomicAlloc()) == NULL) {
        drmu_err(du, "%s: Failed to alloc atomic context", __func__);
        goto fail;
    }

    return da;

fail:
    drmu_atomic_free(da);
    return NULL;
}

static void
free_fb(drmu_fb_t * const dfb)
{
    drmu_env_t * const du = dfb->du;

    if (dfb->pre_delete_fn && dfb->pre_delete_fn(dfb, dfb->pre_delete_v) != 0)
        return;

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

// Beware: used by pool fns
static void
drmu_fb_pre_delete_set(drmu_fb_t *const dfb, drmu_fb_pre_delete_fn fn, void * v)
{
    dfb->pre_delete_fn = fn;
    dfb->pre_delete_v  = v;
}

static void
drmu_fb_pre_delete_unset(drmu_fb_t *const dfb)
{
    dfb->pre_delete_fn = (drmu_fb_pre_delete_fn)0;
    dfb->pre_delete_v  = NULL;
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
    if (dfb != NULL)
        atomic_fetch_add(&dfb->ref_count, 1);
    return dfb;
}

static unsigned int
drmu_fb_pixel_bits(const drmu_fb_t * const dfb, unsigned int plane_n)
{
    switch (dfb->format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
            return plane_n == 0 ? 32 : 0;
        default:
            break;
    }
    return 0;
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
    dfb->cropped = drmu_rect_wh(w, h);
    dfb->format = format;

    if ((bpp = drmu_fb_pixel_bits(dfb, 0)) == 0) {
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

static int
fb_try_reuse(drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format)
{
    if (w > dfb->width || h > dfb->height || format != dfb->format)
        return 0;

    dfb->cropped = drmu_rect_wh(w, h);
    return 1;
}

static drmu_fb_t *
drmu_fb_realloc_dumb(drmu_env_t * const du, drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format)
{
    if (dfb == NULL)
        return drmu_fb_new_dumb(du, w, h, format);

    if (fb_try_reuse(dfb, w, h, format))
        return dfb;

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
    const unsigned int bpp = drmu_fb_pixel_bits(dfb, plane_n);

    if (plane_n != 0 || bpp == 0)
        return (plane_t){.p_pixels = NULL};

    return (plane_t){
        .p_pixels = dfb->map_ptr,
        .i_lines = dfb->height,
        .i_pitch = dfb->map_pitch,
        .i_pixel_pitch = bpp / 8,
        .i_visible_lines = dfb->cropped.h,
        .i_visible_pitch = dfb->cropped.w * bpp / 8
    };
}

static void
fb_list_add_tail(drmu_fb_list_t * const fbl, drmu_fb_t * const dfb)
{
    assert(dfb->prev == NULL && dfb->next == NULL);

    if (fbl->tail == NULL)
        fbl->head = dfb;
    else
        fbl->tail->next = dfb;
    dfb->prev = fbl->tail;
    fbl->tail = dfb;
}

static drmu_fb_t *
fb_list_extract(drmu_fb_list_t * const fbl, drmu_fb_t * const dfb)
{
    if (dfb == NULL)
        return NULL;

    if (dfb->prev == NULL)
        fbl->head = dfb->next;
    else
        dfb->prev->next = dfb->next;

    if (dfb->next == NULL)
        fbl->tail = dfb->prev;
    else
        dfb->next->prev = dfb->prev;

    dfb->next = NULL;
    dfb->prev = NULL;
    return dfb;
}

static drmu_fb_t *
fb_list_extract_head(drmu_fb_list_t * const fbl)
{
    return fb_list_extract(fbl, fbl->head);
}

static drmu_fb_t *
fb_list_peek_head(drmu_fb_list_t * const fbl)
{
    return fbl->head;
}

static bool
fb_list_is_empty(drmu_fb_list_t * const fbl)
{
    return fbl->head == NULL;
}

static void
pool_free_pool(drmu_pool_t * const pool)
{
    drmu_fb_t * dfb;
    while ((dfb = fb_list_extract_head(&pool->free_fbs)) != NULL)
        drmu_fb_unref(&dfb);
}

static void
pool_free(drmu_pool_t * const pool)
{
    pool_free_pool(pool);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

static void
drmu_pool_unref(drmu_pool_t ** const pppool)
{
    drmu_pool_t * const pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    if (atomic_fetch_sub(&pool->ref_count, 1) != 0)
        return;

    pool_free(pool);
}

static drmu_pool_t *
drmu_pool_ref(drmu_pool_t * const pool)
{
    atomic_fetch_add(&pool->ref_count, 1);
    return pool;
}

static drmu_pool_t *
drmu_pool_new(drmu_env_t * const du, unsigned int total_fbs_max)
{
    drmu_pool_t * const pool = calloc(1, sizeof(*pool));

    if (pool == NULL) {
        drmu_err(du, "Failed pool env alloc");
        return NULL;
    }

    pool->du = du;
    pool->fb_max = total_fbs_max;
    pthread_mutex_init(&pool->lock, NULL);

    return pool;
}

static int
pool_fb_pre_delete_cb(drmu_fb_t * dfb, void * v)
{
    drmu_pool_t * pool = v;

    // Ensure we cannot end up in a delete loop
    drmu_fb_pre_delete_unset(dfb);

    // If dead set then might as well delete now
    // It should all work without this shortcut but this reclaims
    // storage quicker
    if (pool->dead) {
        drmu_pool_unref(&pool);
        return 0;
    }

    drmu_fb_ref(dfb);  // Restore ref
    fb_list_add_tail(&pool->free_fbs, dfb);
    // May cause suicide & recursion on fb delete, but that should be OK as
    // the 1 we return here should cause simple exit of fb delete
    drmu_pool_unref(&pool);
    return 1;  // Stop delete
}

static drmu_fb_t *
drmu_pool_fb_new_dumb(drmu_pool_t * const pool, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_env_t * const du = pool->du;
    drmu_fb_t * dfb = fb_list_peek_head(&pool->free_fbs);

    while (dfb != NULL) {
        if (fb_try_reuse(dfb, w, h, format)) {
            fb_list_extract(&pool->free_fbs, dfb);
            break;
        }
        dfb = dfb->next;
    }

    if (dfb == NULL) {
        if (pool->fb_count >= pool->fb_max && !fb_list_is_empty(&pool->free_fbs)) {
            --pool->fb_count;
            dfb = fb_list_extract_head(&pool->free_fbs);
            drmu_fb_unref(&dfb);
        }

        ++pool->fb_count;
        dfb = drmu_fb_realloc_dumb(du, NULL, w, h, format);
    }

    drmu_fb_pre_delete_set(dfb, pool_fb_pre_delete_cb, pool);
    drmu_pool_ref(pool);
    return dfb;
}

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
static void
drmu_pool_delete(drmu_pool_t ** const pppool)
{
    drmu_pool_t * pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    pool->dead = 1;
    pool_free_pool(pool);

    drmu_pool_unref(&pool);
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
plane_set_atomic(drmu_atomic_t * const da,
                 drmu_plane_t * const dp,
                 drmu_fb_t * const dfb,
                int32_t crtc_x, int32_t crtc_y,
                uint32_t crtc_w, uint32_t crtc_h,
                uint32_t src_x, uint32_t src_y,
                uint32_t src_w, uint32_t src_h)
{
    const uint32_t plid = dp->plane->plane_id;
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_id, dfb == NULL ? 0 : drmu_crtc_id(dp->dc));
    drmu_atomic_add_prop(da, plid, dp->pid.fb_id,  dfb == NULL ? 0 : dfb->handle);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_x, crtc_x);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_y, crtc_y);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_w, crtc_w);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_h, crtc_h);
    drmu_atomic_add_prop(da, plid, dp->pid.src_x,  src_x);
    drmu_atomic_add_prop(da, plid, dp->pid.src_y,  src_y);
    drmu_atomic_add_prop(da, plid, dp->pid.src_w,  src_w);
    drmu_atomic_add_prop(da, plid, dp->pid.src_h,  src_h);

    // Remember for ref counting
    drmu_atomic_fb_ops_add(da, dp, dfb);
    return 0;
}

static int
drmu_plane_set(drmu_plane_t * const dp,
    drmu_fb_t * const dfb, const uint32_t flags,
    const drmu_rect_t pos)
{
    int rv;
    drmu_env_t * const du = dp->du;

    if (du->da != NULL) {
        if (dfb == NULL) {
            rv = plane_set_atomic(du->da, dp, NULL,
                                  0, 0, 0, 0,
                                  0, 0, 0, 0);
        }
        else {
            rv = plane_set_atomic(du->da, dp, dfb,
                                  pos.x, pos.y, pos.w, pos.h,
                                  dfb->cropped.x << 16, dfb->cropped.y << 16, dfb->cropped.w << 16, dfb->cropped.h << 16);
        }
    }
    else {
        if (dfb == NULL) {
            rv = drmModeSetPlane(du->fd, dp->plane->plane_id, drmu_crtc_id(dp->dc),
                0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0);
        }
        else {
            rv = drmModeSetPlane(du->fd, dp->plane->plane_id, drmu_crtc_id(dp->dc),
                dfb->handle, flags,
                pos.x, pos.y, pos.w, pos.h,
                dfb->cropped.x << 16, dfb->cropped.y << 16, dfb->cropped.w << 16, dfb->cropped.h << 16);
        }
        if (rv == 0) {
            drmu_fb_unref(&dp->fb_last);
            dp->fb_last = dp->fb_cur;
            dp->fb_cur = drmu_fb_ref(dfb);
        }
    }
    return rv != 0 ? -errno : 0;
}

static inline uint32_t
drmu_plane_id(const drmu_plane_t * const dp)
{
    return dp->plane->plane_id;
}

static const uint32_t *
drmu_plane_formats(const drmu_plane_t * const dp, unsigned int * const pCount)
{
    *pCount = dp->plane->count_formats;
    return dp->plane->formats;
}

static void
drmu_plane_delete(drmu_plane_t ** const ppdp)
{
    drmu_plane_t * const dp = *ppdp;

    if (dp == NULL)
        return;
    *ppdp = NULL;

    // ??? Ensure we kill the plane on the display ???
    drmu_fb_unref(&dp->fb_cur);
    drmu_fb_unref(&dp->fb_last);

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

static int
drmu_env_atomic_start(drmu_env_t * const du)
{
    pthread_mutex_lock(&du->atomic_lock);
    if ((du->da = drmu_atomic_new(du)) == NULL) {
        pthread_mutex_unlock(&du->atomic_lock);
        return -ENOMEM;
    }
    return 0;
}

static void
drmu_env_atomic_abort(drmu_env_t * const du)
{
    if (du->da == NULL)
        return;

    drmu_atomic_unref(&du->da);
    pthread_mutex_unlock(&du->atomic_lock);
}

static drmu_atomic_t *
drmu_env_atomic_finish(drmu_env_t * const du)
{
    drmu_atomic_t * const da = du->da;
    du->da = NULL;
    pthread_mutex_unlock(&du->atomic_lock);
    return da;
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

static void
props_free(drmu_props_t * const props)
{
    unsigned int i;
    for (i = 0; i != props->prop_count; ++i)
        drmModeFreeProperty(props->props[i]);
    free(props);
}

static uint32_t
props_name_to_id(drmu_props_t * const props, const char * const name)
{
    unsigned int i = props->prop_count / 2;
    unsigned int a = 0;
    unsigned int b = props->prop_count;

    while (a < b) {
        const int r = strcmp(name, props->props[i]->name);

        if (r == 0)
            return props->props[i]->prop_id;

        if (r < 0) {
            b = i;
            i = (i + a) / 2;
        } else {
            a = i + 1;
            i = (i + b) / 2;
        }
    }
    return 0;
}

static void
props_dump(const drmu_props_t * const props)
{
    unsigned int i;
    drmu_env_t * const du = props->du;

    for (i = 0; i != props->prop_count; ++i) {
        drmModePropertyPtr p = props->props[i];
        drmu_info(du, "Prop%d/%d: id=%#x, name=%s", i, props->prop_count, p->prop_id, p->name);
    }
}

static int
props_qsort_cb(const void * va, const void * vb)
{
    const drmModePropertyPtr a = *(drmModePropertyPtr *)va;
    const drmModePropertyPtr b = *(drmModePropertyPtr *)vb;
    return strcmp(a->name, b->name);
}

static drmu_props_t *
props_new(drmu_env_t * const du, const uint32_t objid, const uint32_t objtype)
{
    drmu_props_t * const props = calloc(1, sizeof(*props));
    drmModeObjectProperties * objprops;
    int err;
    unsigned int i;

    if (props == NULL) {
        drmu_err(du, "%s: Failed struct alloc", __func__);
        return NULL;
    }
    props->du = du;

    if ((objprops = drmModeObjectGetProperties(du->fd, objid, objtype)) == NULL) {
        err = errno;
        drmu_err(du, "%s: drmModeObjectGetProperties failed: %s", __func__, strerror(err));
        return NULL;
    }

    if ((props->props = calloc(objprops->count_props, sizeof(*props))) == NULL) {
        drmu_err(du, "%s: Failed array alloc", __func__);
        goto fail1;
    }

    for (i = 0; i != objprops->count_props; ++i) {
        if ((props->props[i] = drmModeGetProperty(du->fd, objprops->props[i])) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeGetPropertiy %#x failed: %s", __func__, objprops->props[i], strerror(err));
            goto fail2;
        }
        ++props->prop_count;
    }

    // Sort into name order for faster lookup
    qsort(props->props, props->prop_count, sizeof(*props->props), props_qsort_cb);

    return props;

fail2:
    props_free(props);
fail1:
    drmModeFreeObjectProperties(objprops);
    return NULL;
}

static int
drmu_env_planes_populate(drmu_env_t * const du)
{
    int err = EINVAL;
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
        drmu_plane_t * const dp = du->planes + i;
        drmu_props_t *props;

        dp->du = du;

        if ((dp->plane = drmModeGetPlane(du->fd, res->planes[i])) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeGetPlane failed: %s", __func__, strerror(err));
            goto fail2;
        }

        if ((props = props_new(du, dp->plane->plane_id, DRM_MODE_OBJECT_PLANE)) == NULL) {
            err = errno;
            drmu_err(du, "%s: drmModeObjectGetProperties failed: %s", __func__, strerror(err));
            goto fail2;
        }

        if ((dp->pid.crtc_id = props_name_to_id(props, "CRTC_ID")) == 0 ||
            (dp->pid.fb_id  = props_name_to_id(props, "FB_ID")) == 0 ||
            (dp->pid.crtc_h = props_name_to_id(props, "CRTC_H")) == 0 ||
            (dp->pid.crtc_w = props_name_to_id(props, "CRTC_W")) == 0 ||
            (dp->pid.crtc_x = props_name_to_id(props, "CRTC_X")) == 0 ||
            (dp->pid.crtc_y = props_name_to_id(props, "CRTC_Y")) == 0 ||
            (dp->pid.src_h  = props_name_to_id(props, "SRC_H")) == 0 ||
            (dp->pid.src_w  = props_name_to_id(props, "SRC_W")) == 0 ||
            (dp->pid.src_x  = props_name_to_id(props, "SRC_X")) == 0 ||
            (dp->pid.src_y  = props_name_to_id(props, "SRC_Y")) == 0)
        {
            drmu_err(du, "%s: failed to find required id", __func__);
            props_free(props);
            goto fail2;
        }

        props_free(props);
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

    pollqueue_delete(&du->pq);
    polltask_delete(&du->pt);

    if (du->res != NULL)
        drmModeFreeResources(du->res);
    free_planes(du);

    close(du->fd);
    drmu_atomic_q_uninit(&du->aq);
    pthread_mutex_destroy(&du->atomic_lock);
    free(du);
}

static void
drmu_env_polltask_cb(void * v, short revents)
{
    drmu_env_t * const du = v;
    drmEventContext ctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler2 = drmu_atomic_page_flip_cb,
    };

    if (revents == 0) {
        drmu_warn(du, "%s: Timeout", __func__);
    }
    else {
        drmu_warn(du, "%s: Handle", __func__);
        drmHandleEvent(du->fd, &ctx);
    }

    pollqueue_add_task(du->pq, du->pt, 1000);
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
    pthread_mutex_init(&du->atomic_lock, NULL);
    drmu_atomic_q_init(&du->aq);

    if ((du->pq = pollqueue_new()) == NULL) {
        drmu_err(du, "Failed to create pollqueue");
        goto fail1;
    }
    if ((du->pt = polltask_new(du->fd, POLLIN | POLLPRI, drmu_env_polltask_cb, du)) == NULL) {
        drmu_err(du, "Failed to create polltask");
        goto fail1;
    }

    // We want the primary plane for video
    drmSetClientCap(du->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(du->fd, DRM_CLIENT_CAP_ATOMIC, 1);

    if (drmu_env_planes_populate(du) != 0)
        goto fail1;

    if ((du->res = drmModeGetResources(du->fd)) == NULL) {
        drmu_err(du, "%s: Failed to get resources", __func__);
        goto fail1;
    }

    pollqueue_add_task(du->pq, du->pt, 1000);

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
    drmu_rect_t space;  // display space of pos
} subpic_ent_t;

typedef struct vout_display_sys_t {
    vlc_decoder_device *dec_dev;

    drmu_env_t * du;
    drmu_crtc_t * dc;
    drmu_plane_t * dp;
    drmu_pool_t * sub_fb_pool;
    drmu_plane_t * subplanes[SUBPICS_MAX];
    subpic_ent_t subpics[SUBPICS_MAX];
    vlc_fourcc_t * subpic_chromas;

    uint32_t con_id;
} vout_display_sys_t;

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
            const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format);

            if (drm_fmt == 0) {
                msg_Warn(vd, "Failed drm format for subpic %d: %#x", n, src->format.i_chroma);
                continue;
            }

            if (dst->fb != NULL) {
                // Should never happen - fbs consumed by display
                msg_Warn(vd, "Subpic %d: still has fb attached", n);
                drmu_fb_unref(&dst->fb);
            }

            dst->fb = drmu_pool_fb_new_dumb(sys->sub_fb_pool, src->format.i_width, src->format.i_height, drm_fmt);
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

//            msg_Info(vd, "Orig: %dx%d", spic->i_original_picture_width, spic->i_original_picture_height);
            dst->space = drmu_rect_wh(spic->i_original_picture_width, spic->i_original_picture_height);

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
    drmu_fb_t * dfb;
    int ret = 0;
    drmu_rect_t r;
    unsigned int i;
    drmu_atomic_t * da;

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

    {
        vout_display_place_t place;
        vout_display_PlacePicture(&place, vd->source, vd->cfg);
        r = drmu_rect_vlc_place(&place);
    }

    if ((dfb = drmu_fb_vlc_new_pic_attach(sys->du, p_pic)) == NULL) {
        msg_Err(vd, "Failed to create frme buffer from pic");
        return;
    }

    drmu_env_atomic_start(sys->du);

    ret = drmu_plane_set(sys->dp, dfb, 0, r);
    drmu_fb_unref(&dfb);

    if (ret != 0)
    {
        msg_Err(vd, "drmModeSetPlane failed: %s", ERRSTR);
    }

    for (i = 0; i != SUBPICS_MAX; ++i) {
        subpic_ent_t * const spe = sys->subpics + i;

//        msg_Info(vd, "pic=%dx%d @ %d,%d, r=%dx%d @ %d,%d, space=%dx%d @ %d,%d",
//                 spe->pos.w, spe->pos.h, spe->pos.x, spe->pos.y,
//                 r.w, r.h, r.x, r.y,
//                 spe->space.w, spe->space.h, spe->space.x, spe->space.y);

        // Rescale from sub-space
        if ((ret = drmu_plane_set(sys->subplanes[i], spe->fb, 0,
                                  drmu_rect_rescale(spe->pos, r, spe->space))) != 0)
        {
            msg_Err(vd, "drmModeSetPlane for subplane %d failed: %s", i, strerror(-ret));
        }
        drmu_fb_unref(&spe->fb);
    }

    da = drmu_env_atomic_finish(sys->du);

    drmu_atomic_queue(da);

    drmu_atomic_unref(&da);

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

    drmu_pool_delete(&sys->sub_fb_pool);

    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_plane_delete(sys->subplanes + i);
    for (i = 0; i != SUBPICS_MAX; ++i)
        drmu_fb_unref(&sys->subpics[i].fb);

    drmu_plane_delete(&sys->dp);
    drmu_crtc_delete(&sys->dc);
    drmu_env_delete(&sys->du);

    if (sys->dec_dev)
        vlc_decoder_device_Release(sys->dec_dev);

    free(sys->subpic_chromas);
    vd->info.subpicture_chromas = NULL;

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

    if ((sys->sub_fb_pool = drmu_pool_new(sys->du, 10)) == NULL)
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
// systems s.t. system message are in display space and subs are in source
// with no way of distinguishing so we don't know what to scale by :-(
//        .can_scale_spu = true,
        .subpicture_chromas = sys->subpic_chromas
    };

    vd->ops = &ops;

    vout_display_SetSize(vd, drmu_crtc_width(sys->dc), drmu_crtc_height(sys->dc));
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


