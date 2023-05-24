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
#include "../drmu/drmu_vlc_fmts.h"
#include "../../codec/avcodec/drm_pic.h"
#include <libavutil/hwcontext_drm.h>

#define TRACE_ALL 0

#define MAX_PICTURES 4
#define MAX_SUBPICS  6

#define VIDEO_ON_SUBSURFACE 1

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
    vout_display_place_t dst_rect;
    vout_display_place_t src_rect;
    bool update;
} subpic_ent_t;


struct vout_display_sys_t
{
    vout_window_t *embed; /* VLC window */
    struct wl_event_queue *eventq;
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct zwp_linux_dmabuf_v1 * linux_dmabuf_v1_bind;
    struct wl_subcompositor *subcompositor;

    struct wl_shm *shm;
    int shm_fd;
    struct wl_shm_pool *shm_pool;
    void * shm_mmap;
    unsigned int shm_size;

    picture_pool_t *vlc_pic_pool; /* picture pool */

    int x;
    int y;
    bool use_buffer_transform;

    vout_display_place_t spu_rect;  // Window that subpic coords orignate from
    vout_display_place_t dst_rect;  // Window in the display size that holds the video

    video_format_t curr_aspect;

#if VIDEO_ON_SUBSURFACE
    struct wl_surface * video_surface;
    struct wl_subsurface * video_subsurface;

    struct wp_viewport * bkg_viewport;
#endif

    picpool_ctl_t * subpic_pool;
    subplane_t subplanes[MAX_SUBPICS];
    subpic_ent_t subpics[MAX_SUBPICS];
    vlc_fourcc_t * subpic_chromas;

    fmt_list_t dmabuf_fmts;
};

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
    return sys->embed->compositor.wl;
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


static void
shm_pool_init(vout_display_sys_t * const sys)
{
    sys->shm_fd = -1;
    sys->shm_mmap = MAP_FAILED;
    sys->shm_size = 0;
    sys->shm_pool = NULL;
}

static void
shm_pool_close(vout_display_sys_t * const sys)
{
    if (sys->shm_pool != NULL)
    {
        wl_shm_pool_destroy(sys->shm_pool);
        sys->shm_pool = NULL;
    }
    if (sys->shm_mmap != MAP_FAILED)
    {
        munmap(sys->shm_mmap, sys->shm_size);
        sys->shm_mmap = MAP_FAILED;
    }
    sys->shm_size = 0;
    if (sys->shm_fd != -1)
    {
        vlc_close(sys->shm_fd);
        sys->shm_fd = -1;
    }
}

static int
shm_pool_create(vout_display_t * const vd, vout_display_sys_t * const sys, unsigned int shm_size)
{
    const long pagemask = sysconf(_SC_PAGE_SIZE) - 1;

    sys->shm_fd = vlc_memfd();
    if (sys->shm_fd == -1)
    {
        msg_Err(vd, "cannot create buffers: %s", vlc_strerror_c(errno));
        goto error;
    }

    sys->shm_size = (shm_size + pagemask) & ~pagemask;

    if (ftruncate(sys->shm_fd, sys->shm_size))
    {
        msg_Err(vd, "cannot allocate buffers: %s", vlc_strerror_c(errno));
        goto error;
    }

    sys->shm_mmap = mmap(NULL, sys->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, sys->shm_fd, 0);
    if (sys->shm_mmap == MAP_FAILED)
    {
        msg_Err(vd, "cannot map buffers: %s", vlc_strerror_c(errno));
        goto error;
    }

    memset(sys->shm_mmap, 0x80, sys->shm_size); /* gray fill */

    sys->shm_pool = wl_shm_create_pool(sys->shm, sys->shm_fd, sys->shm_size);
    if (sys->shm_pool == NULL)
    {
        msg_Err(vd, "failed wl_shm_create_pool");
        goto error;
    }

    return VLC_SUCCESS;

error:
    shm_pool_close(sys);
    return VLC_EGENERIC;
}

static void
subpic_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct dmabuf_h * dh = data;

    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
    dmabuf_unref(&dh);
}

static const struct wl_buffer_listener subpic_buffer_listener = {
    .release = subpic_buffer_release,
};

static void
copy_xxxa_with_premul(void * dst_data, int dst_stride,
                      const void * src_data, int src_stride,
                      const unsigned int x, const unsigned int y,
                      const unsigned int w, const unsigned int h,
                      const unsigned int global_alpha)
{
    uint8_t * dst = (uint8_t*)dst_data + dst_stride * y + x * 4;
    const uint8_t * src = (uint8_t*)src_data + src_stride * y + x * 4;
    const int src_inc = src_stride - (int)w * 4;
    const int dst_inc = dst_stride - (int)w * 4;

    for (unsigned int i = 0; i != h; ++i)
    {
        for (unsigned int j = 0; j != w; ++j, src+=4, dst += 4)
        {
            unsigned int a = src[3] * global_alpha * 258;
            const unsigned int k = 0x800000;
            dst[0] = (src[0] * a + k) >> 24;
            dst[1] = (src[1] * a + k) >> 24;
            dst[2] = (src[2] * a + k) >> 24;
            dst[3] = (src[3] * global_alpha * 257 + 0x8000) >> 16;
        }
        src += src_inc;
        dst += dst_inc;
    }
}

static int
copy_subpic_to_w_buffer(vout_display_t *vd, vout_display_sys_t * const sys, picture_t * const src,
                        struct dmabuf_h ** pDmabuf_h, struct wl_buffer ** pW_buffer)
{
    unsigned int w = src->format.i_width;
    unsigned int h = src->format.i_height;
    size_t stride = src->p[0].i_pitch;
    size_t size = h * stride;
    struct dmabuf_h * dh = picpool_get(sys->subpic_pool, size);
    struct zwp_linux_buffer_params_v1 *params;
    const uint32_t drm_fmt = drmu_format_vlc_to_drm(&src->format);
    struct wl_buffer * w_buffer = NULL;

    fprintf(stderr, "%s: %.4s %dx%d, stride=%zd, surface=%p\n", __func__,
            (char*)&drm_fmt, w, h, stride, video_surface(sys));

    dmabuf_write_start(dh);
    copy_xxxa_with_premul(dmabuf_map(dh), stride, src->p[0].p_pixels, src->p[0].i_pitch,
                          0, 0, w, h, 0xff);
    dmabuf_write_end(dh);

    params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1_bind);
    if (!params)
    {
        msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
        goto error;
    }
    zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
    w_buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h, drm_fmt, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    *pW_buffer = w_buffer;
    *pDmabuf_h = dh;

    return VLC_SUCCESS;

error:
    dmabuf_unref(&dh);
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

struct dmabuf_w_env_s {
    picture_context_t * pic_ctx;
    vout_display_t * vd;
    int x, y;
    int w;
    int h;
};

static struct dmabuf_w_env_s *
dmabuf_w_env_new(vout_display_t * const vd, picture_context_t * pic_ctx)
{
    vout_display_sys_t * const sys = vd->sys;
    struct dmabuf_w_env_s * const dbe = calloc(1, sizeof(*dbe));
    if (!dbe)
        return NULL;
    dbe->pic_ctx = pic_ctx->copy(pic_ctx);
    dbe->vd = vd;
//    dbe->w = vd->cfg->display.width;
//    dbe->h = vd->cfg->display.height;
    msg_Dbg(vd, "%s: (%d,%d)", __func__, sys->x, sys->y);
    dbe->x = sys->x;
    dbe->y = sys->y;
    dbe->w = vd->fmt.i_visible_width;
    dbe->h = vd->fmt.i_visible_height;

    return dbe;
}

static void
dmabuf_w_env_delete(struct dmabuf_w_env_s * const dbe)
{
    msg_Dbg(dbe->vd, "%s", __func__);
    dbe->pic_ctx->destroy(dbe->pic_ctx);
    free(dbe);
}

static void
w_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct dmabuf_w_env_s * const dbe = data;
    msg_Dbg(dbe->vd, "%s", __func__);

    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
    dmabuf_w_env_delete(dbe);
}

static const struct wl_buffer_listener w_buffer_listener = {
    .release = w_buffer_release,
};


static struct wl_buffer*
do_display_dmabuf(vout_display_t * const vd, vout_display_sys_t * const sys, picture_t * const pic)
{
    struct zwp_linux_buffer_params_v1 *params;
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
    const uint32_t format = desc->layers[0].format;
    const unsigned int width = pic->format.i_visible_width;
    const unsigned int height = pic->format.i_visible_height;
    unsigned int n = 0;
    unsigned int flags = 0;
    int i;
    struct wl_buffer * w_buffer;
    struct wl_surface * const surface = video_surface(sys);

    msg_Dbg(vd, "<<< %s", __func__);

    /* Creation and configuration of planes  */
    params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1_bind);
    if (!params)
    {
        msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
        return NULL;
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
    w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, flags);

    zwp_linux_buffer_params_v1_destroy(params);

    wl_buffer_add_listener(w_buffer, &w_buffer_listener, dmabuf_w_env_new(vd, pic->context));

    // All offsetting seems bust right now
    wl_surface_attach(surface, w_buffer, 0, 0);

    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);

#if VIDEO_ON_SUBSURFACE
//    wl_surface_commit(sys->embed->handle.wl);
#endif

    return NULL;
}

static void
subpic_ent_flush(subpic_ent_t * const spe)
{
    if (spe->pic != NULL) {
        picture_Release(spe->pic);
        spe->pic = NULL;
    }
    if (spe->wb)
    {
        wl_buffer_destroy(spe->wb);
        spe->wb = NULL;
    }
    dmabuf_unref(&spe->dh);
}

static void Prepare(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t *sys = vd->sys;
    unsigned int n = 0;

    // Attempt to import the subpics
    for (subpicture_t * spic = subpic; spic != NULL; spic = spic->p_next)
    {
        for (subpicture_region_t *sreg = spic->p_region; sreg != NULL; sreg = sreg->p_next) {
            picture_t * const src = sreg->p_picture;
            subpic_ent_t * const dst = sys->subpics + n;

            // If the same picture then assume the same contents
            // We keep a ref to the previous pic to ensure that the same picture
            // structure doesn't get reused and confuse us.
            if (src != dst->pic) {
                subpic_ent_flush(dst);

                if (copy_subpic_to_w_buffer(vd, sys, src, &dst->dh, &dst->wb) != 0)
                    continue;


                dst->pic = picture_Hold(src);
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
}

static void Display(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t *sys = vd->sys;
    struct wl_display *display = sys->embed->display.wl;

    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subpic_ent_t * const spe = sys->subpics + i;

        if (!spe->update)
            continue;

        msg_Info(vd, "%s: Update subpic %i: wb=%p dh=%p", __func__, i, spe->wb, spe->dh);
        if (spe->wb != NULL)
            wl_buffer_add_listener(spe->wb, &subpic_buffer_listener, dmabuf_ref(spe->dh));
        wl_surface_attach(sys->subplanes[i].surface, spe->wb, 0, 0);

        wl_subsurface_set_position(sys->subplanes[i].subsurface, spe->dst_rect.x, spe->dst_rect.y);
        wp_viewport_set_source(sys->subplanes[i].viewport,
                               wl_fixed_from_int(spe->src_rect.x), wl_fixed_from_int(spe->src_rect.y),
                               wl_fixed_from_int(spe->src_rect.width), wl_fixed_from_int(spe->src_rect.height));
        wp_viewport_set_destination(sys->subplanes[i].viewport, spe->dst_rect.width, spe->dst_rect.height);
        wl_surface_damage(sys->subplanes[i].surface, 0, 0, INT32_MAX, INT32_MAX);

        wl_surface_commit(sys->subplanes[i].surface);
        spe->wb = NULL;
        spe->update = false;
    }

    do_display_dmabuf(vd, sys, pic);

    wl_display_roundtrip_queue(display, sys->eventq);

    if (subpic)
        subpicture_Delete(subpic);
    picture_Release(pic);
}

static void ResetPictures(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    kill_pool(sys);
}

static int Control(vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

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

            vout_display_place_t place;
#if !VIDEO_ON_SUBSURFACE
            vout_display_PlacePicture(&place, &sys->curr_aspect, vd->cfg, false);
            sys->x += place.width / 2;
            sys->y += place.height / 2;

            vout_display_PlacePicture(&sys->dst_rect, &vd->source, cfg, false);
            sys->x -= sys->dst_rect.width / 2;
            sys->y -= sys->dst_rect.height / 2;
#else
            vout_display_PlacePicture(&sys->dst_rect, &vd->source, cfg, true);
            wl_subsurface_set_position(sys->video_subsurface, sys->dst_rect.x, sys->dst_rect.y);
#endif

            place_spu_rect(vd, cfg, &vd->fmt);

            if (sys->viewport != NULL)
            {
                video_format_t fmt;

                video_format_ApplyRotation(&fmt, &vd->source);
                wp_viewport_set_source(sys->viewport,
                                wl_fixed_from_int(fmt.i_x_offset),
                                wl_fixed_from_int(fmt.i_y_offset),
                                wl_fixed_from_int(fmt.i_visible_width),
                                wl_fixed_from_int(fmt.i_visible_height));
                wp_viewport_set_destination(sys->viewport,
                                sys->dst_rect.width, sys->dst_rect.height);
            }
            else
                vout_display_SendEventPicturesInvalid(vd);

#if VIDEO_ON_SUBSURFACE
            if (sys->bkg_viewport != NULL)
            {
                msg_Info(vd, "Resize background: %dx%d", cfg->display.width, cfg->display.height);
                wp_viewport_set_destination(sys->bkg_viewport, cfg->display.width, cfg->display.height);
                wl_surface_commit(sys->embed->handle.wl);
            }
#endif

            sys->curr_aspect = vd->source;
            break;
        }
        default:
             msg_Err(vd, "unknown request %d", query);
             return VLC_EGENERIC;
    }
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


static void shm_format_cb(void *data, struct wl_shm *shm, uint32_t format)
{
    vout_display_t *vd = data;
    char str[4];

    memcpy(str, &format, sizeof (str));

    if (format >= 0x20202020)
        msg_Dbg(vd, "format %.4s (0x%08"PRIx32")", str, format);
    else
        msg_Dbg(vd, "format %4"PRIu32" (0x%08"PRIx32")", format, format);
    (void) shm;
}

static const struct wl_shm_listener shm_cbs =
{
    shm_format_cb,
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
    if (!strcmp(iface, "wl_shm")) {
        sys->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(sys->shm, &shm_cbs, vd);
    }
    else
    if (!strcmp(iface, "wp_viewporter"))
        sys->viewporter = wl_registry_bind(registry, name,
                                           &wp_viewporter_interface, 1);
    else
    if (!strcmp(iface, "wl_compositor"))
        sys->use_buffer_transform = vers >= 2;
    else
    if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        sys->linux_dmabuf_v1_bind = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(sys->linux_dmabuf_v1_bind, &linux_dmabuf_v1_listener, vd);
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
shm_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)data;
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_release,
};

static struct wl_buffer *
draw_frame(vout_display_sys_t *sys)
{
    const int width = 640, height = 480;
    int stride = width * 4;
    uint32_t *data = sys->shm_mmap;

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(sys->shm_pool, 0,
            width, height, stride, WL_SHM_FORMAT_XRGB8888);

    chequerboard(data, stride, width, height);

    wl_buffer_add_listener(buffer, &shm_buffer_listener, NULL);
    return buffer;
}

static void
mark_all_surface_opaque(struct wl_compositor * compositor, struct wl_surface * surface)
{
    struct wl_region * region_all = wl_compositor_create_region(compositor);
    wl_region_add(region_all, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface, region_all);
    wl_region_destroy(region_all);
}

static int Open(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t *sys;

    msg_Info(vd, "<<< %s: %.4s %dx%d, cfg.display: %dx%d", __func__,
             (const char*)&vd->fmt.i_chroma, vd->fmt.i_width, vd->fmt.i_height,
             vd->cfg->display.width, vd->cfg->display.height);

    if (!drmu_format_vlc_to_drm_prime(vd->fmt.i_chroma, NULL))
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    vd->sys = sys;
    shm_pool_init(sys);

    /* Get window */
    sys->embed = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_WAYLAND);
    if (sys->embed == NULL)
        goto error;

    struct wl_display *display = sys->embed->display.wl;

    sys->eventq = wl_display_create_queue(display);
    if (sys->eventq == NULL)
        goto error;

    struct wl_registry *registry = wl_display_get_registry(display);
    if (registry == NULL)
        goto error;

    wl_proxy_set_queue((struct wl_proxy *)registry, sys->eventq);
    wl_registry_add_listener(registry, &registry_cbs, vd);
    wl_display_roundtrip_queue(display, sys->eventq);
    wl_registry_destroy(registry);

    // And again - we registered some listeners in the call registry callback
    wl_display_roundtrip_queue(display, sys->eventq);

    fmt_list_sort(&sys->dmabuf_fmts);

    {
        static vlc_fourcc_t const tryfmts[] = {
            VLC_CODEC_RGBA,
            VLC_CODEC_BGRA,
        };
        unsigned int n = 0;

        if ((sys->subpic_chromas = calloc(ARRAY_SIZE(tryfmts) + 1, sizeof(vlc_fourcc_t))) == NULL)
            goto error;
        for (unsigned int i = 0; i != ARRAY_SIZE(tryfmts); ++i)
        {
            uint32_t drmfmt = drmu_format_vlc_chroma_to_drm(tryfmts[i]);
            msg_Dbg(vd, "Look for %.4s", (char*)&drmfmt);
            if (fmt_list_find(&sys->dmabuf_fmts, drmfmt, DRM_FORMAT_MOD_LINEAR) >= 0)
                sys->subpic_chromas[n++] = tryfmts[i];
        }

        if (n == 0)
            msg_Warn(vd, "No compatible subpic formats found");
    }

    // Create a backing store pool for subs etc.
    if (shm_pool_create(vd, sys, 0x1000000) != 0)
    {
        msg_Err(vd, "shm pool create failed");
        goto error;
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

    struct wl_surface *surface = sys->embed->handle.wl;

#if VIDEO_ON_SUBSURFACE
    // Make a new subsurface to use for video
    sys->video_surface = wl_compositor_create_surface(video_compositor(sys));
    sys->video_subsurface = wl_subcompositor_get_subsurface(sys->subcompositor, sys->video_surface, surface);
    wl_subsurface_place_above(sys->video_subsurface, surface);
    wl_subsurface_set_desync(sys->video_subsurface);  // Video update can be desync from main window
    surface = sys->video_surface;
#endif

    // Video is opaque
    mark_all_surface_opaque(video_compositor(sys), surface);

    if (sys->viewporter != NULL)
        sys->viewport = wp_viewporter_get_viewport(sys->viewporter, surface);
    else
        sys->viewport = NULL;

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

    if (sys->use_buffer_transform)
    {
        wl_surface_set_buffer_transform(surface,
                                        transforms[vd->fmt.orientation]);
    }
    else
    {
        video_format_t fmt = vd->fmt;
        video_format_ApplyRotation(&vd->fmt, &fmt);
    }

    {
        unsigned int i;
        struct wl_compositor * const compositor = video_compositor(sys);
        struct wl_surface * below = surface;

        if (compositor == NULL)
        {
            msg_Err(vd, "Can't get compositor");
            goto error;
        }

        for (i = 0; i != MAX_SUBPICS; ++i)
        {
            subplane_t *plane = sys->subplanes + i;
            plane->surface = wl_compositor_create_surface(compositor);
            plane->subsurface = wl_subcompositor_get_subsurface(sys->subcompositor, plane->surface, surface);
            wl_subsurface_place_above(plane->subsurface, below);
            below = plane->surface;
            wl_subsurface_set_sync(plane->subsurface);
            plane->viewport = wp_viewporter_get_viewport(sys->viewporter, plane->surface);
        }
    }

#if VIDEO_ON_SUBSURFACE
    // Build a background
    {
        unsigned int width = 640;
        unsigned int height = 480;
        unsigned int stride = 640 * 4;
        struct zwp_linux_buffer_params_v1 *params;
        struct wl_buffer * w_buffer;
        struct dmabuf_h *dh = picpool_get(sys->subpic_pool, stride * height);
        struct wl_surface *bkg_surface = sys->embed->handle.wl;

        dmabuf_write_start(dh);
        chequerboard(dmabuf_map(dh), stride, width, height);
        dmabuf_write_end(dh);

        params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1_bind);
        if (!params)
        {
            msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
            goto error;
        }
        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
        w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        wl_buffer_add_listener(w_buffer, &subpic_buffer_listener, dh);

        wl_surface_attach(bkg_surface, w_buffer, 0, 0);

        sys->bkg_viewport = wp_viewporter_get_viewport(sys->viewporter, bkg_surface);
        wp_viewport_set_destination(sys->bkg_viewport, vd->cfg->display.width, vd->cfg->display.height);
        mark_all_surface_opaque(video_compositor(sys), bkg_surface);
        wl_surface_commit(bkg_surface);
    }
#endif
    sys->curr_aspect = vd->source;

    vd->info.has_pictures_invalid = sys->viewport == NULL;
    vd->info.subpicture_chromas = sys->subpic_chromas;

    vd->pool = vd_dmabuf_pool;
    vd->prepare = Prepare;
    vd->display = Display;
    vd->control = Control;

    return VLC_SUCCESS;

error:
    if (sys->eventq != NULL)
        wl_event_queue_destroy(sys->eventq);
    if (sys->embed != NULL)
        vout_display_DeleteWindow(vd, sys->embed);
    free(sys);
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;

    ResetPictures(vd);
    shm_pool_close(sys);
    picpool_unref(&sys->subpic_pool);

    if (sys->viewport != NULL)
        wp_viewport_destroy(sys->viewport);
    if (sys->viewporter != NULL)
        wp_viewporter_destroy(sys->viewporter);
    wl_display_flush(sys->embed->display.wl);
    wl_event_queue_destroy(sys->eventq);
    vout_display_DeleteWindow(vd, sys->embed);

    free(sys->subpic_chromas);

    free(sys);
}

vlc_module_begin()
    set_shortname(N_("WL DMABUF"))
    set_description(N_("Wayland dmabuf video output"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 171)
    set_callbacks(Open, Close)
    add_shortcut("wl-dmabuf")
vlc_module_end()
