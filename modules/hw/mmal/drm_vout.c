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

#define DRM_VOUT_SOURCE_MODESET_NAME "drm-vout-source-modeset"
#define DRM_VOUT_SOURCE_MODESET_TEXT N_("Attempt to match display to source")
#define DRM_VOUT_SOURCE_MODESET_LONGTEXT N_("Attempt to match display resolution and refresh rate to source.\
 Defaults to the 'preferred' mode if no good enough match found. \
 If unset then resolution & refresh will not be set.")

#define DRM_VOUT_NO_MODESET_NAME "drm-vout-no-modeset"
#define DRM_VOUT_NO_MODESET_TEXT N_("Do not modeset")
#define DRM_VOUT_NO_MODESET_LONGTEXT N_("Do no operation that would cause a modeset.\
 This overrides the operatino of all other flags.")

#define DRM_VOUT_NO_MAX_BPC "drm-vout-no-max-bpc"
#define DRM_VOUT_NO_MAX_BPC_TEXT N_("Do not set bpc on output")
#define DRM_VOUT_NO_MAX_BPC_LONGTEXT N_("Do not try to switch from 8-bit RGB to 12-bit YCC on UHD frames.\
 12 bit is dependant on kernel and display support so may not be availible")


// HDR enums is copied from linux include/linux/hdmi.h (strangely not part of uapi)
enum hdmi_metadata_type
{
    HDMI_STATIC_METADATA_TYPE1 = 0,
};
enum hdmi_eotf
{
    HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
    HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
    HDMI_EOTF_SMPTE_ST2084,
    HDMI_EOTF_BT_2100_HLG,
};

#define TRACE_ALL 0
#define TRACE_PROP_NEW 0

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

typedef struct drmu_ufrac_s {
    unsigned int num;
    unsigned int den;
} drmu_ufrac_t;

typedef struct drmu_props_s {
    struct drmu_env_s * du;
    unsigned int prop_count;
    drmModePropertyPtr * props;
} drmu_props_t;

typedef struct drmu_prop_enum_s {
    uint32_t id;
    uint32_t flags;
    unsigned int n;
    const struct drm_mode_property_enum * enums;
    char name[DRM_PROP_NAME_LEN];
} drmu_prop_enum_t;

typedef struct drmu_prop_range_s {
    uint32_t id;
    uint32_t flags;
    uint64_t range[2];
    char name[DRM_PROP_NAME_LEN];
} drmu_prop_range_t;

typedef struct drmu_blob_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init
    struct drmu_env_s * du;
    uint32_t blob_id;
} drmu_blob_t;

// Called pre delete.
// Zero returned means continue delete.
// Non-zero means stop delete - fb will have zero refs so will probably want a new ref
//   before next use
typedef int (* drmu_fb_pre_delete_fn)(struct drmu_fb_s * dfb, void * v);
typedef void (* drmu_fb_on_delete_fn)(struct drmu_fb_s * dfb, void * v);

enum drmu_bo_type_e {
    BO_TYPE_NONE = 0,
    BO_TYPE_FD,
    BO_TYPE_DUMB
};

typedef struct drmu_bo_s {
    int ref_count;  // ref counted under bo_env lock so no need for atomic
    struct drmu_bo_s * next;
    struct drmu_bo_s * prev;
    struct drmu_env_s * du;
    enum drmu_bo_type_e bo_type;
    uint32_t handle;
} drmu_bo_t;

typedef struct drmu_bo_env_s {
    pthread_mutex_t lock;
    drmu_bo_t * fd_head;
} drmu_bo_env_t;

typedef void (* drmu_prop_del_fn)(void * v);
typedef void (* drmu_prop_ref_fn)(void * v);

// Atomic property chain structures - no external visibility
typedef struct aprop_prop_s {
    uint32_t id;
    uint64_t value;
    void * v;
    drmu_prop_ref_fn ref_fn;
    drmu_prop_del_fn del_fn;
} aprop_prop_t;

typedef struct aprop_obj_s {
    uint32_t id;
    unsigned int n;
    unsigned int size;
    aprop_prop_t * props;
} aprop_obj_t;

typedef struct aprop_hdr_s {
    unsigned int n;
    unsigned int size;
    aprop_obj_t * objs;
} aprop_hdr_t;

typedef enum drmu_isset_e {
    DRMU_ISSET_UNSET = 0,  // Thing unset
    DRMU_ISSET_NULL,       // Thing is empty
    DRMU_ISSET_SET,        // Thing has valid data
} drmu_isset_t;

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

    uint32_t pitches[4];
    uint32_t offsets[4];
    drmu_bo_t * bo_list[4];

    const char * color_encoding; // Assumed to be constant strings that don't need freeing
    const char * color_range;

    // Do not set colorspace or metadata if not the "master" plane
    const char * colorspace;
    drmu_isset_t hdr_metadata_isset;
    struct hdr_output_metadata hdr_metadata;

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

    unsigned int seq;  // debug

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
        drmu_prop_enum_t * color_encoding;
        drmu_prop_enum_t * color_range;
    } pid;

} drmu_plane_t;

typedef int (* drmu_mode_score_fn)(void * v, const drmModeModeInfo * mode);

typedef struct drmu_crtc_s {
    struct drmu_env_s * du;
    drmModeCrtcPtr crtc;
    drmModeEncoderPtr enc;
    drmModeConnectorPtr con;
    int crtc_idx;
    bool hi_bpc_ok;
    drmu_ufrac_t sar;
    drmu_ufrac_t par;

    struct {
        // crtc
        uint32_t mode_id;
        // connection
        drmu_prop_range_t * max_bpc;
        drmu_prop_enum_t * colorspace;
        drmu_prop_enum_t * output_format;
        uint32_t hdr_output_metadata;
    } pid;

    int cur_mode_id;
    drmu_blob_t * mode_id_blob;
    drmu_blob_t * hdr_metadata_blob;
    struct hdr_output_metadata hdr_metadata;

} drmu_crtc_t;

typedef struct drmu_atomic_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init

    struct drmu_env_s * du;

    aprop_hdr_t props;
} drmu_atomic_t;

typedef struct drmu_atomic_q_s {
    pthread_mutex_t lock;
    drmu_atomic_t * next_flip;
    drmu_atomic_t * cur_flip;
    drmu_atomic_t * last_flip;
    unsigned int retry_count;
    struct polltask * retry_task;
} drmu_atomic_q_t;

typedef struct drmu_env_s {
    vlc_object_t * log;
    int fd;
    uint32_t plane_count;
    drmu_plane_t * planes;
    drmModeResPtr res;

    // global env for atomic flip
    drmu_atomic_q_t aq;
    // global env for bo tracking
    drmu_bo_env_t boe;

    struct pollqueue * pq;
    struct polltask * pt;
} drmu_env_t;

static void drmu_fb_unref(drmu_fb_t ** const ppdfb);
static drmu_fb_t * drmu_fb_ref(drmu_fb_t * const dfb);

static inline unsigned int
max_uint(const unsigned int a, const unsigned int b)
{
    return a < b ? b : a;
}


// N.B. DRM seems to order its format descriptor names the opposite way round to VLC
// DRM is hi->lo within a little-endian word, VLC is byte order

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

static drmu_ufrac_t
drmu_ufrac_reduce(drmu_ufrac_t x)
{
    static const unsigned int primes[] = {2,3,5,7,11,13,17,19,23,0};
    const unsigned int * p;
    for (p = primes; *p != 0; ++p) {
        while (x.den % *p == 0 && x.num % *p ==0) {
            x.den /= *p;
            x.num /= *p;
        }
    }
    return x;
}

static inline vlc_rational_t
drmu_ufrac_vlc_to_rational(const drmu_ufrac_t x)
{
    return (vlc_rational_t) {.num = x.num, .den = x.den};
}

static void
blob_free(drmu_blob_t * const blob)
{
    drmu_env_t * const du = blob->du;

    if (blob->blob_id != 0) {
        struct drm_mode_destroy_blob dblob = {
            .blob_id = blob->blob_id
        };
        if (drmIoctl(du->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &dblob) != 0)
            drmu_err(du, "%s: Failed to destroy blob: %s", __func__, strerror(errno));
    }
    free(blob);
}

static void
drmu_blob_unref(drmu_blob_t ** const ppBlob)
{
    drmu_blob_t * const blob = *ppBlob;

    if (blob == NULL)
        return;
    *ppBlob = NULL;

    if (atomic_fetch_sub(&blob->ref_count, 1) != 0)
        return;

    blob_free(blob);
}

static uint32_t
drmu_blob_id(const drmu_blob_t * const blob)
{
    return blob == NULL ? 0 : blob->blob_id;
}

static drmu_blob_t *
drmu_blob_ref(drmu_blob_t * const blob)
{
    if (blob != NULL)
        atomic_fetch_add(&blob->ref_count, 1);
    return blob;
}

static drmu_blob_t *
drmu_blob_new(drmu_env_t * const du, const void * const data, const size_t len)
{
    int rv;
    drmu_blob_t * blob = calloc(1, sizeof(*blob));
    struct drm_mode_create_blob cblob = {
        .data = (uintptr_t)data,
        .length = (uint32_t)len,
        .blob_id = 0
    };

    if (blob == NULL) {
        drmu_err(du, "%s: Unable to alloc blob", __func__);
        return NULL;
    }
    blob->du = du;

    if ((rv = drmIoctl(du->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cblob)) != 0) {
        drmu_err(du, "%s: Unable to create blob: data=%p, len=%zu: %s", __func__,
                 data, len, strerror(errno));
        blob_free(blob);
        return NULL;
    }

    atomic_init(&blob->ref_count, 0);
    blob->blob_id = cblob.blob_id;
    return blob;
}

static void
aprop_prop_unref(aprop_prop_t * const pp)
{
    if (pp->del_fn)
        pp->del_fn(pp->v);
}

static void
aprop_prop_ref(aprop_prop_t * const pp)
{
    if (pp->ref_fn)
        pp->ref_fn(pp->v);
}

static void
aprop_obj_uninit(aprop_obj_t * const po)
{
    unsigned int i;
    for (i = 0; i != po->n; ++i)
        aprop_prop_unref(po->props + i);
    free(po->props);
}

static int
aprop_obj_copy(aprop_obj_t * const po_c, const aprop_obj_t * const po_a)
{
    unsigned int i;
    aprop_prop_t * props;

    aprop_obj_uninit(po_c);
    if (po_a->n == 0)
        return 0;

    if ((props = calloc(po_a->size, sizeof(*props))) == NULL)
        return -ENOMEM;
    memcpy(props, po_a->props, po_a->n * sizeof(*po_a->props));

    *po_c = *po_a;
    po_c->props = props;

    for (i = 0; i != po_a->n; ++i)
        aprop_prop_ref(props + i);
    return 0;
}

static int
aprop_obj_move(aprop_obj_t * const po_c, aprop_obj_t * const po_a)
{
    *po_c = *po_a;
    memset(po_a, 0, sizeof(*po_a));
    return 0;
}

static int
aprop_prop_qsort_cb(const void * va, const void * vb)
{
    const aprop_prop_t * const a = va;
    const aprop_prop_t * const b = vb;
    return a->id == b->id ? 0 : a->id < b->id ? -1 : 1;
}

// Merge b into a. b is zeroed on exit so must be discarded
static int
aprop_obj_merge(aprop_obj_t * const po_c, aprop_obj_t * const po_a, aprop_obj_t * const po_b)
{
    unsigned int i, j, k;
    unsigned int c_size;
    aprop_prop_t * c;
    aprop_prop_t * const a = po_a->props;
    aprop_prop_t * const b = po_b->props;

    // As we should have no identical els we don't care that qsort is unstable
    qsort(a, po_a->n, sizeof(a[0]), aprop_prop_qsort_cb);
    qsort(b, po_b->n, sizeof(b[0]), aprop_prop_qsort_cb);

    // Pick a size
    c_size = max_uint(po_a->size, po_b->size);
    if (c_size < po_a->n + po_b->n)
        c_size *= 2;
    if ((c = calloc(c_size, sizeof(*c))) == NULL)
        return -ENOMEM;

    for (i = 0, j = 0, k = 0; i < po_a->n && j < po_a->n; ++k) {
        if (a[i].id < b[j].id)
            c[k] = a[i++];
        else if (a[i].id > b[j].id)
            c[k] = b[j++];
        else {
            c[k] = b[j++];
            aprop_prop_unref(a + i++);
        }
    }
    for (; i < po_a->n; ++i, ++k)
        c[k] = a[i++];
    for (; j < po_b->n; ++j, ++k)
        c[k] = b[j++];

    *po_c = (aprop_obj_t){
        .id = po_a->id,
        .n = k,
        .size = c_size,
        .props = c
    };

    // We have avoided excess ref / unref by simple copy so just free the objs
    free(po_a->props);
    free(po_b->props);

    memset(po_a, 0, sizeof(*po_a));
    memset(po_b, 0, sizeof(*po_b));

    return 0;
}

static aprop_prop_t *
aprop_obj_prop_get(aprop_obj_t * const po, const uint32_t id)
{
    unsigned int i;
    aprop_prop_t * pp = po->props;

    for (i = 0; i != po->n; ++i, ++pp) {
        if (pp->id == id)
            return pp;
    }

    if (po->n >= po->size) {
        size_t newsize = po->size < 16 ? 16 : po->size * 2;
        if ((pp = realloc(po->props, newsize * sizeof(*pp))) == NULL)
            return NULL;
        memset(pp + po->size, 0, (newsize - po->size) * sizeof(*pp));

        po->props = pp;
        po->size = newsize;
        pp += po->n;
    }
    ++po->n;

    pp->id = id;
    return pp;
}

static void
aprop_obj_atomic_fill(const aprop_obj_t * const po, uint32_t * prop_ids, uint64_t * prop_values)
{
    unsigned int i;
    for (i = 0; i != po->n; ++i) {
        *prop_ids++ = po->props[i].id;
        *prop_values++ = po->props[i].value;
    }
}

static void
aprop_obj_dump(drmu_env_t * const du, const aprop_obj_t * const po)
{
    unsigned int i;
    drmu_info(du, "Obj: %02x: size %d n %d", po->id, po->size, po->n);
    for (i = 0; i != po->n; ++i) {
        drmu_info(du, "Obj %02x: Prop %02x Value %"PRIx64" v %p", po->id, po->props[i].id, po->props[i].value, po->props[i].v);
    }
}

static void
aprop_hdr_dump(drmu_env_t * const du, const aprop_hdr_t * const ph)
{
    unsigned int i;
    drmu_info(du, "Header: size %d n %d", ph->size, ph->n);
    for (i = 0; i != ph->n; ++i)
        aprop_obj_dump(du, ph->objs + i);
}

static aprop_obj_t *
aprop_hdr_obj_get(aprop_hdr_t * const ph, const uint32_t id)
{
    unsigned int i;
    aprop_obj_t * po = ph->objs;

    for (i = 0; i != ph->n; ++i, ++po) {
        if (po->id == id)
            return po;
    }

    if (ph->n >= ph->size) {
        size_t newsize = ph->size < 16 ? 16 : ph->size * 2;
        if ((po = realloc(ph->objs, newsize * sizeof(*po))) == NULL)
            return NULL;
        memset(po + ph->size, 0, (newsize - ph->size) * sizeof(*po));

        ph->objs = po;
        ph->size = newsize;
        po += ph->n;
    }
    ++ph->n;

    po->id = id;
    return po;
}

static void
aprop_hdr_uninit(aprop_hdr_t * const ph)
{
    unsigned int i;
    for (i = 0; i != ph->n; ++i)
        aprop_obj_uninit(ph->objs + i);
    free(ph->objs);
    memset(ph, 0, sizeof(*ph));
}

static int
aprop_hdr_copy(aprop_hdr_t * const ph_c, const aprop_hdr_t * const ph_a)
{
    unsigned int i;

    aprop_hdr_uninit(ph_c);

    if (ph_a->n == 0)
        return 0;

    if ((ph_c->objs = calloc(ph_a->size, sizeof(*ph_c->objs))) == NULL)
        return -ENOMEM;

    ph_c->n = ph_a->n;
    ph_c->size = ph_a->size;

    for (i = 0; i != ph_a->n; ++i)
        aprop_obj_copy(ph_c->objs + i, ph_a->objs + i);
    return 0;
}

// Move b to a. a must be empty
static int
aprop_hdr_move(aprop_hdr_t * const ph_a, aprop_hdr_t * const ph_b)
{
    *ph_a = *ph_b;
    *ph_b = (aprop_hdr_t){0};
    return 0;
}

static int
aprop_obj_qsort_cb(const void * va, const void * vb)
{
    const aprop_obj_t * const a = va;
    const aprop_obj_t * const b = vb;
    return a->id == b->id ? 0 : a->id < b->id ? -1 : 1;
}

// Merge b into a. b will be uninited
static int
aprop_hdr_merge(aprop_hdr_t * const ph_a, aprop_hdr_t * const ph_b)
{
    unsigned int i, j, k;
    unsigned int c_size;
    aprop_obj_t * c;
    aprop_obj_t * const a = ph_a->objs;
    aprop_obj_t * const b = ph_b->objs;

    if (ph_b->n == 0)
        return 0;
    if (ph_a->n == 0)
        return aprop_hdr_move(ph_a, ph_b);

    // As we should have no identical els we don't care that qsort is unstable
    qsort(a, ph_a->n, sizeof(a[0]), aprop_obj_qsort_cb);
    qsort(b, ph_b->n, sizeof(b[0]), aprop_obj_qsort_cb);

    // Pick a size
    c_size = max_uint(ph_a->size, ph_b->size);
    if (c_size < ph_a->n + ph_b->n)
        c_size *= 2;
    if ((c = calloc(c_size, sizeof(*c))) == NULL)
        return -ENOMEM;

    for (i = 0, j = 0, k = 0; i < ph_a->n && j < ph_a->n; ++k) {
        if (a[i].id < b[j].id)
            aprop_obj_move(c + k, a + i++);
        else if (a[i].id > b[j].id)
            aprop_obj_move(c + k, b + j++);
        else
            aprop_obj_merge(c + k, a + i++, b + j++);
    }
    for (; i < ph_a->n; ++i, ++k)
        aprop_obj_move(c + k, a + i++);
    for (; j < ph_b->n; ++j, ++k)
        aprop_obj_move(c + k, b + j++);

    aprop_hdr_uninit(ph_a);
    aprop_hdr_uninit(ph_b);

    ph_a->n = k;
    ph_a->size = c_size;
    ph_a->objs = c;

    return 0;
}

static aprop_prop_t *
aprop_hdr_prop_get(aprop_hdr_t * const ph, const uint32_t obj_id, const uint32_t prop_id)
{
    aprop_obj_t * const po = aprop_hdr_obj_get(ph, obj_id);
    return po == NULL ? NULL : aprop_obj_prop_get(po, prop_id);
}

// Total props
static unsigned int
aprop_hdr_props_count(const aprop_hdr_t * const ph)
{
    unsigned int i;
    unsigned int n = 0;

    for (i = 0; i != ph->n; ++i)
        n += ph->objs[i].n;
    return n;
}

static unsigned int
aprop_hdr_objs_count(const aprop_hdr_t * const ph)
{
    return ph->n;
}

static void
aprop_hdr_atomic_fill(const aprop_hdr_t * const ph,
                     uint32_t * obj_ids,
                     uint32_t * prop_counts,
                     uint32_t * prop_ids,
                     uint64_t * prop_values)
{
    unsigned int i;
    for (i = 0; i != ph->n; ++i) {
        const unsigned int n = ph->objs[i].n;
        *obj_ids++ = ph->objs[i].id;
        *prop_counts++ = n;
        aprop_obj_atomic_fill(ph->objs +i, prop_ids, prop_values);
        prop_ids += n;
        prop_values += n;
    }
}

static int
aprop_hdr_prop_set(aprop_hdr_t * const ph,
                  const uint32_t obj_id, const uint32_t prop_id, const uint64_t value,
                  const drmu_prop_ref_fn ref_fn, const drmu_prop_del_fn del_fn, void * const v)
{
    if (obj_id == 0 || prop_id == 0)
    {
        return -EINVAL;
    }
    else
    {
        aprop_prop_t *const pp = aprop_hdr_prop_get(ph, obj_id, prop_id);
        if (pp == NULL)
            return -ENOMEM;

        aprop_prop_unref(pp);
        pp->value = value;
        pp->ref_fn = ref_fn;
        pp->del_fn = del_fn;
        pp->v = v;
        aprop_prop_ref(pp);
        return 0;
    }
}

static void
prop_enum_free(drmu_prop_enum_t * const pen)
{
    free((void*)pen->enums);  // Cast away const
    free(pen);
}

static int
prop_enum_qsort_cb(const void * va, const void * vb)
{
    const struct drm_mode_property_enum * a = va;
    const struct drm_mode_property_enum * b = vb;
    return strcmp(a->name, b->name);
}

// NULL if not found
static const uint64_t *
drmu_prop_enum_value(const drmu_prop_enum_t * const pen, const char * const name)
{
    if (pen != NULL && name != NULL) {
        unsigned int i = pen->n / 2;
        unsigned int a = 0;
        unsigned int b = pen->n;

        if (name == NULL)
            return NULL;

        while (a < b) {
            const int r = strcmp(name, pen->enums[i].name);

            if (r == 0)
                return &pen->enums[i].value;

            if (r < 0) {
                b = i;
                i = (i + a) / 2;
            } else {
                a = i + 1;
                i = (i + b) / 2;
            }
        }
    }
    return NULL;
}

static uint32_t
drmu_prop_enum_id(const drmu_prop_enum_t * const pen)
{
    return pen == NULL ? 0 : pen->id;
}

static void
drmu_prop_enum_delete(drmu_prop_enum_t ** const pppen)
{
    drmu_prop_enum_t * const pen = *pppen;
    if (pen == NULL)
        return;
    *pppen = NULL;

    prop_enum_free(pen);
}

static drmu_prop_enum_t *
drmu_prop_enum_new(drmu_env_t * const du, const uint32_t id)
{
    drmu_prop_enum_t * pen;
    struct drm_mode_property_enum * enums = NULL;
    unsigned int retries;

    // If id 0 return without warning for ease of getting props on init
    if (id == 0 || (pen = calloc(1, sizeof(*pen))) == NULL)
        return NULL;
    pen->id = id;

    // Docn says we must loop till stable as there may be hotplug races
    for (retries = 0; retries < 8; ++retries) {
        struct drm_mode_get_property prop = {
            .prop_id = id,
            .count_enum_blobs = pen->n,
            .enum_blob_ptr = (uintptr_t)enums
        };

        if (drmIoctl(du->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) != 0) {
            drmu_err(du, "%s: get property failed: %s", __func__, strerror(errno));
            goto fail;
        }

        if (prop.count_enum_blobs == 0 ||
            (prop.flags & (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BITMASK)) == 0) {
            drmu_err(du, "%s: not an enum: flags=%#x, enums=%d", __func__, prop.flags, prop.count_enum_blobs);
            goto fail;
        }

        if (pen->n >= prop.count_enum_blobs) {
            pen->flags = prop.flags;
            pen->n = prop.count_enum_blobs;
            memcpy(pen->name, prop.name, sizeof(pen->name));
            break;
        }

        free(enums);

        pen->n = prop.count_enum_blobs;
        if ((enums = malloc(pen->n * sizeof(*enums))) == NULL)
            goto fail;
    }
    if (retries >= 8) {
        drmu_err(du, "%s: Too many retries", __func__);
        goto fail;
    }

    qsort(enums, pen->n, sizeof(*enums), prop_enum_qsort_cb);
    pen->enums = enums;

#if TRACE_PROP_NEW
    {
        unsigned int i;
        for (i = 0; i != pen->n; ++i) {
            drmu_info(du, "%32s %2d:%02d: %32s %#"PRIx64, pen->name, pen->id, i, pen->enums[i].name, pen->enums[i].value);
        }
    }
#endif

    return pen;

fail:
    free(enums);
    prop_enum_free(pen);
    return NULL;
}

static void
prop_range_free(drmu_prop_range_t * const pra)
{
    free(pra);
}

static void
drmu_prop_range_delete(drmu_prop_range_t ** pppra)
{
    drmu_prop_range_t * const pra = *pppra;

    if (pra == NULL)
        return;
    *pppra = NULL;

    prop_range_free(pra);
}

static bool
drmu_prop_range_validate(const drmu_prop_range_t * const pra, const uint64_t x)
{
    if (pra == NULL)
        return false;
    if ((pra->flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_TYPE(DRM_MODE_PROP_SIGNED_RANGE)) {
        return (int64_t)pra->range[0] <= (int64_t)x && (int64_t)pra->range[1] >= (int64_t)x;
    }
    return pra->range[0] <= x && pra->range[1] >= x;
}

static uint32_t
drmu_prop_range_id(const drmu_prop_range_t * const pra)
{
    return pra == NULL ? 0 : pra->id;
}

static drmu_prop_range_t *
drmu_prop_range_new(drmu_env_t * const du, const uint32_t id)
{
    drmu_prop_range_t * pra;

    // If id 0 return without warning for ease of getting props on init
    if (id == 0 || (pra = calloc(1, sizeof(*pra))) == NULL)
        return NULL;
    pra->id = id;

    // We are expecting exactly 2 values so no need to loop
    {
        struct drm_mode_get_property prop = {
            .prop_id = id,
            .count_values = 2,
            .values_ptr = (uintptr_t)pra->range
        };

        if (drmIoctl(du->fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) != 0) {
            drmu_err(du, "%s: get property failed: %s", __func__, strerror(errno));
            goto fail;
        }

        if ((prop.flags & DRM_MODE_PROP_RANGE) == 0 &&
            (prop.flags & DRM_MODE_PROP_EXTENDED_TYPE) != DRM_MODE_PROP_TYPE(DRM_MODE_PROP_SIGNED_RANGE)) {
            drmu_err(du, "%s: not an enum: flags=%#x", __func__, prop.flags);
            goto fail;
        }
        if ((prop.count_values != 2)) {
            drmu_err(du, "%s: unexpected count values: %d", __func__, prop.count_values);
            goto fail;
        }

        pra->flags = prop.flags;
        memcpy(pra->name, prop.name, sizeof(pra->name));
    }

#if TRACE_PROP_NEW
    drmu_info(du, "%32s %2d: %"PRId64"->%"PRId64, pra->name, pra->id, pra->range[0], pra->range[1]);
#endif

    return pra;

fail:
    prop_range_free(pra);
    return NULL;
}



static int
bo_close(drmu_env_t * const du, uint32_t * const ph)
{
    struct drm_gem_close gem_close = {.handle = *ph};

    if (gem_close.handle == 0)
        return 0;
    *ph = 0;

    return drmIoctl(du->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
}

// BOE lock expected
static void
drmu_bo_free(drmu_bo_t * bo)
{
    drmu_env_t * const du = bo->du;

    if (bo->handle != 0) {
        switch (bo->bo_type) {
            case BO_TYPE_FD:
            {
                drmu_bo_env_t * const boe = &du->boe;
                const uint32_t h = bo->handle;
                if (bo_close(du, &bo->handle) != 0)
                    drmu_warn(du, "%s: Failed to close BO handle %d", __func__, h);
                if (bo->next != NULL)
                    bo->next->prev = bo->prev;
                if (bo->prev != NULL)
                    bo->prev->next = bo->next;
                else
                    boe->fd_head = bo->next;
                break;
            }
            case BO_TYPE_DUMB:
            {
                struct drm_mode_destroy_dumb destroy_env = {.handle = bo->handle};
                if (drmIoctl(du->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_env) != 0)
                    drmu_warn(du, "%s: Failed to destroy dumb handle %d", __func__, bo->handle);
                break;
            }
            case BO_TYPE_NONE:
            default:
                break;
        }
    }
    free(bo);
}

static void
drmu_bo_unref(drmu_bo_t ** ppbo)
{
    drmu_bo_t * const bo = *ppbo;
    drmu_bo_env_t * boe;

    if (bo == NULL)
        return;
    *ppbo = NULL;

    boe = &bo->du->boe;
    pthread_mutex_lock(&boe->lock);
    if (bo->ref_count-- == 0)
        drmu_bo_free(bo);
    pthread_mutex_unlock(&boe->lock);
}

static drmu_bo_t *
drmu_bo_alloc(drmu_env_t *const du, enum drmu_bo_type_e bo_type)
{
    drmu_bo_t * const bo = calloc(1, sizeof(*bo));
    if (bo == NULL) {
        drmu_err(du, "Failed to alloc BO");
        return NULL;
    }

    bo->du = du;
    bo->bo_type = bo_type;
    return bo;
}

static drmu_bo_t *
drmu_bo_new_fd(drmu_env_t *const du, const int fd)
{
    drmu_bo_env_t * const boe = &du->boe;
    drmu_bo_t * bo = NULL;
    uint32_t h = 0;

    pthread_mutex_lock(&boe->lock);

    if (drmPrimeFDToHandle(du->fd, fd, &h) != 0) {
        drmu_err(du, "%s: Failed to convert fd %d to BO: %s", __func__, fd, strerror(errno));
        goto unlock;
    }

    bo = boe->fd_head;
    while (bo != NULL && bo->handle != h)
        bo = bo->next;

    if (bo != NULL) {
        ++bo->ref_count;
    }
    else {
        if ((bo = drmu_bo_alloc(du, BO_TYPE_FD)) == NULL) {
            bo_close(du, &h);
        }
        else {
            bo->handle = h;

            if ((bo->next = boe->fd_head) != NULL)
                bo->next->prev = bo;
            boe->fd_head = bo;
        }
    }

unlock:
    pthread_mutex_unlock(&boe->lock);
    return bo;
}

// Updates the passed dumb structure with the results of creation
static drmu_bo_t *
drmu_bo_new_dumb(drmu_env_t *const du, struct drm_mode_create_dumb * const d)
{
    drmu_bo_t *bo = drmu_bo_alloc(du, BO_TYPE_DUMB);

    if (bo == NULL)
        return NULL;

    if (drmIoctl(du->fd, DRM_IOCTL_MODE_CREATE_DUMB, d) != 0)
    {
        drmu_err(du, "%s: Create dumb %dx%dx%d failed: %s", __func__,
                 d->width, d->height, d->bpp, strerror(errno));
        drmu_bo_unref(&bo);  // After this point aux is bound to dfb and gets freed with it
        return NULL;
    }

    bo->handle = d->handle;
    return bo;
}

static void
drmu_bo_env_uninit(drmu_bo_env_t * const boe)
{
    if (boe->fd_head != NULL)
        drmu_warn(boe->fd_head->du, "%s: fd chain not null", __func__);
    boe->fd_head = NULL;
    pthread_mutex_destroy(&boe->lock);
}

static void
drmu_bo_env_init(drmu_bo_env_t * boe)
{
    boe->fd_head = NULL;
    pthread_mutex_init(&boe->lock, NULL);
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

#if TRACE_PROP_NEW || 1
static void
props_dump(const drmu_props_t * const props)
{
    if (props != NULL) {
        unsigned int i;
        drmu_env_t * const du = props->du;

        for (i = 0; i != props->prop_count; ++i) {
            drmModePropertyPtr p = props->props[i];
            drmu_info(du, "Prop%02d/%02d: id=%#02x, name=%s, flags=%#x, values=%d, value[0]=%#"PRIx64", blobs=%d, blob[0]=%#x", i, props->prop_count, p->prop_id,
                      p->name, p->flags,
                      p->count_values, !p->values ? (uint64_t)0 : p->values[0],
                      p->count_blobs, !p->blob_ids ? 0 : p->blob_ids[0]);
        }
    }
}
#endif

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

static void
drmu_atomic_dump(const drmu_atomic_t * const da)
{
    drmu_info(da->du, "Atomic %p: refs %d", da, atomic_load(&da->ref_count)+1);
    aprop_hdr_dump(da->du, &da->props);
}

static void
drmu_atomic_free(drmu_atomic_t * const da)
{
    aprop_hdr_uninit(&da->props);
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

static drmu_atomic_t *
drmu_atomic_new(drmu_env_t * const du)
{
    drmu_atomic_t * const da = calloc(1, sizeof(*da));

    if (da == NULL) {
        drmu_err(du, "%s: Failed to alloc struct", __func__);
        return NULL;
    }
    da->du = du;

    return da;
}

// Merge b into a. b is unrefed (inc on error), *ppa is replaced by the merged
// contents
static int
drmu_atomic_merge(drmu_atomic_t * const a, drmu_atomic_t ** const ppb)
{
    drmu_atomic_t * b = *ppb;
    aprop_hdr_t bh = {0};
    int rv = -EINVAL;

    if (b == NULL)
        return 0;
    *ppb = NULL;

    if (a == NULL) {
        drmu_atomic_unref(&b);
        return -EINVAL;
    }
    // We expect this to be the sole ref to a
    assert(atomic_load(&a->ref_count) == 0);

    // If this is the only copy of b then use it directly otherwise
    // copy before (probably) making it unusable
    if (atomic_load(&b->ref_count) == 0)
        rv = aprop_hdr_move(&bh, &b->props);
    else
        rv = aprop_hdr_copy(&bh, &b->props);
    drmu_atomic_unref(&b);

    if (rv != 0) {
        drmu_err(a->du, "%s: Copy Failed", __func__);
        return rv;
    }

    rv = aprop_hdr_merge(&a->props, &bh);
    aprop_hdr_uninit(&bh);

    if (rv != 0) {
        drmu_err(a->du, "%s: Merge Failed", __func__);
        return rv;
    }

    return 0;
}

static int
drmu_atomic_commit(drmu_atomic_t * const da, uint32_t flags)
{
    drmu_env_t * const du = da->du;
    const unsigned int n_objs = aprop_hdr_objs_count(&da->props);
    const unsigned int n_props = aprop_hdr_props_count(&da->props);
    int rv = 0;

    if (n_props != 0) {
        uint32_t obj_ids[n_objs];
        uint32_t prop_counts[n_objs];
        uint32_t prop_ids[n_props];
        uint64_t prop_values[n_props];
        struct drm_mode_atomic atomic = {
            .flags           = flags,
            .count_objs      = n_objs,
            .objs_ptr        = (uintptr_t)obj_ids,
            .count_props_ptr = (uintptr_t)prop_counts,
            .props_ptr       = (uintptr_t)prop_ids,
            .prop_values_ptr = (uintptr_t)prop_values,
            .user_data       = (uintptr_t)da
        };

        aprop_hdr_atomic_fill(&da->props, obj_ids, prop_counts, prop_ids, prop_values);

        if (drmIoctl(du->fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0)
            return 0;

        rv = -errno;
        if (rv == -EINVAL) {
            drmu_debug(du, "%s: EINVAL try ALLOW_MODESET", __func__);
            atomic.flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
            if (drmIoctl(du->fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0)
                return 0;
        }

//        drmu_err(du, "%s: Atomic failed: %s", __func__, strerror(-rv));
    }

    return rv;
}

static void atomic_q_retry(drmu_atomic_q_t * const aq, drmu_env_t * const du);

// Needs locked
static int
atomic_q_attempt_commit_next(drmu_atomic_q_t * const aq)
{
    drmu_env_t * const du = aq->next_flip->du;
    int rv;

    if ((rv = drmu_atomic_commit(aq->next_flip, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT)) == 0) {
        if (aq->retry_count != 0)
            drmu_warn(du, "%s: Atomic commit OK", __func__);
        aq->cur_flip = aq->next_flip;
        aq->next_flip = NULL;
        aq->retry_count = 0;
    }
    else if (rv == -EBUSY && ++aq->retry_count < 16) {
        // This really shouldn't happen but we observe that the 1st commit after
        // a modeset often fails with BUSY.  It seems to be fine on a 10ms retry
        // but allow some more in case ww need a bit longer in some cases
        drmu_warn(du, "%s: Atomic commit BUSY", __func__);
        atomic_q_retry(aq, du);
        rv = 0;
    }
    else {
        drmu_err(du, "%s: Atomic commit failed: %s", __func__, strerror(-rv));
        drmu_atomic_dump(aq->next_flip);
        drmu_atomic_unref(&aq->next_flip);
        aq->retry_count = 0;
    }

    return rv;
}

static void
atomic_q_retry_cb(void * v, short revents)
{
    drmu_atomic_q_t * const aq = v;
    (void)revents;

    pthread_mutex_lock(&aq->lock);

    // If we need a retry then next != NULL && cur == NULL
    // if not that then we've fixed ourselves elsewhere

    if (aq->next_flip != NULL && aq->cur_flip == NULL)
        atomic_q_attempt_commit_next(aq);

    pthread_mutex_unlock(&aq->lock);
}

static void
atomic_q_retry(drmu_atomic_q_t * const aq, drmu_env_t * const du)
{
    if (aq->retry_task == NULL)
        aq->retry_task = polltask_new_timer(du->pq, atomic_q_retry_cb, aq);
    pollqueue_add_task(aq->retry_task, 20);
}

// Called after an atomic commit has completed
// not called on every vsync, so if we haven't committed anything this won't be called
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

    // At this point:
    //  next   The atomic we are about to commit
    //  cur    The last atomic we committed, now in use (must be != NULL)
    //  last   The atomic that has just become obsolete

    pthread_mutex_lock(&aq->lock);

    if (da != aq->cur_flip) {
        drmu_err(du, "%s: User data el (%p) != cur (%p)", __func__, da, aq->cur_flip);
    }

    drmu_atomic_unref(&aq->last_flip);
    aq->last_flip = aq->cur_flip;
    aq->cur_flip = NULL;

    if (aq->next_flip != NULL)
        atomic_q_attempt_commit_next(aq);

    pthread_mutex_unlock(&aq->lock);
}

// 'consumes' da
static int
atomic_q_queue(drmu_atomic_q_t * const aq, drmu_atomic_t * da)
{
    int rv = 0;

    pthread_mutex_lock(&aq->lock);

    if (aq->next_flip != NULL) {
        // We already have something pending or retrying - merge the new with it
        rv = drmu_atomic_merge(aq->next_flip, &da);
    }
    else {
        aq->next_flip = da;

        // No pending commit?
        if (aq->cur_flip == NULL)
            rv = atomic_q_attempt_commit_next(aq);
    }

    pthread_mutex_unlock(&aq->lock);
    return rv;
}

// Consumes the passed atomic structure as it isn't copied
// * arguably should copy & unref if ref count != 0
static int
drmu_atomic_queue(drmu_atomic_t ** ppda)
{
    drmu_atomic_t * da = *ppda;

    if (da == NULL)
        return 0;
    *ppda = NULL;

    return atomic_q_queue(&da->du->aq, da);
}

static void
drmu_atomic_q_uninit(drmu_atomic_q_t * const aq)
{
    polltask_delete(&aq->retry_task);
    drmu_atomic_unref(&aq->next_flip);
    drmu_atomic_unref(&aq->cur_flip);
    drmu_atomic_unref(&aq->last_flip);
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
    if (aprop_hdr_prop_set(&da->props, obj_id, prop_id, value, 0, 0, NULL) < 0)
        drmu_warn(da->du, "%s: Failed to set obj_id=%#x, prop_id=%#x, val=%" PRId64, __func__,
                 obj_id, prop_id, value);
    return 0;
}

static void
atomic_prop_fb_unref(void * v)
{
    drmu_fb_t * fb = v;
    drmu_fb_unref(&fb);
}

static void
atomic_prop_fb_ref(void * v)
{
    drmu_fb_ref(v);
}

static int
drmu_atomic_add_prop_fb(drmu_atomic_t * const da, const uint32_t obj_id, const uint32_t prop_id, drmu_fb_t * const dfb)
{
    int rv;

    if (dfb == NULL)
        return drmu_atomic_add_prop(da, obj_id, prop_id, 0);

    rv = aprop_hdr_prop_set(&da->props, obj_id, prop_id, dfb->handle, atomic_prop_fb_ref, atomic_prop_fb_unref, dfb);
    if (rv != 0)
        drmu_warn(da->du, "%s: Failed to add fb obj_id=%#x, prop_id=%#x: %s", __func__, obj_id, prop_id, strerror(-rv));

    return rv;
}

static int
drmu_atomic_add_prop_enum(drmu_atomic_t * const da, const uint32_t obj_id, const drmu_prop_enum_t * const pen, const char * const name)
{
    const uint64_t * const pval = drmu_prop_enum_value(pen, name);
    int rv;

    rv = (pval == NULL) ? -EINVAL :
        aprop_hdr_prop_set(&da->props, obj_id, drmu_prop_enum_id(pen), *pval, 0, 0, NULL);

    if (rv != 0 && name != NULL)
        drmu_warn(da->du, "%s: Failed to add enum obj_id=%#x, prop_id=%#x, name='%s': %s", __func__,
                  obj_id, drmu_prop_enum_id(pen), name, strerror(-rv));

    return rv;
}

static int
drmu_atomic_add_prop_range(drmu_atomic_t * const da, const uint32_t obj_id, const drmu_prop_range_t * const pra, const uint64_t x)
{
    int rv;

    rv = !drmu_prop_range_validate(pra, x) ? -EINVAL :
        aprop_hdr_prop_set(&da->props, obj_id, drmu_prop_range_id(pra), x, 0, 0, NULL);

    if (rv != 0)
        drmu_warn(da->du, "%s: Failed to add range obj_id=%#x, prop_id=%#x, val=%"PRId64": %s", __func__,
                  obj_id, drmu_prop_range_id(pra), x, strerror(-rv));

    return rv;
}

static void
atomic_prop_blob_unref(void * v)
{
    drmu_blob_t * blob = v;
    drmu_blob_unref(&blob);
}

static void
atomic_prop_blob_ref(void * v)
{
    drmu_blob_ref(v);
}

static int
drmu_atomic_add_prop_blob(drmu_atomic_t * const da, const uint32_t obj_id, const uint32_t prop_id, drmu_blob_t * const blob)
{
    int rv;

    if (blob == NULL)
        return drmu_atomic_add_prop(da, obj_id, prop_id, 0);

    rv = aprop_hdr_prop_set(&da->props, obj_id, prop_id, drmu_blob_id(blob), atomic_prop_blob_ref, atomic_prop_blob_unref, blob);
    if (rv != 0)
        drmu_warn(da->du, "%s: Failed to add blob obj_id=%#x, prop_id=%#x: %s", __func__, obj_id, prop_id, strerror(-rv));

    return rv;
}

static void
free_fb(drmu_fb_t * const dfb)
{
    drmu_env_t * const du = dfb->du;
    unsigned int i;

    if (dfb->pre_delete_fn && dfb->pre_delete_fn(dfb, dfb->pre_delete_v) != 0)
        return;

    if (dfb->handle != 0)
        drmModeRmFB(du->fd, dfb->handle);

    if (dfb->map_ptr != NULL && dfb->map_ptr != MAP_FAILED)
        munmap(dfb->map_ptr, dfb->map_size);

    for (i = 0; i != 4; ++i)
        drmu_bo_unref(dfb->bo_list + i);

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

// Bits per pixel on plane 0
static unsigned int
drmu_fb_pixel_bits(const drmu_fb_t * const dfb)
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
        case DRM_FORMAT_AYUV:
            return 32;
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_YVYU:
        case DRM_FORMAT_VYUY:
        case DRM_FORMAT_UYVY:
            return 16;
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_YUV420:
            return 8;
        default:
            break;
    }
    return 0;
}

// For allocation purposes given fb_pixel bits how tall
// does the frame have to be to fit all planes
static unsigned int
fb_total_height(const drmu_fb_t * const dfb, unsigned int h)
{
    switch (dfb->format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_YUV420:
            return h * 3 / 2;
        default:
            break;
    }
    return h;
}

static void
fb_pitches_set(drmu_fb_t * const dfb)
{
    memset(dfb->offsets, 0, sizeof(dfb->offsets));
    memset(dfb->pitches, 0, sizeof(dfb->pitches));

    switch (dfb->format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_AYUV:
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_YVYU:
        case DRM_FORMAT_VYUY:
        case DRM_FORMAT_UYVY:
            dfb->pitches[0] = dfb->map_pitch;
            break;
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
            dfb->pitches[0] = dfb->map_pitch;
            dfb->pitches[1] = dfb->map_pitch;
            dfb->offsets[1] = dfb->pitches[0] * dfb->height;
            break;
        case DRM_FORMAT_YUV420:
            dfb->pitches[0] = dfb->map_pitch;
            dfb->pitches[1] = dfb->map_pitch / 2;
            dfb->pitches[2] = dfb->map_pitch / 2;
            dfb->offsets[1] = dfb->pitches[0] * dfb->height;
            dfb->offsets[2] = dfb->offsets[1] + dfb->pitches[1] * dfb->height / 2;
            break;
        default:
            break;
    }
}

static drmu_fb_t *
drmu_fb_new_dumb(drmu_env_t * const du, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_fb_t * const dfb = alloc_fb(du);
    uint32_t bpp;

    if (dfb == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }
    dfb->width = (w + 63) & ~63;
    dfb->height = (h + 63) & ~63;
    dfb->cropped = drmu_rect_wh(w, h);
    dfb->format = format;

    if ((bpp = drmu_fb_pixel_bits(dfb)) == 0) {
        drmu_err(du, "%s: Unexpected format %#x", __func__, format);
        goto fail;
    }

    {
        struct drm_mode_create_dumb dumb = {
            .height = fb_total_height(dfb, dfb->height),
            .width = dfb->width,
            .bpp = bpp
        };
        if ((dfb->bo_list[0] = drmu_bo_new_dumb(du, &dumb)) == NULL)
            goto fail;

        dfb->map_pitch = dumb.pitch;
        dfb->map_size = (size_t)dumb.size;
    }

    {
        struct drm_mode_map_dumb map_dumb = {
            .handle = dfb->bo_list[0]->handle
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
        uint32_t bo_handles[4] = { dfb->bo_list[0]->handle };

        fb_pitches_set(dfb);

        if (dfb->pitches[1] != 0)
            bo_handles[1] = bo_handles[0];
        if (dfb->pitches[2] != 0)
            bo_handles[2] = bo_handles[0];

        if (drmModeAddFB2WithModifiers(du->fd,
                                       dfb->width, dfb->height, dfb->format,
                                       bo_handles, dfb->pitches, dfb->offsets, NULL,
                                       &dfb->handle, 0) != 0)
        {
            drmu_err(du, "%s: drmModeAddFB2WithModifiers failed: %s\n", __func__, ERRSTR);
            goto fail;
        }
    }

    drmu_debug(du, "Create dumb %p %dx%d / %dx%d size: %zd", dfb, dfb->width, dfb->height, dfb->cropped.w, dfb->cropped.h, dfb->map_size);

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

#if 0

std::string GetColorimetry(const VideoPicture& picture)
{
  switch (picture.color_space)
  {
    case AVCOL_SPC_BT2020_CL:
    case AVCOL_SPC_BT2020_NCL:
      return "BT2020_RGB";
  }

  return "Default";
}

std::string GetColorEncoding(const VideoPicture& picture)
{
  switch (picture.color_space)
  {
    case AVCOL_SPC_BT2020_CL:
    case AVCOL_SPC_BT2020_NCL:
      return "ITU-R BT.2020 YCbCr";
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_FCC:
      return "ITU-R BT.601 YCbCr";
    case AVCOL_SPC_BT709:
      return "ITU-R BT.709 YCbCr";
    case AVCOL_SPC_RESERVED:
    case AVCOL_SPC_UNSPECIFIED:
    default:
      if (picture.iWidth > 1024 || picture.iHeight >= 600)
        return "ITU-R BT.709 YCbCr";
      else
        return "ITU-R BT.601 YCbCr";
  }
}

std::string GetColorRange(const VideoPicture& picture)
{
  if (picture.color_range)
    return "YCbCr full range";
  return "YCbCr limited range";
}


void CVideoLayerBridgeDRMPRIME::Configure(CVideoBufferDRMPRIME* buffer)
{
  const VideoPicture& picture = buffer->GetPicture();

  auto plane = m_DRM->GetVideoPlane();

  bool result;
  uint64_t value;
  std::tie(result, value) = plane->GetPropertyValue("COLOR_ENCODING", GetColorEncoding(picture));
  if (result)
    m_DRM->AddProperty(plane, "COLOR_ENCODING", value);

  std::tie(result, value) = plane->GetPropertyValue("COLOR_RANGE", GetColorRange(picture));
  if (result)
    m_DRM->AddProperty(plane, "COLOR_RANGE", value);

  auto connector = m_DRM->GetConnector();

  std::tie(result, value) =  connector->GetPropertyValue("Colorspace", GetColorimetry(picture));
  if (result)
  {
    CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - setting connector colorspace to {}", __FUNCTION__,
              GetColorimetry(picture));
    m_DRM->AddProperty(connector, "Colorspace", value);
    m_DRM->SetActive(true);
  }

  if (connector->SupportsProperty("HDR_OUTPUT_METADATA"))
  {
    m_hdr_metadata.metadata_type = HDMI_STATIC_METADATA_TYPE1;
    m_hdr_metadata.hdmi_metadata_type1 = {
        .eotf = GetEOTF(picture),
        .metadata_type = HDMI_STATIC_METADATA_TYPE1,
    };

    if (m_hdr_blob_id)
      drmModeDestroyPropertyBlob(m_DRM->GetFileDescriptor(), m_hdr_blob_id);
    m_hdr_blob_id = 0;

    if (m_hdr_metadata.hdmi_metadata_type1.eotf)
    {
      const AVMasteringDisplayMetadata* mdmd = GetMasteringDisplayMetadata(picture);
      if (mdmd && mdmd->has_primaries)
      {
        // Convert to unsigned 16-bit values in units of 0.00002,
        // where 0x0000 represents zero and 0xC350 represents 1.0000
        for (int i = 0; i < 3; i++)
        {
          m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].x =
              std::round(av_q2d(mdmd->display_primaries[i][0]) * 50000.0);
          m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].y =
              std::round(av_q2d(mdmd->display_primaries[i][1]) * 50000.0);
        }
        m_hdr_metadata.hdmi_metadata_type1.white_point.x =
            std::round(av_q2d(mdmd->white_point[0]) * 50000.0);
        m_hdr_metadata.hdmi_metadata_type1.white_point.y =
            std::round(av_q2d(mdmd->white_point[1]) * 50000.0);
      }
      if (mdmd && mdmd->has_luminance)
      {
        // Convert to unsigned 16-bit value in units of 1 cd/m2,
        // where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2
        m_hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance =
            std::round(av_q2d(mdmd->max_luminance));

        // Convert to unsigned 16-bit value in units of 0.0001 cd/m2,
        // where 0x0001 represents 0.0001 cd/m2 and 0xFFFF represents 6.5535 cd/m2
        m_hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance =
            std::round(av_q2d(mdmd->min_luminance) * 10000.0);
      }

      const AVContentLightMetadata* clmd = GetContentLightMetadata(picture);
      if (clmd)
      {
        m_hdr_metadata.hdmi_metadata_type1.max_cll = clmd->MaxCLL;
        m_hdr_metadata.hdmi_metadata_type1.max_fall = clmd->MaxFALL;
      }

      drmModeCreatePropertyBlob(m_DRM->GetFileDescriptor(), &m_hdr_metadata, sizeof(m_hdr_metadata),
                                &m_hdr_blob_id);
    }

    m_DRM->AddProperty(connector, "HDR_OUTPUT_METADATA", m_hdr_blob_id);
    m_DRM->SetActive(true);
  }
}
#endif

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
static drmu_fb_t *
drmu_fb_vlc_new_pic_attach(drmu_env_t * const du, picture_t * const pic)
{
    uint64_t modifiers[4] = { 0 };
    uint32_t bo_handles[4] = { 0 };
    int i, j, n;
    drmu_fb_t * const dfb = alloc_fb(du);
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
        drmu_err(du, "drmModeAddFB2WithModifiers failed: %s", ERRSTR);
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

    pthread_mutex_lock(&pool->lock);
    fb_list_add_tail(&pool->free_fbs, dfb);
    pthread_mutex_unlock(&pool->lock);

    // May cause suicide & recursion on fb delete, but that should be OK as
    // the 1 we return here should cause simple exit of fb delete
    drmu_pool_unref(&pool);
    return 1;  // Stop delete
}

static drmu_fb_t *
drmu_pool_fb_new_dumb(drmu_pool_t * const pool, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_env_t * const du = pool->du;
    drmu_fb_t * dfb;

    pthread_mutex_lock(&pool->lock);

    dfb = fb_list_peek_head(&pool->free_fbs);
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
        }
        ++pool->fb_count;
        pthread_mutex_unlock(&pool->lock);

        drmu_fb_unref(&dfb);  // Will free the dfb as pre-delete CB will be unset
        if ((dfb = drmu_fb_realloc_dumb(du, NULL, w, h, format)) == NULL) {
            --pool->fb_count;  // ??? lock
            return NULL;
        }
    }
    else {
        pthread_mutex_unlock(&pool->lock);
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

    drmu_prop_range_delete(&dc->pid.max_bpc);
    drmu_prop_enum_delete(&dc->pid.colorspace);
    drmu_prop_enum_delete(&dc->pid.output_format);
    drmu_blob_unref(&dc->hdr_metadata_blob);
    drmu_blob_unref(&dc->mode_id_blob);
    free(dc);
}

static int
drmu_atomic_crtc_hdr_metadata_set(drmu_atomic_t * const da, drmu_crtc_t * const dc, const struct hdr_output_metadata * const m)
{
    drmu_env_t * const du = da->du;
    int rv;

    if (dc->pid.hdr_output_metadata == 0)
        return 0;

    if (m == NULL) {
        if (dc->hdr_metadata_blob != NULL) {
            drmu_info(du, "Unset hdr metadata");
            drmu_blob_unref(&dc->hdr_metadata_blob);
        }
    }
    else {
        const size_t blob_len = sizeof(*m);
        drmu_blob_t * blob = NULL;

        if (dc->hdr_metadata_blob == NULL || memcmp(&dc->hdr_metadata, m, blob_len) != 0)
        {
            drmu_info(du, "Set hdr metadata");

            if ((blob = drmu_blob_new(du, m, blob_len)) == NULL)
                return -ENOMEM;

            // memcpy rather than structure copy to ensure keeping all padding 0s
            memcpy(&dc->hdr_metadata, m, blob_len);

            drmu_blob_unref(&dc->hdr_metadata_blob);
            dc->hdr_metadata_blob = blob;
        }
    }

    rv = drmu_atomic_add_prop_blob(da, dc->con->connector_id, dc->pid.hdr_output_metadata, dc->hdr_metadata_blob);
    if (rv != 0)
        drmu_err(du, "Set property fail: %s", strerror(errno));

    return rv;
}

// Set misc derived vars from mode
static void
crtc_mode_set_vars(drmu_crtc_t * const dc)
{
    switch (dc->crtc->mode.flags & DRM_MODE_FLAG_PIC_AR_MASK) {
        case DRM_MODE_FLAG_PIC_AR_4_3:
            dc->par = (drmu_ufrac_t){4,3};
            break;
        case DRM_MODE_FLAG_PIC_AR_16_9:
            dc->par = (drmu_ufrac_t){16,9};
            break;
        case DRM_MODE_FLAG_PIC_AR_64_27:
            dc->par = (drmu_ufrac_t){64,27};
            break;
        case DRM_MODE_FLAG_PIC_AR_256_135:
            dc->par = (drmu_ufrac_t){256,135};
            break;
        default:
        case DRM_MODE_FLAG_PIC_AR_NONE:
            dc->par = (drmu_ufrac_t){0,0};
            break;
    }

    if (dc->par.den == 0) {
        // Assume 1:1
        dc->sar = (drmu_ufrac_t){1,1};
    }
    else {
        dc->sar = drmu_ufrac_reduce((drmu_ufrac_t){dc->par.num * dc->crtc->height, dc->par.den * dc->crtc->width});
    }
}

// This sets width/height etc on the CRTC
// Really it should be held with the atomic but so far I haven't worked out
// a plausible API
static int
drmu_atomic_crtc_mode_id_set(drmu_atomic_t * const da, drmu_crtc_t * const dc, const int mode_id)
{
    drmu_env_t * const du = dc->du;
    drmu_blob_t * blob = NULL;
    const drmModeModeInfo * mode;

    if (mode_id < 0 || dc->pid.mode_id == 0)
        return 0;

    if (dc->cur_mode_id == mode_id && dc->mode_id_blob != NULL)
        return 0;

    drmu_info(du, "Set mode_id");

    mode = dc->con->modes + mode_id;
    if ((blob = drmu_blob_new(du, mode, sizeof(*mode))) == NULL) {
        return -ENOMEM;
    }

    drmu_blob_unref(&dc->mode_id_blob);
    dc->cur_mode_id = mode_id;
    dc->mode_id_blob = blob;

    dc->crtc->mode = *mode;
    dc->crtc->width = mode->hdisplay;
    dc->crtc->height = mode->vdisplay;
    crtc_mode_set_vars(dc);

    return drmu_atomic_add_prop_blob(da, dc->enc->crtc_id, dc->pid.mode_id, dc->mode_id_blob);
}

static int
atomic_crtc_bpc_set(drmu_atomic_t * const da, drmu_crtc_t * const dc,
                    const char * const colorspace, const char * const output_format,
                    const unsigned int max_bpc)
{
    const uint32_t con_id = dc->con->connector_id;
    int rv = 0;

    if ((dc->pid.colorspace &&
         (rv = drmu_atomic_add_prop_enum(da, con_id, dc->pid.colorspace, colorspace)) != 0) ||
        (dc->pid.output_format &&
         (rv = drmu_atomic_add_prop_enum(da, con_id, dc->pid.output_format, output_format)) != 0) ||
        (dc->pid.max_bpc &&
         (rv = drmu_atomic_add_prop_range(da, con_id, dc->pid.max_bpc, max_bpc)) != 0))
        return rv;
    return 0;
}

static int
atomic_crtc_hi_bpc_set(drmu_atomic_t * const da, drmu_crtc_t * const dc)
{
    return atomic_crtc_bpc_set(da, dc, "BT2020_YCC", "YCrCb422", 12);
}

static int
drmu_atomic_crtc_colorspace_set(drmu_atomic_t * const da, drmu_crtc_t * const dc, const char * colorspace, int hi_bpc)
{
    if (!hi_bpc || !dc->hi_bpc_ok || !colorspace || strcmp(colorspace, "BT2020_RGB") != 0) {
        return atomic_crtc_bpc_set(da, dc, colorspace, "RGB444", 8);
    }
    else {
        return atomic_crtc_hi_bpc_set(da, dc);
    }
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
static inline drmu_ufrac_t
drmu_crtc_sar(const drmu_crtc_t * const dc)
{
    return dc->sar;;
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
    drmu_atomic_add_prop_fb(da, plid, dp->pid.fb_id, dfb);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_x, crtc_x);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_y, crtc_y);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_w, crtc_w);
    drmu_atomic_add_prop(da, plid, dp->pid.crtc_h, crtc_h);
    drmu_atomic_add_prop(da, plid, dp->pid.src_x,  src_x);
    drmu_atomic_add_prop(da, plid, dp->pid.src_y,  src_y);
    drmu_atomic_add_prop(da, plid, dp->pid.src_w,  src_w);
    drmu_atomic_add_prop(da, plid, dp->pid.src_h,  src_h);
    return 0;
}

static int
drmu_atomic_plane_set(drmu_atomic_t * const da, drmu_plane_t * const dp,
    drmu_fb_t * const dfb, const drmu_rect_t pos)
{
    int rv;
    const uint32_t plid = dp->plane->plane_id;

    if (dfb == NULL) {
        rv = plane_set_atomic(da, dp, NULL,
                              0, 0, 0, 0,
                              0, 0, 0, 0);
    }
    else {
        rv = plane_set_atomic(da, dp, dfb,
                              pos.x, pos.y, pos.w, pos.h,
                              dfb->cropped.x << 16, dfb->cropped.y << 16, dfb->cropped.w << 16, dfb->cropped.h << 16);
    }
    if (rv != 0 || dfb == NULL)
        return rv;

    drmu_atomic_add_prop_enum(da, plid, dp->pid.color_encoding, dfb->color_encoding);
    drmu_atomic_add_prop_enum(da, plid, dp->pid.color_range,    dfb->color_range);

    // *** Need to rethink this
    if (dp->dc != NULL) {
        drmu_crtc_t * const dc = dp->dc;

        if (dfb->colorspace != NULL) {
            drmu_atomic_crtc_colorspace_set(da, dc, dfb->colorspace, 1);
        }
        if (dfb->hdr_metadata_isset == DRMU_ISSET_NULL)
            drmu_atomic_crtc_hdr_metadata_set(da, dc, NULL);
        else if (dfb->hdr_metadata_isset == DRMU_ISSET_SET)
            drmu_atomic_crtc_hdr_metadata_set(da, dc, &dfb->hdr_metadata);
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

    drmu_prop_enum_delete(&dp->pid.color_encoding);
    drmu_prop_enum_delete(&dp->pid.color_range);
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

    {
        drmu_info(du, "Crtc:");
        drmu_props_t * props = props_new(du, dc->crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
        props_dump(props);

        dc->pid.mode_id = props_name_to_id(props, "MODE_ID");

        props_free(props);
    }

    {
        drmu_props_t * const props = props_new(du, dc->con->connector_id, DRM_MODE_OBJECT_CONNECTOR);

        if (props != NULL) {
#if TRACE_PROP_NEW || 1
            drmu_info(du, "Connector:");
            props_dump(props);
#endif
            dc->pid.max_bpc             = drmu_prop_range_new(du, props_name_to_id(props, "max bpc"));
            dc->pid.colorspace          = drmu_prop_enum_new(du, props_name_to_id(props, "Colorspace"));
            dc->pid.output_format       = drmu_prop_enum_new(du, props_name_to_id(props, "output format"));
            dc->pid.hdr_output_metadata = props_name_to_id(props, "HDR_OUTPUT_METADATA");
            props_free(props);
        }
    }

    if (dc->pid.output_format && dc->pid.colorspace && dc->pid.max_bpc) {
        drmu_atomic_t * da = drmu_atomic_new(du);
        if (da != NULL &&
            atomic_crtc_hi_bpc_set(da, dc) == 0 &&
            drmu_atomic_commit(da, DRM_MODE_ATOMIC_TEST_ONLY) == 0) {
            dc->hi_bpc_ok = true;
        }
        drmu_atomic_unref(&da);
    }
    drmu_info(du, "Hi BPC %s", dc->hi_bpc_ok ? "OK" : "no");

    crtc_mode_set_vars(dc);

    drmu_info(du, "Flags: %#x, par=%d/%d sar=%d/%d", dc->crtc->mode.flags, dc->par.num, dc->par.den, dc->sar.num, dc->sar.den);

    {
        drmModePropertyPtr p = drmModeGetProperty(du->fd, dc->pid.mode_id);
        drmu_info(du, "Mode_id: flags %#x, values=%d, blobs=%d, blob[0]=%#x", p->flags, p->count_values, p->count_blobs, p->blob_ids == NULL ? 0 : p->blob_ids[0]);
        drmModeFreeProperty(p);
    }

    return dc;

fail:
    free_crtc(dc);
    return NULL;
}

static int
drmu_crtc_mode_pick(drmu_crtc_t * const dc, drmu_mode_score_fn score_fn, void * const score_v)
{
    int best_score = -1;
    int best_mode = -1;
    int i;

    for (i = 0; i < dc->con->count_modes; ++i) {
        int score = score_fn(score_v, dc->con->modes + i);
        if (score > best_score) {
            best_score = score;
            best_mode = i;
        }
    }

    return best_mode;
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

        dp->pid.color_encoding = drmu_prop_enum_new(du, props_name_to_id(props, "COLOR_ENCODING"));
        dp->pid.color_range    = drmu_prop_enum_new(du, props_name_to_id(props, "COLOR_RANGE"));

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

    pollqueue_unref(&du->pq);
    polltask_delete(&du->pt);

    if (du->res != NULL)
        drmModeFreeResources(du->res);
    free_planes(du);
    drmu_atomic_q_uninit(&du->aq);
    drmu_bo_env_uninit(&du->boe);

    close(du->fd);
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
        drmHandleEvent(du->fd, &ctx);
    }

    pollqueue_add_task(du->pt, 1000);
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
    drmu_bo_env_init(&du->boe);
    drmu_atomic_q_init(&du->aq);

    if ((du->pq = pollqueue_new()) == NULL) {
        drmu_err(du, "Failed to create pollqueue");
        goto fail1;
    }
    if ((du->pt = polltask_new(du->pq, du->fd, POLLIN | POLLPRI, drmu_env_polltask_cb, du)) == NULL) {
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

    pollqueue_add_task(du->pt, 1000);

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
        if ((ret = drmu_atomic_plane_set(da, sys->subplanes[i], spe->fb,
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

    if ((sys->du = drmu_env_new_open(VLC_OBJECT(vd), DRM_MODULE)) == NULL)
        goto fail;

    if ((sys->dc = drmu_crtc_new_find(sys->du)) == NULL)
        goto fail;

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

