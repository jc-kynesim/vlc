/**
 * @file shm.c
 * @brief Wayland shared memory video output module for VLC media player
 */
/*****************************************************************************
 * Copyright © 2014, 2017 Rémi Denis-Courmont
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
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>
#include <vlc_fs.h>

// *** Avoid this include if possible
#include <libdrm/drm_fourcc.h>

#include "dmabuf_alloc.h"
#include "picpool.h"
#include "rgba_premul.h"
#include "../drmu/drmu_vlc_fmts.h"
#include "../drmu/pollqueue.h"
#include "../../codec/avcodec/drm_pic.h"
#include <libavutil/hwcontext_drm.h>

#define TRACE_ALL 0

#define MAX_PICTURES 4
#define MAX_SUBPICS  6

#define VIDEO_ON_SUBSURFACE 0

#define WL_DMABUF_USE_SHM_NAME "wl-dmabuf-use-shm"
#define WL_DMABUF_USE_SHM_TEXT N_("Attempt to map via shm")
#define WL_DMABUF_USE_SHM_LONGTEXT N_("Attempt to map via shm rather than linux_dmabuf")

typedef struct fmt_ent_s {
    uint32_t fmt;
    int32_t pri;
    uint64_t mod;
} fmt_ent_t;

typedef struct fmt_list_s {
    fmt_ent_t * fmts;
    unsigned int size;
    unsigned int len;
} fmt_list_t;

typedef struct subplane_s {
    struct wl_surface * surface;
    struct wl_subsurface * subsurface;
    struct wp_viewport * viewport;
} subplane_t;

typedef struct subpic_ent_s {
    struct wl_buffer * wb;
    struct dmabuf_h * dh;
    picture_t * pic;
    int alpha;
    vout_display_place_t dst_rect;
    vout_display_place_t src_rect;
    bool update;
} subpic_ent_t;

typedef struct video_dmabuf_release_env_ss
{
    struct picture_context_t * ctx;
    unsigned int rel_count;
    unsigned int pt_count;
    struct polltask * pt[AV_DRM_MAX_PLANES];
} video_dmabuf_release_env_t;

struct vout_display_sys_t
{
    vout_window_t *embed; /* VLC window */
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct zwp_linux_dmabuf_v1 * linux_dmabuf_v1;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;

    picture_pool_t *vlc_pic_pool; /* picture pool */

    struct wl_surface * last_embed_surface;

    int x;
    int y;
    bool video_attached;
    bool viewport_set;
    bool use_shm;

    vout_display_place_t spu_rect;  // Window that subpic coords orignate from
    vout_display_place_t dst_rect;  // Window in the display size that holds the video

    video_format_t curr_aspect;

#if VIDEO_ON_SUBSURFACE
    struct wl_surface * video_surface;
    struct wl_subsurface * video_subsurface;

    struct wp_viewport * bkg_viewport;
    unsigned int bkg_w;
    unsigned int bkg_h;
#endif

    struct pollqueue * pollq;

    picpool_ctl_t * subpic_pool;
    subplane_t subplanes[MAX_SUBPICS];
    subpic_ent_t subpics[MAX_SUBPICS];
    subpic_ent_t piccpy;
    video_dmabuf_release_env_t * pic_vdre;
    vlc_fourcc_t * subpic_chromas;

    fmt_list_t dmabuf_fmts;
    fmt_list_t shm_fmts;
};

static inline struct wl_display *
video_display(const vout_display_sys_t * const sys)
{
    return sys->embed->display.wl;
}

static inline struct wl_surface *
video_surface(const vout_display_sys_t * const sys)
{
#if VIDEO_ON_SUBSURFACE
    return sys->video_surface;
#else
    return sys->embed->handle.wl;
#endif
}

static inline struct wl_compositor *
video_compositor(const vout_display_sys_t * const sys)
{
    return sys->compositor;
}

#if VIDEO_ON_SUBSURFACE
static inline struct wl_surface *
bkg_surface(const vout_display_sys_t * const sys)
{
    return sys->embed->handle.wl;
}
#endif

static int
check_embed(vout_display_t * const vd, vout_display_sys_t * const sys, const char * const func)
{
    if (!sys->embed) {
        msg_Err(vd, "%s: Embed NULL", func);
        return -1;
    }
    if (sys->embed->handle.wl != sys->last_embed_surface) {
        msg_Warn(vd, "%s: Embed surface changed %p->%p", func, sys->last_embed_surface, sys->embed->handle.wl);
        sys->last_embed_surface = sys->embed->handle.wl;
        return 1;
    }
    return 0;
}

static inline void
roundtrip(const vout_display_sys_t * const sys)
{
    wl_display_roundtrip(video_display(sys));
}

static void
buffer_destroy(struct wl_buffer ** ppbuffer)
{
    struct wl_buffer * const buffer = *ppbuffer;
    if (buffer == NULL)
        return;
    *ppbuffer = NULL;
    wl_buffer_destroy(buffer);
}

static void
subsurface_destroy(struct wl_subsurface ** const ppsubsurface)
{
    if (*ppsubsurface == NULL)
        return;
    wl_subsurface_destroy(*ppsubsurface);
    *ppsubsurface = NULL;
}

static void
surface_destroy(struct wl_surface ** const ppsurface)
{
    if (*ppsurface == NULL)
        return;
    wl_surface_destroy(*ppsurface);
    *ppsurface = NULL;
}

static void
shm_destroy(struct wl_shm ** const ppshm)
{
    if (*ppshm == NULL)
        return;
    wl_shm_destroy(*ppshm);
    *ppshm = NULL;
}

static void
viewport_destroy(struct wp_viewport ** const ppviewport)
{
    if (*ppviewport == NULL)
        return;
    wp_viewport_destroy(*ppviewport);
    *ppviewport = NULL;
}

static inline int_fast32_t
place_rescale_1s(int_fast32_t x, uint_fast32_t mul, uint_fast32_t div)
{
    const int_fast64_t m = x * (int_fast64_t)mul;
    const uint_fast32_t d2 = div/2;
    return div == 0 ? (int_fast32_t)m :
        m >= 0 ? (int_fast32_t)(((uint_fast64_t)m + d2) / div) :
            -(int_fast32_t)(((uint_fast64_t)(-m) + d2) / div);
}

static inline uint_fast32_t
place_rescale_1u(uint_fast32_t x, uint_fast32_t mul, uint_fast32_t div)
{
    const uint_fast64_t m = x * (uint_fast64_t)mul;
    return (uint_fast32_t)(div == 0 ? m : (m + div/2) / div);
}

static inline vout_display_place_t
place_rescale(const vout_display_place_t s, const vout_display_place_t mul, const vout_display_place_t div)
{
    return (vout_display_place_t){
        .x      = place_rescale_1s(s.x - div.x, mul.width,  div.width)  + mul.x,
        .y      = place_rescale_1s(s.y - div.y, mul.height, div.height) + mul.y,
        .width  = place_rescale_1u(s.width,     mul.width,  div.width),
        .height = place_rescale_1u(s.height,    mul.height, div.height)
    };
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

#if 0
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
place_dest_rect(vout_display_t * const vd,
          const vout_display_cfg_t * const cfg,
          const video_format_t * fmt)
{
    vout_display_sys_t * const sys = vd->sys;
    sys->dest_rect = rect_transform(place_out(cfg, fmt, sys->win_rect),
                                    sys->display_rect, sys->dest_transform);
}
#endif

static void
place_spu_rect(vout_display_t * const vd,
          const vout_display_cfg_t * const cfg,
          const video_format_t * fmt)
{
    vout_display_sys_t * const sys = vd->sys;
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


static int
fmt_list_add(fmt_list_t * const fl, uint32_t fmt, uint64_t mod, int32_t pri)
{
    if (fl->len >= fl->size)
    {
        unsigned int n = fl->len == 0 ? 64 : fl->len * 2;
        fmt_ent_t * t = realloc(fl->fmts, n * sizeof(*t));
        if (t == NULL)
            return VLC_ENOMEM;
        fl->fmts = t;
        fl->size = n;
    }
    fl->fmts[fl->len++] = (fmt_ent_t){
        .fmt = fmt,
        .pri = pri,
        .mod = mod
    };
    return 0;
}

static int
fmt_sort_cb(const void * va, const void * vb)
{
    const fmt_ent_t * const a = va;
    const fmt_ent_t * const b = vb;
    return a->fmt < b->fmt ? -1 : a->fmt != b->fmt ? 1 :
           a->mod < b->mod ? -1 : a->mod != b->mod ? 1 : 0;
}

static void
fmt_list_sort(fmt_list_t * const fl)
{
    unsigned int n = 0;
    if (fl->len <= 1)
        return;
    qsort(fl->fmts, fl->len, sizeof(*fl->fmts), fmt_sort_cb);
    // Dedup - in case we have multiple working callbacks
    for (unsigned int i = 1; i != fl->len; ++i)
    {
        if (fl->fmts[i].fmt != fl->fmts[n].fmt || fl->fmts[i].mod != fl->fmts[n].mod)
            fl->fmts[n++] = fl->fmts[i];
    }
    fl->len = n + 1;
}

static int
fmt_list_find(const fmt_list_t * const fl, uint32_t fmt, uint64_t mod)
{
    const fmt_ent_t x = {
        .fmt = fmt,
        .mod = mod
    };
    const fmt_ent_t * const fe = (fl->len == 0) ? NULL :
        bsearch(&x, fl->fmts, fl->len, sizeof(x), fmt_sort_cb);
    return fe == NULL ? -1 : fe->pri;
}

static void
fmt_list_uninit(fmt_list_t * const fl)
{
    free(fl->fmts);
    fl->fmts = NULL;
    fl->size = 0;
    fl->len = 0;
}

static int
fmt_list_init(fmt_list_t * const fl, const size_t initial_size)
{
    fl->size = 0;
    fl->len = 0;
    if ((fl->fmts = malloc(initial_size * sizeof(*fl->fmts))) == NULL)
        return VLC_ENOMEM;
    fl->size = initial_size;
    return VLC_SUCCESS;
}

static void
chequerboard(uint32_t *const data, unsigned int stride, const unsigned int width, const unsigned int height)
{
    stride /= sizeof(uint32_t);

    /* Draw checkerboxed background */
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * stride + x] = 0xFF666666;
            else
                data[y * stride + x] = 0xFFEEEEEE;
        }
    }
}

/* Sent by the compositor when it's no longer using this buffer */
static void
subpic_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct dmabuf_h * dh = data;

    buffer_destroy(&wl_buffer);
    dmabuf_unref(&dh);
}

static const struct wl_buffer_listener subpic_buffer_listener = {
    .release = subpic_buffer_release,
};

static inline size_t cpypic_plane_alloc_size(const plane_t * const p)
{
    return p->i_pitch * p->i_lines;
}

static int
copy_subpic_to_w_buffer(vout_display_t *vd, vout_display_sys_t * const sys, picture_t * const src,
                        int alpha,
                        struct dmabuf_h ** pDmabuf_h, struct wl_buffer ** pW_buffer)
{
    unsigned int w = src->format.i_width;
    unsigned int h = src->format.i_height;
    struct zwp_linux_buffer_params_v1 *params = NULL;
    uint64_t mod;
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format, &mod);
    size_t total_size = 0;
    size_t offset = 0;
    struct dmabuf_h * dh = NULL;
    int i;

    for (i = 0; i != src->i_planes; ++i)
        total_size += cpypic_plane_alloc_size(src->p + i);

    *pW_buffer = NULL;
    *pDmabuf_h = NULL;

    if ((dh = picpool_get(sys->subpic_pool, total_size)) == NULL)
    {
        msg_Warn(vd, "Failed to alloc dmabuf for subpic");
        goto error;
    }
    *pDmabuf_h = dh;

    if (sys->use_shm)
    {
        struct wl_shm_pool *pool = wl_shm_create_pool(sys->shm, dmabuf_fd(dh), dmabuf_size(dh));
        const uint32_t w_fmt = drm_fmt == DRM_FORMAT_ARGB8888 ? 0 :
            drm_fmt == DRM_FORMAT_XRGB8888 ? 1 : drm_fmt;
        const size_t stride = src->p[0].i_pitch;
        const size_t size = cpypic_plane_alloc_size(src->p + 0);

        assert(src->i_planes == 1);

        if (pool == NULL)
        {
            msg_Err(vd, "Failed to create pool from dmabuf");
            goto error;
        }
        *pW_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, w_fmt);
        wl_shm_pool_destroy(pool);

        if (*pW_buffer == NULL)
        {
            msg_Err(vd, "Failed to create buffer from pool");
            goto error;
        }

        if (src->format.i_chroma == VLC_CODEC_RGBA ||
            src->format.i_chroma == VLC_CODEC_BGRA)
            copy_frame_xxxa_with_premul(dmabuf_map(dh), stride, src->p[0].p_pixels, stride, w, h, alpha);
        else
            memcpy((char *)dmabuf_map(dh) + offset, src->p[0].p_pixels, size);
    }
    else
    {
        if ((params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1)) == NULL)
        {
            msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
            goto error;
        }

        dmabuf_write_start(dh);
        for (i = 0; i != src->i_planes; ++i)
        {
            const size_t stride = src->p[i].i_pitch;
            const size_t size = cpypic_plane_alloc_size(src->p + i);

            if (src->format.i_chroma == VLC_CODEC_RGBA ||
                src->format.i_chroma == VLC_CODEC_BGRA)
                copy_frame_xxxa_with_premul(dmabuf_map(dh), stride, src->p[i].p_pixels, stride, w, h, alpha);
            else
                memcpy((char *)dmabuf_map(dh) + offset, src->p[i].p_pixels, size);

            zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), i, offset, stride, 0, 0);

            offset += size;
        }
        dmabuf_write_end(dh);

        if ((*pW_buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h, drm_fmt, 0)) == NULL)
        {
            msg_Err(vd, "zwp_linux_buffer_params_v1_create_immed FAILED");
            goto error;
        }

        zwp_linux_buffer_params_v1_destroy(params);
    }
    wl_buffer_add_listener(*pW_buffer, &subpic_buffer_listener, dh);

    return VLC_SUCCESS;

error:
    if (params)
        zwp_linux_buffer_params_v1_destroy(params);
    dmabuf_unref(pDmabuf_h);
    return VLC_EGENERIC;
}


static void kill_pool(vout_display_sys_t * const sys)
{
    if (sys->vlc_pic_pool != NULL)
    {
        picture_pool_Release(sys->vlc_pic_pool);
        sys->vlc_pic_pool = NULL;
    }
}

// Actual picture pool for dmabufs is just a set of trivial containers
static picture_pool_t *vd_dmabuf_pool(vout_display_t * const vd, unsigned count)
{
    vout_display_sys_t * const sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "%s: fmt:%dx%d,sar:%d/%d; source:%dx%d", __func__,
            vd->fmt.i_width, vd->fmt.i_height, vd->fmt.i_sar_num, vd->fmt.i_sar_den, vd->source.i_width, vd->source.i_height);
#endif

    if (sys->vlc_pic_pool == NULL)
        sys->vlc_pic_pool = picture_pool_NewFromFormat(&vd->fmt, count);
    return sys->vlc_pic_pool;
}

static video_dmabuf_release_env_t *
vdre_new(struct picture_context_t * ctx)
{
    video_dmabuf_release_env_t * const vdre = calloc(1, sizeof(*vdre));
    if ((vdre->ctx = ctx->copy(ctx)) == NULL)
    {
        free(vdre);
        return NULL;
    }
    return vdre;
}

static void
vdre_free(video_dmabuf_release_env_t * const vdre)
{
    unsigned int i;
    vdre->ctx->destroy(vdre->ctx);
    for (i = 0; i != vdre->pt_count; ++i)
        polltask_delete(vdre->pt + i);
    free(vdre);
}

static void
vdre_delete(video_dmabuf_release_env_t ** const ppvdre)
{
    video_dmabuf_release_env_t * const vdre = *ppvdre;
    if (vdre == NULL)
        return;
    *ppvdre = NULL;
    vdre_free(vdre);
}

static void
w_ctx_release(void * v, short revents)
{
    video_dmabuf_release_env_t * const vdre = v;
    VLC_UNUSED(revents);
    // Wait for all callbacks to come back before releasing buffer
    if (++vdre->rel_count >= vdre->pt_count)
        vdre_free(vdre);
}

static void
vdre_add_pt(video_dmabuf_release_env_t * const vdre, struct pollqueue * pq, int fd)
{
    assert(vdre->pt_count < AV_DRM_MAX_PLANES);
    vdre->pt[vdre->pt_count++] = polltask_new(pq, fd, POLLOUT, w_ctx_release, vdre);
}

// Avoid use of vd here as there's a possibility this will be called after
// it has gone
static void
w_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    video_dmabuf_release_env_t * const vdre = data;
    unsigned int i = vdre->pt_count;

    /* Sent by the compositor when it's no longer using this buffer */
    buffer_destroy(&wl_buffer);

    // Whilst we can happily destroy the buffer that doesn't mean we can reuse
    // the dmabufs yet - we have to wait for them to be free of fences.
    // We don't want to wait in this callback so do the waiting in pollqueue
    while (i-- != 0)
        pollqueue_add_task(vdre->pt[i], 1000);
}

static const struct wl_buffer_listener w_buffer_listener = {
    .release = w_buffer_release,
};

static int
do_display_dmabuf(vout_display_t * const vd, vout_display_sys_t * const sys, picture_t * const pic,
                  video_dmabuf_release_env_t ** const pVdre, struct wl_buffer ** const pWbuffer)
{
    struct zwp_linux_buffer_params_v1 *params = NULL;
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
    const uint32_t format = desc->layers[0].format;
    const unsigned int width = pic->format.i_visible_width;
    const unsigned int height = pic->format.i_visible_height;
    unsigned int n = 0;
    unsigned int flags = 0;
    int i;
    struct wl_buffer * w_buffer;
    video_dmabuf_release_env_t * const vdre = vdre_new(pic->context);

    assert(*pWbuffer == NULL);
    assert(*pVdre == NULL);

    if (vdre == NULL) {
        msg_Err(vd, "Failed to create vdre");
        return VLC_ENOMEM;
    }

    for (i = 0; i != desc->nb_objects; ++i)
        vdre_add_pt(vdre, sys->pollq, desc->objects[i].fd);

    if (sys->use_shm)
    {
        const AVDRMPlaneDescriptor *const p = desc->layers[0].planes + 0;
        struct wl_shm_pool *pool = wl_shm_create_pool(sys->shm, desc->objects[0].fd, desc->objects[0].size);
        const uint32_t w_fmt = format == DRM_FORMAT_ARGB8888 ? 0 :
            format == DRM_FORMAT_XRGB8888 ? 1 : format;

        if (pool == NULL)
        {
            msg_Err(vd, "Failed to create pool from dmabuf");
            goto error;
        }
        w_buffer = wl_shm_pool_create_buffer(pool, p->offset, width, height, p->pitch, w_fmt);
        wl_shm_pool_destroy(pool);
        if (w_buffer == NULL)
        {
            msg_Err(vd, "Failed to create buffer from pool");
            goto error;
        }
    }
    else
    {
        /* Creation and configuration of planes  */
        params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1);
        if (!params)
        {
            msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
            goto error;
        }

        for (i = 0; i < desc->nb_layers; ++i)
        {
            int j;
            for (j = 0; j < desc->layers[i].nb_planes; ++j)
            {
                const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
                const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;

                zwp_linux_buffer_params_v1_add(params, obj->fd, n++, p->offset, p->pitch,
                                   (unsigned int)(obj->format_modifier >> 32),
                                   (unsigned int)(obj->format_modifier & 0xFFFFFFFF));
            }
        }

        if (!pic->b_progressive)
        {
            flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED;
            if (!pic->b_top_field_first)
                flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;
        }

        /* Request buffer creation */
        if ((w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, flags)) == NULL)
        {
            msg_Err(vd, "zwp_linux_buffer_params_v1_create_immed FAILED");
            goto error;
        }

        zwp_linux_buffer_params_v1_destroy(params);
        params = NULL;
    }

    wl_buffer_add_listener(w_buffer, &w_buffer_listener, vdre);

    *pVdre = vdre;
    *pWbuffer = w_buffer;
    return VLC_SUCCESS;

error:
    if (params)
        zwp_linux_buffer_params_v1_destroy(params);
    vdre_free(vdre);
    return VLC_EGENERIC;
}

static void
subpic_ent_flush(subpic_ent_t * const spe)
{
    if (spe->pic != NULL) {
        picture_Release(spe->pic);
        spe->pic = NULL;
    }
    buffer_destroy(&spe->wb);
    dmabuf_unref(&spe->dh);
}

static void
subpic_ent_attach(struct wl_surface * const surface, subpic_ent_t * const spe)
{
    wl_surface_attach(surface, spe->wb, 0, 0);
    spe->dh = NULL;
    spe->wb = NULL;
}

static void
mark_all_surface_opaque(struct wl_compositor * compositor, struct wl_surface * surface)
{
    struct wl_region * region_all = wl_compositor_create_region(compositor);
    wl_region_add(region_all, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface, region_all);
    wl_region_destroy(region_all);
}

static int
make_video_surface(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    VLC_UNUSED(vd);

    if (sys->viewport)
        return VLC_SUCCESS;

#if VIDEO_ON_SUBSURFACE
    // Make a new subsurface to use for video
    sys->video_surface = wl_compositor_create_surface(video_compositor(sys));
    sys->video_subsurface = wl_subcompositor_get_subsurface(sys->subcompositor, sys->video_surface, bkg_surface(sys));
    wl_subsurface_place_above(sys->video_subsurface, bkg_surface(sys));
    wl_subsurface_set_desync(sys->video_subsurface);  // Video update can be desync from main window
#endif

    struct wl_surface * const surface = video_surface(sys);

    // Video is opaque
    mark_all_surface_opaque(video_compositor(sys), surface);

    sys->viewport = wp_viewporter_get_viewport(sys->viewporter, surface);

    /* Determine our pixel format */
    static const enum wl_output_transform transforms[8] = {
        [ORIENT_TOP_LEFT] = WL_OUTPUT_TRANSFORM_NORMAL,
        [ORIENT_TOP_RIGHT] = WL_OUTPUT_TRANSFORM_FLIPPED,
        [ORIENT_BOTTOM_LEFT] = WL_OUTPUT_TRANSFORM_FLIPPED_180,
        [ORIENT_BOTTOM_RIGHT] = WL_OUTPUT_TRANSFORM_180,
        [ORIENT_LEFT_TOP] = WL_OUTPUT_TRANSFORM_FLIPPED_270,
        [ORIENT_LEFT_BOTTOM] = WL_OUTPUT_TRANSFORM_90,
        [ORIENT_RIGHT_TOP] = WL_OUTPUT_TRANSFORM_270,
        [ORIENT_RIGHT_BOTTOM] = WL_OUTPUT_TRANSFORM_FLIPPED_90,
    };

    wl_surface_set_buffer_transform(surface, transforms[vd->fmt.orientation]);
    return VLC_SUCCESS;
}

static int
make_subpic_surfaces(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    unsigned int i;
    struct wl_surface * const surface = video_surface(sys);
    struct wl_surface * below = surface;
    VLC_UNUSED(vd);

    if (sys->subplanes[0].surface)
        return VLC_SUCCESS;

    for (i = 0; i != MAX_SUBPICS; ++i)
    {
        subplane_t *plane = sys->subplanes + i;
        plane->surface = wl_compositor_create_surface(video_compositor(sys));
        plane->subsurface = wl_subcompositor_get_subsurface(sys->subcompositor, plane->surface, surface);
        wl_subsurface_place_above(plane->subsurface, below);
        below = plane->surface;
        wl_subsurface_set_sync(plane->subsurface);
        plane->viewport = wp_viewporter_get_viewport(sys->viewporter, plane->surface);
    }
    return VLC_SUCCESS;
}

static int
make_background(vout_display_t * const vd, vout_display_sys_t * const sys)
{
#if !VIDEO_ON_SUBSURFACE
    VLC_UNUSED(vd);
    VLC_UNUSED(sys);
    return VLC_SUCCESS;
#else
    // Build a background
    // This would be a perfect use of the single_pixel_surface extension
    // However we don't seem to support it
    struct dmabuf_h * dh = NULL;

    if (!sys->bkg_viewport)
    {
        unsigned int width = 640;
        unsigned int height = 480;
        unsigned int stride = 640 * 4;
        struct zwp_linux_buffer_params_v1 *params;
        struct wl_buffer * w_buffer;

        if ((dh = picpool_get(sys->subpic_pool, stride * height)) == NULL) {
            msg_Err(vd, "Failed to get DmaBuf for background");
            goto error;
        }

        dmabuf_write_start(dh);
        chequerboard(dmabuf_map(dh), stride, width, height);
        dmabuf_write_end(dh);

        params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1);
        if (!params) {
            msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
            goto error;
        }
        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
        w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!w_buffer) {
            msg_Err(vd, "Failed to create background buffer");
            goto error;
        }

        sys->bkg_viewport = wp_viewporter_get_viewport(sys->viewporter, bkg_surface(sys));

        wl_buffer_add_listener(w_buffer, &subpic_buffer_listener, dh);
        wl_surface_attach(bkg_surface(sys), w_buffer, 0, 0);
        dh = NULL;

        wp_viewport_set_destination(sys->bkg_viewport, sys->bkg_w, sys->bkg_h);
        mark_all_surface_opaque(video_compositor(sys), bkg_surface(sys));

        wl_surface_commit(bkg_surface(sys));
    }
    return VLC_SUCCESS;

error:
    dmabuf_unref(&dh);
    return VLC_ENOMEM;
#endif
}

static void
set_video_viewport(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    video_format_t fmt;

    if (!sys->video_attached || sys->viewport_set)
        return;

    sys->viewport_set = true;

    video_format_ApplyRotation(&fmt, &vd->source);
    wp_viewport_set_source(sys->viewport,
                    wl_fixed_from_int(fmt.i_x_offset),
                    wl_fixed_from_int(fmt.i_y_offset),
                    wl_fixed_from_int(fmt.i_visible_width),
                    wl_fixed_from_int(fmt.i_visible_height));
    wp_viewport_set_destination(sys->viewport,
                    sys->dst_rect.width, sys->dst_rect.height);
#if VIDEO_ON_SUBSURFACE
    wl_subsurface_set_position(sys->video_subsurface, sys->dst_rect.x, sys->dst_rect.y);
#endif
}

static void Prepare(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t * const sys = vd->sys;
    unsigned int n = 0;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif
    check_embed(vd, sys, __func__);

    if (drmu_format_vlc_to_drm_prime(&pic->format, NULL) == 0) {
        copy_subpic_to_w_buffer(vd, sys, pic, 0xff, &sys->piccpy.dh, &sys->piccpy.wb);
    }
    else {
        do_display_dmabuf(vd, sys, pic, &sys->pic_vdre, &sys->piccpy.wb);
    }

    // Attempt to import the subpics
    for (subpicture_t * spic = subpic; spic != NULL; spic = spic->p_next)
    {
        for (subpicture_region_t *sreg = spic->p_region; sreg != NULL; sreg = sreg->p_next) {
            picture_t * const src = sreg->p_picture;
            subpic_ent_t * const dst = sys->subpics + n;

            // If the same picture then assume the same contents
            // We keep a ref to the previous pic to ensure that the same picture
            // structure doesn't get reused and confuse us.
            if (src != dst->pic || sreg->i_alpha != dst->alpha) {
                subpic_ent_flush(dst);

                if (copy_subpic_to_w_buffer(vd, sys, src, sreg->i_alpha, &dst->dh, &dst->wb) != 0)
                    continue;

                dst->pic = picture_Hold(src);
                dst->alpha = sreg->i_alpha;
                dst->update = true;
            }

            dst->src_rect = (vout_display_place_t) {
                .x = sreg->fmt.i_x_offset,
                .y = sreg->fmt.i_y_offset,
                .width = sreg->fmt.i_visible_width,
                .height = sreg->fmt.i_visible_height,
            };
            dst->dst_rect = place_rescale(
                (vout_display_place_t) {
                    .x = sreg->i_x,
                    .y = sreg->i_y,
                    .width = sreg->fmt.i_visible_width,
                    .height = sreg->fmt.i_visible_height,
                },
                (vout_display_place_t) {
                    .x = 0,
                    .y = 0,
                    .width  = sys->dst_rect.width,
                    .height = sys->dst_rect.height,
                },
                sys->spu_rect);

            if (++n == MAX_SUBPICS)
                goto subpics_done;
        }
    }
subpics_done:

    // Clear any other entries
    for (; n != MAX_SUBPICS; ++n) {
        subpic_ent_t * const dst = sys->subpics + n;

        if (dst->dh != NULL)
            dst->update = true;
        subpic_ent_flush(dst);
    }

    (void)pic;

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif
}

static void Display(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t * const sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif

    check_embed(vd, sys, __func__);

    make_video_surface(vd, sys);
    make_subpic_surfaces(vd, sys);
    make_background(vd, sys);

    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subpic_ent_t * const spe = sys->subpics + i;

        if (!spe->update)
            continue;

        msg_Dbg(vd, "%s: Update subpic %i: wb=%p alpha=%d", __func__, i, spe->wb, spe->alpha);
        subpic_ent_attach(sys->subplanes[i].surface, spe);

        wl_subsurface_set_position(sys->subplanes[i].subsurface, spe->dst_rect.x, spe->dst_rect.y);
        wp_viewport_set_source(sys->subplanes[i].viewport,
                               wl_fixed_from_int(spe->src_rect.x), wl_fixed_from_int(spe->src_rect.y),
                               wl_fixed_from_int(spe->src_rect.width), wl_fixed_from_int(spe->src_rect.height));
        wp_viewport_set_destination(sys->subplanes[i].viewport, spe->dst_rect.width, spe->dst_rect.height);
        wl_surface_damage(sys->subplanes[i].surface, 0, 0, INT32_MAX, INT32_MAX);

        wl_surface_commit(sys->subplanes[i].surface);
        spe->update = false;
    }

    if (!sys->piccpy.wb) {
        msg_Warn(vd, "Display called but prepared pic buffer");
    }
    else {
        subpic_ent_attach(video_surface(sys), &sys->piccpy);
        sys->video_attached = true;
        // Now up to the buffer callback to free stuff
        sys->pic_vdre = NULL;
    }

    set_video_viewport(vd, sys);

    wl_surface_damage(video_surface(sys), 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(video_surface(sys));

    // With VIDEO_ON_SUBSURFACE we need a commit on the background here
    // too if the video surface isn't desync.  Desync is set, but wayland
    // can force sync if the bkg surface is a sync subsurface.
    // Try adding a bkg surface commit here if things freeze with
    // VIDEO_ON_SUBSURFACE set but don't with it unset

    wl_display_flush(video_display(sys));
//    roundtrip(sys);

    if (subpic)
        subpicture_Delete(subpic);
    picture_Release(pic);

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif
}

static void ResetPictures(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    check_embed(vd, sys, __func__);

    kill_pool(sys);

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}

static int Control(vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t * const sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: Query=%d", __func__, query);
#endif
    check_embed(vd, sys, __func__);

    switch (query)
    {
        case VOUT_DISPLAY_RESET_PICTURES:
        {
            vout_display_place_t place;
            video_format_t src;

            assert(sys->viewport == NULL);

            vout_display_PlacePicture(&place, &vd->source, vd->cfg, false);
            video_format_ApplyRotation(&src, &vd->source);

            vd->fmt.i_width  = src.i_width  * place.width
                                            / src.i_visible_width;
            vd->fmt.i_height = src.i_height * place.height
                                            / src.i_visible_height;
            vd->fmt.i_visible_width  = place.width;
            vd->fmt.i_visible_height = place.height;
            vd->fmt.i_x_offset = src.i_x_offset * place.width
                                                / src.i_visible_width;
            vd->fmt.i_y_offset = src.i_y_offset * place.height
                                                / src.i_visible_height;
            ResetPictures(vd);
            sys->curr_aspect = vd->source;
            break;
        }

        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
        case VOUT_DISPLAY_CHANGE_ZOOM:
        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        {
            const vout_display_cfg_t *cfg;

            if (query == VOUT_DISPLAY_CHANGE_SOURCE_ASPECT
             || query == VOUT_DISPLAY_CHANGE_SOURCE_CROP)
            {
                cfg = vd->cfg;
            }
            else
            {
                cfg = va_arg(ap, const vout_display_cfg_t *);
            }

#if !VIDEO_ON_SUBSURFACE
            vout_display_place_t place;

            vout_display_PlacePicture(&place, &sys->curr_aspect, vd->cfg, false);
            sys->x += place.width / 2;
            sys->y += place.height / 2;

            vout_display_PlacePicture(&sys->dst_rect, &vd->source, cfg, false);
            sys->x -= sys->dst_rect.width / 2;
            sys->y -= sys->dst_rect.height / 2;
#else
            vout_display_PlacePicture(&sys->dst_rect, &vd->source, cfg, true);
#endif

            place_spu_rect(vd, cfg, &vd->fmt);
            sys->viewport_set = false;

            if (sys->viewport)
                set_video_viewport(vd, sys);

#if VIDEO_ON_SUBSURFACE
            if (sys->bkg_viewport != NULL && (cfg->display.width != sys->bkg_w || cfg->display.height != sys->bkg_h))
            {
                sys->bkg_w = cfg->display.width;
                sys->bkg_h = cfg->display.height;

                msg_Dbg(vd, "Resize background: %dx%d; surface=%p", cfg->display.width, cfg->display.height, bkg_surface(sys));
                wp_viewport_set_destination(sys->bkg_viewport, cfg->display.width, cfg->display.height);
                wl_surface_commit(bkg_surface(sys));
            }
#endif

            sys->curr_aspect = vd->source;
            break;
        }
        default:
             msg_Err(vd, "unknown request %d", query);
             return VLC_EGENERIC;
    }

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif
    return VLC_SUCCESS;
}

static void linux_dmabuf_v1_listener_format(void *data,
               struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
               uint32_t format)
{
    // Superceeded by _modifier
    vout_display_t * const vd = data;
    vout_display_sys_t * const sys = vd->sys;
    (void)zwp_linux_dmabuf_v1;

    msg_Dbg(vd, "%s[%p], %.4s", __func__, (void*)vd, (const char *)&format);

    fmt_list_add(&sys->dmabuf_fmts, format, DRM_FORMAT_MOD_LINEAR, 0);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
         struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
         uint32_t format,
         uint32_t modifier_hi,
         uint32_t modifier_lo)
{
    vout_display_t * const vd = data;
    vout_display_sys_t * const sys = vd->sys;
    (void)zwp_linux_dmabuf_v1;

    msg_Dbg(vd, "%s[%p], %.4s %08x%08x", __func__, (void*)vd, (const char *)&format, modifier_hi, modifier_lo);

    fmt_list_add(&sys->dmabuf_fmts, format, modifier_lo | ((uint64_t)modifier_hi << 32), 0);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
    .format = linux_dmabuf_v1_listener_format,
    .modifier = linux_dmabuf_v1_listener_modifier,
};

static void shm_listener_format(void *data,
               struct wl_shm *shm,
               uint32_t format)
{
    vout_display_t * const vd = data;
    vout_display_sys_t * const sys = vd->sys;
    (void)shm;

    if (format == 0)
        format = DRM_FORMAT_ARGB8888;
    else if (format == 1)
        format = DRM_FORMAT_XRGB8888;

    msg_Dbg(vd, "%s[%p], %.4s", __func__, (void*)vd, (const char *)&format);

    fmt_list_add(&sys->shm_fmts, format, DRM_FORMAT_MOD_LINEAR, 0);
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_listener_format,
};


static void registry_global_cb(void *data, struct wl_registry *registry,
                               uint32_t name, const char *iface, uint32_t vers)
{
    vout_display_t * const vd = data;
    vout_display_sys_t * const sys = vd->sys;

    msg_Dbg(vd, "global %3"PRIu32": %s version %"PRIu32, name, iface, vers);

    if (strcmp(iface, wl_subcompositor_interface.name) == 0)
        sys->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    else
    if (strcmp(iface, wl_shm_interface.name) == 0)
        sys->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else
    if (!strcmp(iface, wp_viewporter_interface.name))
        sys->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else
    if (!strcmp(iface, wl_compositor_interface.name))
    {
        if (vers >= 4)
            sys->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
        else
            msg_Warn(vd, "Interface %s wanted v 4 got v %d", wl_compositor_interface.name, vers);
    }
    else
    if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0)
    {
        if (vers >= 3)
            sys->linux_dmabuf_v1 = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        else
            msg_Warn(vd, "Interface %s wanted v 3 got v %d", zwp_linux_dmabuf_v1_interface.name, vers);
    }

}

static void registry_global_remove_cb(void *data, struct wl_registry *registry,
                                      uint32_t name)
{
    vout_display_t *vd = data;

    msg_Dbg(vd, "global remove %3"PRIu32, name);
    (void) registry;
}

static const struct wl_registry_listener registry_cbs =
{
    registry_global_cb,
    registry_global_remove_cb,
};


static void
clear_surface_buffer(struct wl_surface * surface)
{
    if (surface == NULL)
        return;
    wl_surface_attach(surface, NULL, 0, 0);
    wl_surface_commit(surface);
}

static void
clear_all_buffers(vout_display_sys_t *sys)
{
    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subpic_ent_t * const spe = sys->subpics + i;
        subpic_ent_flush(spe);

        clear_surface_buffer(sys->subplanes[i].surface);
    }

    clear_surface_buffer(video_surface(sys));
    sys->video_attached = false;

#if VIDEO_ON_SUBSURFACE
    clear_surface_buffer(bkg_surface(sys));
#endif

    subpic_ent_flush(&sys->piccpy);
    vdre_delete(&sys->pic_vdre);
}


static void Close(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t * const sys = vd->sys;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif

    if (sys == NULL)
        return;

    if (sys->embed == NULL)
        goto no_window;

    check_embed(vd, sys, __func__);

    clear_all_buffers(sys);

    // Free subpic resources
    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subplane_t * const spl = sys->subplanes + i;

        viewport_destroy(&spl->viewport);
        subsurface_destroy(&spl->subsurface);
        surface_destroy(&spl->surface);
    }

    viewport_destroy(&sys->viewport);

#if VIDEO_ON_SUBSURFACE
    subsurface_destroy(&sys->video_subsurface);
    surface_destroy(&sys->video_surface);

    viewport_destroy(&sys->bkg_viewport);
#endif

    wl_display_flush(video_display(sys));

    if (sys->viewporter != NULL)
        wp_viewporter_destroy(sys->viewporter);
    if (sys->linux_dmabuf_v1 != NULL)
        zwp_linux_dmabuf_v1_destroy(sys->linux_dmabuf_v1);
    if (sys->subcompositor != NULL)
        wl_subcompositor_destroy(sys->subcompositor);
    if (sys->compositor != NULL)
        wl_compositor_destroy(sys->compositor);
    shm_destroy(&sys->shm);
    wl_display_flush(video_display(sys));

    vout_display_DeleteWindow(vd, sys->embed);
    sys->embed = NULL;

    kill_pool(sys);
    picpool_unref(&sys->subpic_pool);
    pollqueue_unref(&sys->pollq);

    free(sys->subpic_chromas);

no_window:
    fmt_list_uninit(&sys->dmabuf_fmts);
    fmt_list_uninit(&sys->shm_fmts);
    free(sys);

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}


static void reg_done_sync_cb(void * data, struct wl_callback * cb, unsigned int cb_data)
{
    vout_display_t * const vd = data;
    VLC_UNUSED(cb);
    VLC_UNUSED(cb_data);

    msg_Info(vd, "%s", __func__);
}

static const struct wl_callback_listener reg_done_sync_listener =
{
    .done = reg_done_sync_cb
};

static int Open(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t *sys;
    uint32_t pic_drm_fmt = 0;
    uint64_t pic_drm_mod = DRM_FORMAT_MOD_LINEAR;
    fmt_list_t * flist = NULL;

    msg_Info(vd, "<<< %s: %.4s %dx%d, cfg.display: %dx%d", __func__,
             (const char*)&vd->fmt.i_chroma, vd->fmt.i_width, vd->fmt.i_height,
             vd->cfg->display.width, vd->cfg->display.height);

    if (!(pic_drm_fmt = drmu_format_vlc_to_drm(&vd->fmt, &pic_drm_mod)))
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    vd->sys = sys;

    if (fmt_list_init(&sys->dmabuf_fmts, 128)) {
        msg_Err(vd, "Failed to allocate dmabuf format list!");
        goto error;
    }
    if (fmt_list_init(&sys->shm_fmts, 32)) {
        msg_Err(vd, "Failed to allocate shm format list!");
        goto error;
    }

    sys->use_shm = var_InheritBool(vd, WL_DMABUF_USE_SHM_NAME);

        /* Get window */
    sys->embed = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_WAYLAND);
    if (sys->embed == NULL)
        goto error;
    sys->last_embed_surface = sys->embed->handle.wl;

    {
        struct wl_callback * cb;
        struct wl_registry * const registry = wl_display_get_registry(video_display(sys));
        if (registry == NULL) {
            msg_Err(vd, "Cannot get registry for display");
            goto error;
        }

        wl_registry_add_listener(registry, &registry_cbs, vd);
        cb = wl_display_sync(video_display(sys));
        wl_callback_add_listener(cb, &reg_done_sync_listener, vd);

        roundtrip(sys);
        wl_registry_destroy(registry);
    }

    if (sys->compositor == NULL) {
        msg_Warn(vd, "Interface %s missing", wl_compositor_interface.name);
        goto error;
    }
    if (sys->subcompositor == NULL) {
        msg_Warn(vd, "Interface %s missing", wl_subcompositor_interface.name);
        goto error;
    }
    if (sys->viewporter == NULL) {
        msg_Warn(vd, "Interface %s missing", wp_viewporter_interface.name);
        goto error;
    }
    if (!sys->use_shm && sys->linux_dmabuf_v1 == NULL) {
        msg_Warn(vd, "Interface %s missing", zwp_linux_dmabuf_v1_interface.name);
        goto error;
    }

    // And again for registering formats
    if (sys->linux_dmabuf_v1)
        zwp_linux_dmabuf_v1_add_listener(sys->linux_dmabuf_v1, &linux_dmabuf_v1_listener, vd);
    wl_shm_add_listener(sys->shm, &shm_listener, vd);

    roundtrip(sys);

    fmt_list_sort(&sys->dmabuf_fmts);
    fmt_list_sort(&sys->shm_fmts);
    flist = sys->use_shm ? &sys->shm_fmts : &sys->dmabuf_fmts;

    // Check PIC DRM format here
    if (fmt_list_find(flist, pic_drm_fmt, pic_drm_mod) < 0) {
        msg_Warn(vd, "Could not find %.4s mod %#"PRIx64" in supported formats", (char*)&pic_drm_fmt, pic_drm_mod);
        goto error;
    }

    {
        static vlc_fourcc_t const tryfmts[] = {
            VLC_CODEC_RGBA,
            VLC_CODEC_BGRA,
            VLC_CODEC_ARGB,
        };
        unsigned int n = 0;

        if ((sys->subpic_chromas = calloc(ARRAY_SIZE(tryfmts) + 1, sizeof(vlc_fourcc_t))) == NULL)
            goto error;
        for (unsigned int i = 0; i != ARRAY_SIZE(tryfmts); ++i)
        {
            uint32_t drmfmt = drmu_format_vlc_chroma_to_drm(tryfmts[i]);
            msg_Dbg(vd, "Look for %.4s", (char*)&drmfmt);
            if (fmt_list_find(flist, drmfmt, DRM_FORMAT_MOD_LINEAR) >= 0)
                sys->subpic_chromas[n++] = tryfmts[i];
        }

        if (n == 0)
            msg_Warn(vd, "No compatible subpic formats found");
    }

    {
        struct dmabufs_ctl * dbsc = dmabufs_ctl_new();
        if (dbsc == NULL)
        {
            msg_Err(vd, "Failed to create dmabuf ctl");
            goto error;
        }
        sys->subpic_pool = picpool_new(dbsc);
        dmabufs_ctl_unref(&dbsc);
        if (sys->subpic_pool == NULL)
        {
            msg_Err(vd, "Failed to create picpool");
            goto error;
        }
    }

    if ((sys->pollq = pollqueue_new()) == NULL)
    {
        msg_Err(vd, "Failed to create pollqueue");
        goto error;
    }

    sys->curr_aspect = vd->source;
#if VIDEO_ON_SUBSURFACE
    sys->bkg_w = vd->cfg->display.width;
    sys->bkg_h = vd->cfg->display.height;
#endif

    vd->info.has_pictures_invalid = sys->viewport == NULL;
    vd->info.subpicture_chromas = sys->subpic_chromas;

    vd->pool = vd_dmabuf_pool;
    vd->prepare = Prepare;
    vd->display = Display;
    vd->control = Control;

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: OK", __func__);
#endif
    return VLC_SUCCESS;

error:
    Close(obj);
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: ERROR", __func__);
#endif
    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname(N_("WL DMABUF"))
    set_description(N_("Wayland dmabuf video output"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 0)
    set_callbacks(Open, Close)
    add_shortcut("wl-dmabuf")
    add_bool(WL_DMABUF_USE_SHM_NAME, false, WL_DMABUF_USE_SHM_TEXT, WL_DMABUF_USE_SHM_LONGTEXT, false)
vlc_module_end()
