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
#ifndef HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
#define HAVE_WAYLAND_SINGLE_PIXEL_BUFFER 0
#endif

#include <assert.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#if HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
#include "single-pixel-buffer-v1-client-protocol.h"
#endif
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
#include "../drmu/drmu_log.h"
#include "../drmu/drmu_vlc_fmts.h"
#include "../drmu/pollqueue.h"
#include "../../codec/avcodec/drm_pic.h"
#include <libavutil/hwcontext_drm.h>

#define TRACE_ALL 0

#define MAX_PICTURES 4
#define MAX_SUBPICS  6

#define WL_DMABUF_DISABLE_NAME "wl-dmabuf-disable"
#define WL_DMABUF_DISABLE_TEXT N_("Disable wl-dmabuf")
#define WL_DMABUF_DISABLE_LONGTEXT N_("Disable wl-dmabuf - useful if auto selection is wanted but not wl-dmabuf")

#define WL_DMABUF_USE_SHM_NAME "wl-dmabuf-use-shm"
#define WL_DMABUF_USE_SHM_TEXT N_("Attempt to map via shm")
#define WL_DMABUF_USE_SHM_LONGTEXT N_("Attempt to map via shm rather than linux_dmabuf")

#define WL_DMABUF_CHEQUERBOARD_NAME "wl-dmabuf-chequerboard"
#define WL_DMABUF_CHEQUERBOARD_TEXT N_("Chequerboard background")
#define WL_DMABUF_CHEQUERBOARD_LONGTEXT N_("Fill unused window area with chequerboard rather than black")

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

typedef struct eq_env_ss {
    atomic_int eq_count;
    sem_t sem;

    struct wl_display *display;
    struct pollqueue *pq;
    struct wl_event_queue *q;
    struct wl_display *wrapped_display;
} eq_env_t;

typedef struct video_dmabuf_release_env_ss
{
    void (* dma_rel_fn)(void *);
    void * dma_rel_v;
    eq_env_t * eq;
    unsigned int rel_count;
    unsigned int pt_count;
    struct polltask * pt[AV_DRM_MAX_PLANES];
} video_dmabuf_release_env_t;

typedef struct subpic_ent_s {
    struct wl_buffer * wb;
    struct dmabuf_h * dh;
    video_dmabuf_release_env_t * vdre;
    picture_t * pic;
    int alpha;
    enum wl_output_transform trans;
    vout_display_place_t src_rect;
    vout_display_place_t dst_rect;

    atomic_int ready;

    struct polltask * pt;
    vout_display_t * vd;
    vout_display_sys_t * sys;
} subpic_ent_t;

typedef struct subplane_s {
    struct wl_surface * surface;
    struct wl_subsurface * subsurface;
    struct wp_viewport * viewport;

    enum wl_output_transform trans;
    vout_display_place_t src_rect;
    vout_display_place_t dst_rect;

    subpic_ent_t * spe_cur;
    subpic_ent_t * spe_next;
} subplane_t;

typedef struct w_bound_ss
{
    struct wp_viewporter *viewporter;
    struct zwp_linux_dmabuf_v1 * linux_dmabuf_v1;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
#if HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
    struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager_v1;
#endif
} w_bound_t;

#define COMMIT_BKG 0
#define COMMIT_VID 1
#define COMMIT_SUB 2

struct vout_display_sys_t
{
    vout_window_t *embed; /* VLC window */

    w_bound_t bound;

    picture_pool_t *vlc_pic_pool; /* picture pool */

    struct wl_surface * last_embed_surface;
    unsigned int last_embed_seq;

    int x;
    int y;
    bool video_attached;
    bool use_shm;
    bool chequerboard;

    struct wp_viewport * bkg_viewport;
    // Current size of background viewport if we have one
    // If not created yet then the size that the viewport should be created
    unsigned int bkg_w;
    unsigned int bkg_h;

    eq_env_t * eq;

    struct pollqueue * pollq;
    struct pollqueue * speq;

    picpool_ctl_t * subpic_pool;
    subplane_t video_plane[1];
    subplane_t subplanes[MAX_SUBPICS];
    bool commit_req[MAX_SUBPICS + 2];
    subpic_ent_t video_spe;
    vlc_fourcc_t * subpic_chromas;

    struct wl_region * region_none;
    struct wl_region * region_all;

    fmt_list_t dmabuf_fmts;
    fmt_list_t shm_fmts;
};


static struct wl_surface * bkg_surface_get_lock(vout_display_t * const vd, vout_display_sys_t * const sys);
static void bkg_surface_unlock(vout_display_t * const vd, vout_display_sys_t * const sys);


static inline struct wl_display *
video_display(const vout_display_sys_t * const sys)
{
    return sys->embed->display.wl;
}

static inline struct wl_surface *
video_surface(const vout_display_sys_t * const sys)
{
    return sys->video_plane->surface;
}

static inline struct wl_compositor *
video_compositor(const vout_display_sys_t * const sys)
{
    return sys->bound.compositor;
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
region_destroy(struct wl_region ** const ppregion)
{
    if (*ppregion == NULL)
        return;
    wl_region_destroy(*ppregion);
    *ppregion = NULL;
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

static inline bool
place_xy_eq(const vout_display_place_t a, const vout_display_place_t b)
{
    return a.x == b.x && a.y == b.y;
}

static inline bool
place_wh_eq(const vout_display_place_t a, const vout_display_place_t b)
{
    return a.width == b.width && a.height == b.height;
}

static inline bool
place_eq(const vout_display_place_t a, const vout_display_place_t b)
{
    return place_xy_eq(a, b) && place_wh_eq(a, b);
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
fmt_list_find(const fmt_list_t * const fl, const drmu_vlc_fmt_info_t * const fmti)
{
    if (fmti == NULL || fl->len == 0)
    {
        return -1;
    }
    else
    {
        const fmt_ent_t x = {
            .fmt = drmu_vlc_fmt_info_drm_pixelformat(fmti),
            .mod = drmu_vlc_fmt_info_drm_modifier(fmti),
        };
        const fmt_ent_t * const fe =
            bsearch(&x, fl->fmts, fl->len, sizeof(x), fmt_sort_cb);
        return fe == NULL ? -1 : fe->pri;
    }
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

// ----------------------------------------------------------------------------

static struct wl_display *
eq_wrapper(eq_env_t * const eq)
{
    return eq->wrapped_display;
}

static void
eq_ref(eq_env_t * const eq)
{
    const int n = atomic_fetch_add(&eq->eq_count, 1);
    (void)n;
//    fprintf(stderr, "Ref: count=%d\n", n + 1);
}

static void
eq_unref(eq_env_t ** const ppeq)
{
    eq_env_t * eq = *ppeq;
    if (eq != NULL)
    {
        int n;
        *ppeq = NULL;
        n = atomic_fetch_sub(&eq->eq_count, 1);
//        fprintf(stderr, "Unref: Buffer count=%d\n", n);
        if (n == 0)
        {
            pollqueue_set_pre_post(eq->pq, 0, 0, NULL);
            pollqueue_unref(&eq->pq);

            wl_proxy_wrapper_destroy(eq->wrapped_display);
            wl_event_queue_destroy(eq->q);

            sem_destroy(&eq->sem);
            free(eq);
//            fprintf(stderr, "Eq closed\n");
        }
    }
}

static int
eq_finish(eq_env_t ** const ppeq)
{
    eq_env_t * const eq = *ppeq;

    if (eq == NULL)
        return 0;

    eq_unref(ppeq);
    return 0;
}

static void
pollq_pre_cb(void * v, struct pollfd * pfd)
{
    eq_env_t * const eq = v;
    struct wl_display *const display = eq->display;
    int ferr;
    int frv;

//    fprintf(stderr, "Start Prepare\n");

    while (wl_display_prepare_read_queue(display, eq->q) != 0) {
        int n = wl_display_dispatch_queue_pending(display, eq->q);
        (void)n;
//        fprintf(stderr, "Dispatch=%d\n", n);
    }
    if ((frv = wl_display_flush(display)) >= 0) {
        pfd->events = POLLIN;
        ferr = 0;
    }
    else {
        ferr = errno;
        pfd->events = POLLOUT | POLLIN;
    }
    pfd->fd = wl_display_get_fd(display);
(void)ferr;
//    fprintf(stderr, "Done Prepare: fd=%d, evts=%#x, frv=%d, ferr=%s\n", pfd->fd, pfd->events, frv, ferr == 0 ? "ok" : strerror(ferr));
}

static void
pollq_post_cb(void *v, short revents)
{
    eq_env_t * const eq = v;
    struct wl_display *const display = eq->display;
    int n;

    if ((revents & POLLIN) == 0) {
//        fprintf(stderr, "Cancel read: Events=%#x: IN=%#x, OUT=%#x, ERR=%#x\n", (int)revents, POLLIN, POLLOUT, POLLERR);
        wl_display_cancel_read(display);
    }
    else {
//        fprintf(stderr, "Read events: Events=%#x: IN=%#x, OUT=%#x, ERR=%#x\n", (int)revents, POLLIN, POLLOUT, POLLERR);
        wl_display_read_events(display);
    }

//    fprintf(stderr, "Start Dispatch\n");
    n = wl_display_dispatch_queue_pending(display, eq->q);
    (void)n;
//    fprintf(stderr, "Dispatch=%d\n", n);
}

static eq_env_t *
eq_new(struct wl_display * const display, struct pollqueue * const pq)
{
    eq_env_t * eq = calloc(1, sizeof(*eq));

    if (eq == NULL)
        return NULL;

    atomic_init(&eq->eq_count, 0);
    sem_init(&eq->sem, 0, 0);

    if ((eq->q = wl_display_create_queue(display)) == NULL)
        goto err1;
    if ((eq->wrapped_display = wl_proxy_create_wrapper(display)) == NULL)
        goto err2;
    wl_proxy_set_queue((struct wl_proxy *)eq->wrapped_display, eq->q);

    eq->display = display;
    eq->pq = pollqueue_ref(pq);

    pollqueue_set_pre_post(eq->pq, pollq_pre_cb, pollq_post_cb, eq);

    return eq;

err2:
    wl_event_queue_destroy(eq->q);
err1:
    free(eq);
    return NULL;
}

static void eventq_sync_cb(void * data, struct wl_callback * cb, unsigned int cb_data)
{
    sem_t * const sem = data;
    VLC_UNUSED(cb_data);
    wl_callback_destroy(cb);
    sem_post(sem);
}

static const struct wl_callback_listener eq_sync_listener = {.done = eventq_sync_cb};

struct eq_sync_env_ss {
    eq_env_t * eq;
    sem_t sem;
};

static void
eq_sync_pq_cb(void * v, short revents)
{
    struct eq_sync_env_ss * const eqs = v;
    struct wl_callback * const cb = wl_display_sync(eq_wrapper(eqs->eq));
    VLC_UNUSED(revents);
    wl_callback_add_listener(cb, &eq_sync_listener, &eqs->sem);
    // No flush needed as that will occur as part of the pollqueue loop
}

static int
eventq_sync(eq_env_t * const eq)
{
    struct eq_sync_env_ss eqs = {.eq = eq};
    int rv;

    if (!eq)
        return -1;

    sem_init(&eqs.sem, 0, 0);
    // Bounce execution to pollqueue to avoid race setting up listener
    pollqueue_callback_once(eq->pq, eq_sync_pq_cb, &eqs);
    while ((rv = sem_wait(&eqs.sem)) == -1 && errno == EINTR)
        /* Loop */;
    sem_destroy(&eqs.sem);
    return rv;
}

// ----------------------------------------------------------------------------

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
fill_uniform(uint32_t *const data, unsigned int stride, const unsigned int width, const unsigned int height, const uint32_t val)
{
    stride /= sizeof(uint32_t);

    /* Draw solid background */
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x)
            data[y * stride + x] = val;
    }
}

// ----------------------------------------------------------------------------

static void
vdre_free(video_dmabuf_release_env_t * const vdre)
{
    unsigned int i;
    if (vdre->dma_rel_fn)
        vdre->dma_rel_fn(vdre->dma_rel_v);
    for (i = 0; i != vdre->pt_count; ++i)
        polltask_delete(vdre->pt + i);
    eq_unref(&vdre->eq);
    free(vdre);
}

static video_dmabuf_release_env_t *
vdre_new_null(void)
{
    video_dmabuf_release_env_t * const vdre = calloc(1, sizeof(*vdre));
    return vdre;
}

static void
vdre_dma_rel_cb(void * v)
{
    struct picture_context_t * ctx = v;
    ctx->destroy(ctx);
}

static video_dmabuf_release_env_t *
vdre_new_ctx(struct picture_context_t * ctx)
{
    video_dmabuf_release_env_t * const vdre = vdre_new_null();
    if (vdre == NULL)
        return NULL;
    if ((vdre->dma_rel_v = ctx->copy(ctx)) == NULL)
    {
        free(vdre);
        return NULL;
    }
    vdre->dma_rel_fn = vdre_dma_rel_cb;
    return vdre;
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
vdre_eq_ref(video_dmabuf_release_env_t * const vdre, eq_env_t * const eq)
{
    if (vdre == NULL)
        return;
    vdre->eq = eq;
    eq_ref(vdre->eq);
}

static void
vdre_add_pt(video_dmabuf_release_env_t * const vdre, struct pollqueue * pq, int fd)
{
    assert(vdre->pt_count < AV_DRM_MAX_PLANES);
    vdre->pt[vdre->pt_count++] = polltask_new(pq, fd, POLLOUT, w_ctx_release, vdre);
}

static void
vdre_dh_rel_cb(void * v)
{
    struct dmabuf_h * dh = v;
    dmabuf_unref(&dh);
}

static video_dmabuf_release_env_t *
vdre_new_dh(struct dmabuf_h *const dh, struct pollqueue *const pq)
{
    video_dmabuf_release_env_t * const vdre = vdre_new_null();

    vdre->dma_rel_fn = vdre_dh_rel_cb;
    vdre->dma_rel_v = dh;

    if (!dmabuf_is_fake(dh))
        vdre_add_pt(vdre, pq, dmabuf_fd(dh));
    return vdre;
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

    eq_unref(&vdre->eq);

    if (i == 0)
        vdre_free(vdre);
    else
    {
        // Whilst we can happily destroy the buffer that doesn't mean we can reuse
        // the dmabufs yet - we have to wait for them to be free of fences.
        // We don't want to wait in this callback so do the waiting in pollqueue
        while (i-- != 0)
            pollqueue_add_task(vdre->pt[i], 1000);
    }
}

static const struct wl_buffer_listener w_buffer_listener = {
    .release = w_buffer_release,
};

// ----------------------------------------------------------------------------

static inline size_t cpypic_plane_alloc_size(const plane_t * const p)
{
    return p->i_pitch * p->i_lines;
}

static inline uint32_t
drm_fmt_to_wl_shm(const uint32_t drm_fmt)
{
    return (drm_fmt == DRM_FORMAT_ARGB8888) ? WL_SHM_FORMAT_ARGB8888 :
           (drm_fmt == DRM_FORMAT_XRGB8888) ? WL_SHM_FORMAT_XRGB8888 : drm_fmt;
}

static int
copy_subpic_to_w_buffer(vout_display_t *vd, vout_display_sys_t * const sys, picture_t * const src,
                        int alpha,
                        video_dmabuf_release_env_t ** pVdre, struct wl_buffer ** pW_buffer)
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
    *pVdre = NULL;

    if ((dh = picpool_get(sys->subpic_pool, total_size)) == NULL)
    {
        msg_Warn(vd, "Failed to alloc dmabuf for subpic");
        goto error;
    }
    if ((*pVdre = vdre_new_dh(dh, sys->pollq)) == NULL)
    {
        msg_Warn(vd, "Failed to alloc vdre for subpic");
        dmabuf_unref(&dh);
        goto error;
    }

    if (dmabuf_is_fake(dh) || !sys->bound.linux_dmabuf_v1)
    {
        struct wl_shm_pool *pool = wl_shm_create_pool(sys->bound.shm, dmabuf_fd(dh), dmabuf_size(dh));
        const uint32_t w_fmt = drm_fmt_to_wl_shm(drm_fmt);
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
        if ((params = zwp_linux_dmabuf_v1_create_params(sys->bound.linux_dmabuf_v1)) == NULL)
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
    wl_buffer_add_listener(*pW_buffer, &w_buffer_listener, *pVdre);

    return VLC_SUCCESS;

error:
    if (params)
        zwp_linux_buffer_params_v1_destroy(params);
    vdre_delete(pVdre);
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

static int
do_display_dmabuf(vout_display_t * const vd, vout_display_sys_t * const sys, picture_t * const pic,
                  video_dmabuf_release_env_t ** const pVdre, struct wl_buffer ** const pWbuffer)
{
    struct zwp_linux_buffer_params_v1 *params = NULL;
    const AVDRMFrameDescriptor * const desc = drm_prime_get_desc(pic);
    const uint32_t format = desc->layers[0].format;
    const unsigned int width = pic->format.i_width;
    const unsigned int height = pic->format.i_height;
    unsigned int n = 0;
    unsigned int flags = 0;
    int i;
    struct wl_buffer * w_buffer;
    video_dmabuf_release_env_t * const vdre = vdre_new_ctx(pic->context);

    assert(*pWbuffer == NULL);
    assert(*pVdre == NULL);

    if (vdre == NULL) {
        msg_Err(vd, "Failed to create vdre");
        return VLC_ENOMEM;
    }

    for (i = 0; i != desc->nb_objects; ++i)
        vdre_add_pt(vdre, sys->pollq, desc->objects[i].fd);

    if (!sys->bound.linux_dmabuf_v1)
    {
        const AVDRMPlaneDescriptor *const p = desc->layers[0].planes + 0;
        struct wl_shm_pool *pool = wl_shm_create_pool(sys->bound.shm, desc->objects[0].fd, desc->objects[0].size);
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
        params = zwp_linux_dmabuf_v1_create_params(sys->bound.linux_dmabuf_v1);
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
    vdre_delete(&spe->vdre);
    dmabuf_unref(&spe->dh);
}

static bool
subpic_ent_attach(struct wl_surface * const surface, subpic_ent_t * const spe, eq_env_t * eq)
{
    const bool has_pic = (spe->wb != NULL);
    vdre_eq_ref(spe->vdre, eq);
    wl_surface_attach(surface, spe->wb, 0, 0);
    spe->vdre = NULL;
    spe->wb = NULL;
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    return has_pic;
}

static void
spe_convert_cb(void * v, short revents)
{
    subpic_ent_t * const spe = v;
    VLC_UNUSED(revents);

    copy_subpic_to_w_buffer(spe->vd, spe->sys, spe->pic, spe->alpha, &spe->vdre, &spe->wb);
    atomic_store(&spe->ready, 1);
}

static inline bool
spe_no_pic(const subpic_ent_t * const spe)
{
    return spe == NULL || spe->pic == NULL;
}

static bool
spe_changed(const subpic_ent_t * const spe, const subpicture_region_t * const sreg)
{
    const bool no_pic = (sreg == NULL || sreg->i_alpha == 0);
    if (no_pic && spe_no_pic(spe))
        return false;
    return no_pic || spe_no_pic(spe) || spe->pic != sreg->p_picture || spe->alpha != sreg->i_alpha;
}

static void
spe_update_rect(subpic_ent_t * const spe, vout_display_sys_t * const sys,
                const subpicture_t * const spic,
                const subpicture_region_t * const sreg)
{
    spe->src_rect = (vout_display_place_t) {
        .x = sreg->fmt.i_x_offset,
        .y = sreg->fmt.i_y_offset,
        .width = sreg->fmt.i_visible_width,
        .height = sreg->fmt.i_visible_height,
    };
    spe->dst_rect = place_rescale(
        (vout_display_place_t) {
            .x = sreg->i_x,
            .y = sreg->i_y,
            .width = sreg->fmt.i_visible_width,
            .height = sreg->fmt.i_visible_height,
        },
        (vout_display_place_t) {
            .x = 0,
            .y = 0,
            .width  = sys->video_spe.dst_rect.width,
            .height = sys->video_spe.dst_rect.height,
        },
        (vout_display_place_t) {
            .x = 0,
            .y = 0,
            .width  = spic->i_original_picture_width,
            .height = spic->i_original_picture_height,
        });
}

static subpic_ent_t *
spe_new(vout_display_t * const vd, vout_display_sys_t * const sys,
        const subpicture_t * const spic,
        const subpicture_region_t * const sreg)
{
    subpic_ent_t * const spe = calloc(1, sizeof(*spe));

    if (spe == NULL)
        return NULL;

    atomic_init(&spe->ready, 0);
    spe->vd = vd;
    spe->sys = sys;

    if (sreg == NULL || sreg->i_alpha == 0)
    {
        atomic_init(&spe->ready, 1);
        return spe;
    }

    spe->pic = picture_Hold(sreg->p_picture);
    spe->alpha = sreg->i_alpha;

    spe_update_rect(spe, sys, spic, sreg);

    spe->pt = polltask_new_timer(sys->speq, spe_convert_cb, spe);

    return spe;
}

static void
spe_delete(subpic_ent_t ** const ppspe)
{
    subpic_ent_t * const spe = *ppspe;

    if (spe == NULL)
        return;
    *ppspe = NULL;

    polltask_delete(&spe->pt);
    subpic_ent_flush(spe);
    free(spe);
}

static int
spe_convert(subpic_ent_t * const spe)
{
    if (spe->pt != NULL)
        pollqueue_add_task(spe->pt, 0);
    return 0;
}

static void
commit_req(vout_display_sys_t * const sys, const unsigned int layer)
{
    sys->commit_req[layer] = true;
}

static void
commit_do(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    int i;
    bool flush_req = false;

    for (i = MAX_SUBPICS - 1; i >= 0; --i)
    {
        if (sys->commit_req[i + COMMIT_SUB])
        {
            sys->commit_req[i + COMMIT_SUB] = false;
            wl_surface_commit(sys->subplanes[i].surface);
            flush_req = true;
        }
    }
    if (sys->commit_req[COMMIT_VID])
    {
        sys->commit_req[COMMIT_VID] = false;
        wl_surface_commit(video_surface(sys));
        flush_req = true;
    }
    if (sys->commit_req[COMMIT_BKG])
    {
        struct wl_surface * const bkg_surface = bkg_surface_get_lock(vd, sys);
        if (bkg_surface != NULL)
        {
            wp_viewport_set_destination(sys->bkg_viewport, sys->bkg_w, sys->bkg_h);
            wl_surface_commit(bkg_surface);
            bkg_surface_unlock(vd, sys);
            flush_req = true;
        }
        sys->commit_req[COMMIT_BKG] = false;
    }
    if (flush_req)
        wl_display_flush(video_display(sys));
}

static void
clear_surface_buffer(struct wl_surface * surface)
{
    if (surface == NULL)
        return;
    wl_surface_attach(surface, NULL, 0, 0);
    wl_surface_commit(surface);
}

static void
clear_all_buffers(vout_display_sys_t * const sys, const bool bkg_valid)
{
    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subplane_t *const plane = sys->subplanes + i;
        spe_delete(&plane->spe_next);
        spe_delete(&plane->spe_cur);
        clear_surface_buffer(plane->surface);
    }

    clear_surface_buffer(video_surface(sys));
    sys->video_attached = false;

    if (bkg_valid)
        clear_surface_buffer(sys->last_embed_surface);

    subpic_ent_flush(&sys->video_spe);
}

static void
plane_destroy(subplane_t * const spl)
{
    viewport_destroy(&spl->viewport);
    subsurface_destroy(&spl->subsurface);
    surface_destroy(&spl->surface);
    // Zap all tracking vars
    spl->trans = 0;
    memset(&spl->src_rect, 0, sizeof(spl->src_rect));
    memset(&spl->dst_rect, 0, sizeof(spl->dst_rect));
}

static int
plane_create(vout_display_sys_t * const sys, subplane_t * const plane,
             struct wl_surface * const parent,
             struct wl_surface * const above,
             const bool sync)
{
    if ((plane->surface = wl_compositor_create_surface(video_compositor(sys))) == NULL ||
        (plane->subsurface = wl_subcompositor_get_subsurface(sys->bound.subcompositor, plane->surface, parent)) == NULL ||
        (plane->viewport = wp_viewporter_get_viewport(sys->bound.viewporter, plane->surface)) == NULL)
        return VLC_EGENERIC;
    wl_subsurface_place_above(plane->subsurface, above);
    if (sync)
        wl_subsurface_set_sync(plane->subsurface);
    else
        wl_subsurface_set_desync(plane->subsurface);
    wl_surface_set_input_region(plane->surface, sys->region_none);
    return 0;
}

static void
unmap_all(vout_display_sys_t * const sys, const bool bkg_valid)
{
    clear_all_buffers(sys, bkg_valid);

    // Free subpic resources
    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
        plane_destroy(sys->subplanes + i);

    plane_destroy(sys->video_plane);

    viewport_destroy(&sys->bkg_viewport);
}

static struct wl_surface *
bkg_surface_get_lock(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    if (!sys->embed) {
        msg_Err(vd, "%s: Embed NULL", __func__);
        return NULL;
    }

    vlc_mutex_lock(&sys->embed->handle_lock);

    if (sys->embed->handle.wl != sys->last_embed_surface || sys->embed->handle_seq != sys->last_embed_seq)
    {
        msg_Warn(vd, "%s: Embed surface changed %p (%u)->%p (%u)", __func__,
                 sys->last_embed_surface, sys->last_embed_seq,
                 sys->embed->handle.wl, sys->embed->handle_seq);

        sys->last_embed_surface = sys->embed->handle.wl;
        sys->last_embed_seq = sys->embed->handle_seq;
        unmap_all(sys, false);
    }

    if (sys->last_embed_surface == NULL)
        vlc_mutex_unlock(&sys->embed->handle_lock);

    return sys->last_embed_surface;
}

static void
bkg_surface_unlock(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    VLC_UNUSED(vd);
    vlc_mutex_unlock(&sys->embed->handle_lock);
}

static int
make_subpic_surfaces(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    unsigned int i;
    struct wl_surface * const surface = video_surface(sys);
    struct wl_surface * below = surface;
    int rv;

    if (sys->subplanes[0].surface)
        return VLC_SUCCESS;

    for (i = 0; i != MAX_SUBPICS; ++i)
    {
        subplane_t * const plane = sys->subplanes + i;
        if ((rv = plane_create(sys, plane, surface, below, true)) != 0)
        {
            msg_Err(vd, "%s: Failed to create subpic plane %d", __func__, i);
            return rv;
        }
        below = plane->surface;
    }
    return VLC_SUCCESS;
}

static int
make_background_and_video(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    // Build a background
    // Use single_pixel_surface extension if we have it & want a simple
    // single colour (black) patch
    struct dmabuf_h * dh = NULL;
    video_dmabuf_release_env_t * vdre = NULL;
    struct wl_buffer * w_buffer = NULL;
    struct wl_surface * bkg_surface = NULL;

    if (sys->bkg_viewport)
        return VLC_SUCCESS;

#if HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
    if (sys->bound.single_pixel_buffer_manager_v1 && !sys->chequerboard)
    {
        w_buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            sys->bound.single_pixel_buffer_manager_v1,
            0, 0, 0, UINT32_MAX);  // R, G, B, A
        vdre = vdre_new_null();
    }
    else
#endif
    {
        // Buffer width & height - not display
        const unsigned int width = sys->chequerboard ? 640 : 32;
        const unsigned int height = sys->chequerboard ? 480 : 32;
        const unsigned int stride = width * 4;

        if ((dh = picpool_get(sys->subpic_pool, stride * height)) == NULL)
        {
            msg_Err(vd, "Failed to get DmaBuf for background");
            goto error;
        }

        dmabuf_write_start(dh);
        if (sys->chequerboard)
            chequerboard(dmabuf_map(dh), stride, width, height);
        else
            fill_uniform(dmabuf_map(dh), stride, width, height, 0xff000000);
        dmabuf_write_end(dh);

        if (sys->use_shm)
        {
            struct wl_shm_pool *pool = wl_shm_create_pool(sys->bound.shm, dmabuf_fd(dh), dmabuf_size(dh));
            if (pool == NULL)
            {
                msg_Err(vd, "Failed to create pool from dmabuf");
                goto error;
            }
            w_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
        }
        else
        {
            struct zwp_linux_buffer_params_v1 *params;
            params = zwp_linux_dmabuf_v1_create_params(sys->bound.linux_dmabuf_v1);
            if (!params) {
                msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
                goto error;
            }
            zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
            w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_XRGB8888, 0);
            zwp_linux_buffer_params_v1_destroy(params);
        }

        vdre = vdre_new_dh(dh, sys->pollq);
        dh = NULL;
    }
    if (!w_buffer || !vdre)
    {
        msg_Err(vd, "Failed to create background buffer");
        goto error;
    }

    if ((bkg_surface = bkg_surface_get_lock(vd, sys)) == NULL)
        goto error;

    sys->bkg_viewport = wp_viewporter_get_viewport(sys->bound.viewporter, bkg_surface);
    if (sys->bkg_viewport == NULL)
    {
        msg_Err(vd, "Failed to create background viewport");
        goto err_unlock;
    }

    vdre_eq_ref(vdre, sys->eq);
    wl_buffer_add_listener(w_buffer, &w_buffer_listener, vdre);
    wl_surface_attach(bkg_surface, w_buffer, 0, 0);
    vdre = NULL;
    w_buffer = NULL;

    wp_viewport_set_destination(sys->bkg_viewport, sys->bkg_w, sys->bkg_h);
    wl_surface_set_opaque_region(bkg_surface, sys->region_all);

    wl_surface_damage(bkg_surface, 0, 0, INT32_MAX, INT32_MAX);

    if (plane_create(sys, sys->video_plane, bkg_surface, bkg_surface, false) != 0)
    {
        msg_Err(vd, "Failed to create video plane");
        goto err_unlock;
    }

    wl_surface_set_opaque_region(sys->video_plane->surface, sys->region_all);

    commit_req(sys, COMMIT_BKG);

    bkg_surface_unlock(vd, sys);

    return VLC_SUCCESS;

err_unlock:
    bkg_surface_unlock(vd, sys);
error:
    buffer_destroy(&w_buffer);
    vdre_delete(&vdre);
    dmabuf_unref(&dh);
    return VLC_ENOMEM;
}

// Get tranform & adjusted source coords for orientation
static enum wl_output_transform
transform_from_fmt(const video_format_t * const fmt, vout_display_place_t * const s)
{
    const int rx_offset = fmt->i_width - (fmt->i_visible_width + fmt->i_x_offset);
    const int by_offset = fmt->i_height - (fmt->i_visible_height + fmt->i_y_offset);

    switch (fmt->orientation)
    {
        case ORIENT_ROTATED_90:  // ORIENT_RIGHT_TOP,
            *s = (vout_display_place_t){
                .x      = by_offset,
                .y      = fmt->i_x_offset,
                .width  = fmt->i_visible_height,
                .height = fmt->i_visible_width};
            return WL_OUTPUT_TRANSFORM_90;

        case ORIENT_ROTATED_180: // ORIENT_BOTTOM_RIGHT,
            *s = (vout_display_place_t){
                .x      = by_offset,
                .y      = rx_offset,
                .width  = fmt->i_visible_width,
                .height = fmt->i_visible_height};
            return WL_OUTPUT_TRANSFORM_180;

        case ORIENT_ROTATED_270: // ORIENT_LEFT_BOTTOM,
            *s = (vout_display_place_t){
                .x      = fmt->i_y_offset,
                .y      = rx_offset,
                .width  = fmt->i_visible_height,
                .height = fmt->i_visible_width};
            return WL_OUTPUT_TRANSFORM_270;

        case ORIENT_HFLIPPED:    // ORIENT_TOP_RIGHT,
            *s = (vout_display_place_t){
                .x      = rx_offset,
                .y      = fmt->i_y_offset,
                .width  = fmt->i_visible_width,
                .height = fmt->i_visible_height};
            return WL_OUTPUT_TRANSFORM_FLIPPED;

        case ORIENT_VFLIPPED:    // ORIENT_BOTTOM_LEFT,
            *s = (vout_display_place_t){
                .x      = fmt->i_x_offset,
                .y      = by_offset,
                .width  = fmt->i_visible_width,
                .height = fmt->i_visible_height};
            return WL_OUTPUT_TRANSFORM_FLIPPED_180;

        case ORIENT_TRANSPOSED:  // ORIENT_LEFT_TOP,
            *s = (vout_display_place_t){
                .x      = fmt->i_y_offset,
                .y      = fmt->i_x_offset,
                .width  = fmt->i_visible_height,
                .height = fmt->i_visible_width};
            return WL_OUTPUT_TRANSFORM_FLIPPED_90;

        case ORIENT_ANTI_TRANSPOSED: // ORIENT_RIGHT_BOTTOM,
            *s = (vout_display_place_t){
                .x      = rx_offset,
                .y      = by_offset,
                .width  = fmt->i_visible_height,
                .height = fmt->i_visible_width};
            return WL_OUTPUT_TRANSFORM_FLIPPED_270;

        case ORIENT_NORMAL:      // ORIENT_TOP_LEFT,
        default:
            *s = (vout_display_place_t){
                .x      = fmt->i_x_offset,
                .y      = fmt->i_y_offset,
                .width  = fmt->i_visible_width,
                .height = fmt->i_visible_height};
            return WL_OUTPUT_TRANSFORM_NORMAL;
    }
}

static void
place_rects(vout_display_t * const vd,
          const vout_display_cfg_t * const cfg)
{
    vout_display_sys_t * const sys = vd->sys;

    vout_display_PlacePicture(&sys->video_spe.dst_rect, &vd->source, cfg, true);
    sys->video_spe.trans = transform_from_fmt(&vd->fmt, &sys->video_spe.src_rect);
}

static void
plane_set_rect(vout_display_sys_t * const sys, subplane_t * const plane, const subpic_ent_t * const spe,
               const unsigned int commit_this, const unsigned int commit_parent)
{
    if (spe->trans != plane->trans)
    {
        wl_surface_set_buffer_transform(plane->surface, spe->trans);
        commit_req(sys, commit_this);
    }
    if (!place_eq(spe->src_rect, plane->src_rect))
    {
        wp_viewport_set_source(plane->viewport,
                               wl_fixed_from_int(spe->src_rect.x), wl_fixed_from_int(spe->src_rect.y),
                               wl_fixed_from_int(spe->src_rect.width), wl_fixed_from_int(spe->src_rect.height));
        commit_req(sys, commit_this);
    }
    if (!place_xy_eq(spe->dst_rect, plane->dst_rect))
    {
        wl_subsurface_set_position(plane->subsurface, spe->dst_rect.x, spe->dst_rect.y);
        commit_req(sys, commit_this);
    }
    if (!place_wh_eq(spe->dst_rect, plane->dst_rect))
    {
        wp_viewport_set_destination(plane->viewport, spe->dst_rect.width, spe->dst_rect.height);
        commit_req(sys, commit_parent); // Subsurface pos needs parent commit (video)
    }

    plane->trans = spe->trans;
    plane->src_rect = spe->src_rect;
    plane->dst_rect = spe->dst_rect;
}

static void
set_video_viewport(vout_display_sys_t * const sys)
{
    if (!sys->video_attached)
        return;

    plane_set_rect(sys, sys->video_plane, &sys->video_spe, COMMIT_VID, COMMIT_BKG);
}

static void Prepare(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t * const sys = vd->sys;
    unsigned int n = 0;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: Surface: %p", __func__, sys->embed->handle.wl);
#endif

    subpic_ent_flush(&sys->video_spe); // If somehow we have a buffer here - avoid leaking
    if (drmu_format_vlc_to_drm_prime(&pic->format, NULL) == 0)
        copy_subpic_to_w_buffer(vd, sys, pic, 0xff, &sys->video_spe.vdre, &sys->video_spe.wb);
    else
        do_display_dmabuf(vd, sys, pic, &sys->video_spe.vdre, &sys->video_spe.wb);
    wl_display_flush(video_display(sys)); // Kick off any work required by Wayland

    // Attempt to import the subpics
    for (const subpicture_t * spic = subpic; spic != NULL; spic = spic->p_next)
    {
        for (const subpicture_region_t *sreg = spic->p_region; sreg != NULL; sreg = sreg->p_next)
        {
            subplane_t * const plane = sys->subplanes + n;

            if (plane->spe_next != NULL)
            {
                if (!spe_changed(plane->spe_next, sreg))
                    spe_update_rect(plane->spe_next, sys, spic, sreg);
                // else if changed ignore as we are already doing stuff
            }
            else
            {
                if (!spe_changed(plane->spe_cur, sreg))
                    spe_update_rect(plane->spe_cur, sys, spic, sreg);
                else
                {
                    plane->spe_next = spe_new(vd, sys, spic, sreg);
                    spe_convert(plane->spe_next);
                }
            }

            if (++n == MAX_SUBPICS)
                goto subpics_done;
        }
    }
subpics_done:

    // Clear any other entries
    for (; n != MAX_SUBPICS; ++n) {
        subplane_t * const plane = sys->subplanes + n;
        if (plane->spe_next == NULL && spe_changed(plane->spe_cur, NULL))
            plane->spe_next = spe_new(vd, sys, NULL, NULL);
    }

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

    // Check we have a surface to put the video on
    if (bkg_surface_get_lock(vd, sys) == NULL)
    {
        msg_Warn(vd, "%s: No background surface", __func__);
        goto done;
    }
    bkg_surface_unlock(vd, sys);

    if (make_background_and_video(vd, sys) != 0)
    {
        msg_Warn(vd, "%s: Make background fail", __func__);
        goto done;
    }
    make_subpic_surfaces(vd, sys);

    for (unsigned int i = 0; i != MAX_SUBPICS; ++i)
    {
        subplane_t * const plane = sys->subplanes + i;
        subpic_ent_t * spe = plane->spe_cur;

        if (plane->spe_next && atomic_load(&plane->spe_next->ready))
        {
            spe_delete(&plane->spe_cur);
            spe = plane->spe_cur = plane->spe_next;
            plane->spe_next = NULL;
            subpic_ent_attach(plane->surface, spe, sys->eq);
            commit_req(sys, COMMIT_SUB + i);
        }

        if (!spe_no_pic(spe))
            plane_set_rect(sys, plane, spe, COMMIT_SUB + i, COMMIT_VID);
    }

    if (!sys->video_spe.wb)
    {
        msg_Warn(vd, "Display called but no prepared pic buffer");
    }
    else
    {
        subpic_ent_attach(video_surface(sys), &sys->video_spe, sys->eq);
        sys->video_attached = true;
        commit_req(sys, COMMIT_VID);
    }

    set_video_viewport(sys);

    commit_do(vd, sys);

done:
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

    switch (query)
    {
        case VOUT_DISPLAY_RESET_PICTURES:
        {
            vout_display_place_t place;
            video_format_t src;

            assert(sys->video_plane->viewport == NULL);

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

            place_rects(vd, cfg);

            set_video_viewport(sys);

            if (sys->bkg_viewport != NULL && (cfg->display.width != sys->bkg_w || cfg->display.height != sys->bkg_h))
            {
                msg_Dbg(vd, "Resize background: %dx%d", cfg->display.width, cfg->display.height);
                commit_req(sys, COMMIT_BKG);
            }
            sys->bkg_w = cfg->display.width;
            sys->bkg_h = cfg->display.height;
            commit_do(vd, sys);
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
#if TRACE_ALL
    msg_Dbg(vd, "%s[%p], %.4s", __func__, (void*)vd, (const char *)&format);
#endif
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
#if TRACE_ALL
    msg_Dbg(vd, "%s[%p], %.4s %08x%08x", __func__, (void*)vd, (const char *)&format, modifier_hi, modifier_lo);
#endif
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

#if TRACE_ALL
    msg_Dbg(vd, "%s[%p], %.4s", __func__, (void*)vd, (const char *)&format);
#endif
    fmt_list_add(&sys->shm_fmts, format, DRM_FORMAT_MOD_LINEAR, 0);
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_listener_format,
};


static void w_bound_add(vout_display_t * const vd, w_bound_t * const b,
                        struct wl_registry * const registry,
                        const uint32_t name, const char *const iface, const uint32_t vers)
{
#if TRACE_ALL
    msg_Dbg(vd, "global %3"PRIu32": %s version %"PRIu32, name, iface, vers);
#endif
    if (strcmp(iface, wl_subcompositor_interface.name) == 0)
        b->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    else
    if (strcmp(iface, wl_shm_interface.name) == 0)
    {
        b->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(b->shm, &shm_listener, vd);
    }
    else
    if (strcmp(iface, wp_viewporter_interface.name) == 0)
        b->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else
    if (!strcmp(iface, wl_compositor_interface.name))
    {
        if (vers >= 4)
            b->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
        else
            msg_Warn(vd, "Interface %s wanted v 4 got v %d", wl_compositor_interface.name, vers);
    }
    else
    if (!vd->sys->use_shm && strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0)
    {
        if (vers >= 3)
        {
            b->linux_dmabuf_v1 = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
            zwp_linux_dmabuf_v1_add_listener(b->linux_dmabuf_v1, &linux_dmabuf_v1_listener, vd);
        }
        else
            msg_Warn(vd, "Interface %s wanted v 3 got v %d", zwp_linux_dmabuf_v1_interface.name, vers);
    }
#if HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
    else
    if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0)
        b->single_pixel_buffer_manager_v1 = wl_registry_bind(registry, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
#endif
}

static void w_bound_destroy(w_bound_t * const b)
{
    if (b->viewporter != NULL)
        wp_viewporter_destroy(b->viewporter);
    if (b->linux_dmabuf_v1 != NULL)
        zwp_linux_dmabuf_v1_destroy(b->linux_dmabuf_v1);
    if (b->subcompositor != NULL)
        wl_subcompositor_destroy(b->subcompositor);
    if (b->compositor != NULL)
        wl_compositor_destroy(b->compositor);
    if (b->shm != NULL)
        wl_shm_destroy(b->shm);
#if HAVE_WAYLAND_SINGLE_PIXEL_BUFFER
    if (b->single_pixel_buffer_manager_v1)
        wp_single_pixel_buffer_manager_v1_destroy(b->single_pixel_buffer_manager_v1);
#endif
    memset(b, 0, sizeof(*b));
}

static void registry_global_cb(void *data, struct wl_registry *registry,
                               uint32_t name, const char *iface, uint32_t vers)
{
    vout_display_t * const vd = data;
    vout_display_sys_t * const sys = vd->sys;

    w_bound_add(vd, &sys->bound, registry, name, iface, vers);
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

struct registry_scan_bounce_env {
    struct wl_registry * registry;
    eq_env_t * const eq;
    vout_display_t * const vd;
};

// Only safe place to add a listener is on pollq thread
static void
registry_scan_bounce_cb(void * v, short revents)
{
    struct registry_scan_bounce_env * rsbe = v;
    (void)revents;
    rsbe->registry = wl_display_get_registry(eq_wrapper(rsbe->eq)),
    wl_registry_add_listener(rsbe->registry, &registry_cbs, rsbe->vd);
}

// N.B. Having got the registry with a wrapped display
// by default everything we do with the newly bound interfaces will turn
// up on the wrapped queue

static int
registry_scan(vout_display_t * const vd, vout_display_sys_t * const sys)
{
    struct registry_scan_bounce_env rsbe = {
        .registry = NULL,
        .eq = sys->eq,
        .vd = vd
    };

    pollqueue_callback_once(rsbe.eq->pq, registry_scan_bounce_cb, &rsbe);

    eventq_sync(rsbe.eq);
    // Registry callback provokes shm & fmt callbacks so another sync needed
    eventq_sync(rsbe.eq);

    if (rsbe.registry == NULL)
        return -1;

    wl_registry_destroy(rsbe.registry);
    return 0;
}

static const drmu_vlc_fmt_info_t *
find_fmt_fallback(vout_display_t * const vd, const fmt_list_t * const flist, const vlc_fourcc_t * fallback)
{
    const drmu_vlc_fmt_info_t * fmti_best = NULL;
    int pri_best = INT_MAX;

    for (; *fallback != 0; ++fallback)
    {
        const video_frame_format_t vf = {.i_chroma = *fallback};
        const drmu_vlc_fmt_info_t * fmti = NULL;

        msg_Dbg(vd, "Try %s", drmu_log_fourcc(*fallback));

        for (fmti = drmu_vlc_fmt_info_find_vlc(&vf);
             fmti != NULL;
             fmti = drmu_vlc_fmt_info_find_vlc_next(&vf, fmti))
        {
            const int pri = fmt_list_find(flist, fmti);
            msg_Dbg(vd, "Try %s -> %s %"PRIx64": %d", drmu_log_fourcc(*fallback),
                    drmu_log_fourcc(drmu_vlc_fmt_info_drm_pixelformat(fmti)),
                    drmu_vlc_fmt_info_drm_modifier(fmti), pri);
            if (pri >= 0 && pri < pri_best)
            {
                fmti_best = fmti;
                pri_best = pri;

                // If we've got pri 0 then might as well stop now
                if (pri == 0)
                    return fmti_best;
            }
        }
    }

    return fmti_best;
}

static void Close(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t * const sys = vd->sys;

    msg_Dbg(vd, "<<< %s", __func__);

    if (sys == NULL)
        return;

    if (sys->embed == NULL)
        goto no_window;

    if (bkg_surface_get_lock(vd, sys) != NULL)
    {
        unmap_all(sys, true);
        bkg_surface_unlock(vd, sys);
    }

    region_destroy(&sys->region_all);
    region_destroy(&sys->region_none);

    pollqueue_unref(&sys->speq);

    w_bound_destroy(&sys->bound);

    eventq_sync(sys->eq);

    if (eq_finish(&sys->eq) != 0)
        msg_Err(vd, "Failed to reclaim all buffers on close");

    pollqueue_unref(&sys->pollq);

    vout_display_DeleteWindow(vd, sys->embed);
    sys->embed = NULL;

    kill_pool(sys);
    picpool_unref(&sys->subpic_pool);

    free(sys->subpic_chromas);

no_window:
    fmt_list_uninit(&sys->dmabuf_fmts);
    fmt_list_uninit(&sys->shm_fmts);
    free(sys);

    msg_Dbg(vd, ">>> %s", __func__);
}

static int Open(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t *sys;
    const drmu_vlc_fmt_info_t * pic_fmti;
    fmt_list_t * flist = NULL;

    if (var_InheritBool(vd, WL_DMABUF_DISABLE_NAME))
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
    sys->chequerboard = var_InheritBool(vd, WL_DMABUF_CHEQUERBOARD_NAME);

        /* Get window */
    sys->embed = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_WAYLAND);
    if (sys->embed == NULL) {
        msg_Dbg(vd, "Cannot create window - probably not using Wayland");
        goto error;
    }
    sys->last_embed_surface = sys->embed->handle.wl;
    sys->last_embed_seq = sys->embed->handle_seq;

    msg_Info(vd, "<<< %s: %s %dx%d(%dx%d @ %d,%d %d/%d), cfg.display: %dx%d, source: %dx%d(%dx%d @ %d,%d %d/%d)", __func__,
             drmu_log_fourcc(vd->fmt.i_chroma), vd->fmt.i_width, vd->fmt.i_height,
             vd->fmt.i_visible_width, vd->fmt.i_visible_height, vd->fmt.i_x_offset, vd->fmt.i_y_offset,
             vd->fmt.i_sar_num, vd->fmt.i_sar_den,
             vd->cfg->display.width, vd->cfg->display.height,
             vd->source.i_width, vd->source.i_height,
             vd->source.i_visible_width, vd->source.i_visible_height, vd->source.i_x_offset, vd->source.i_y_offset,
             vd->source.i_sar_num, vd->source.i_sar_den);

    if ((sys->pollq = pollqueue_new()) == NULL ||
        (sys->speq = pollqueue_new()) == NULL)
    {
        msg_Err(vd, "Failed to create pollqueues");
        goto error;
    }
    if ((sys->eq = eq_new(video_display(sys), sys->pollq)) == NULL)
    {
        msg_Err(vd, "Failed to create event Q");
        goto error;
    }

    if (registry_scan(vd, sys) != 0)
    {
        msg_Err(vd, "Cannot get registry for display");
        goto error;
    }

    if (sys->bound.compositor == NULL) {
        msg_Warn(vd, "Interface %s missing", wl_compositor_interface.name);
        goto error;
    }
    if (sys->bound.subcompositor == NULL) {
        msg_Warn(vd, "Interface %s missing", wl_subcompositor_interface.name);
        goto error;
    }
    if (sys->bound.viewporter == NULL) {
        msg_Warn(vd, "Interface %s missing", wp_viewporter_interface.name);
        goto error;
    }
    if (!sys->use_shm && sys->bound.linux_dmabuf_v1 == NULL) {
        msg_Warn(vd, "Interface %s missing", zwp_linux_dmabuf_v1_interface.name);
        goto error;
    }

    fmt_list_sort(&sys->dmabuf_fmts);
    fmt_list_sort(&sys->shm_fmts);
    flist = sys->use_shm ? &sys->shm_fmts : &sys->dmabuf_fmts;

    // Check PIC DRM format here
    if ((pic_fmti = drmu_vlc_fmt_info_find_vlc(&vd->fmt)) == NULL ||
        fmt_list_find(flist, pic_fmti) < 0)
    {
        static const vlc_fourcc_t fallback2[] = {
            VLC_CODEC_I420,
            VLC_CODEC_RGB32,
            0
        };

        msg_Warn(vd, "Could not find %s mod %#"PRIx64" in supported formats",
                 drmu_log_fourcc(drmu_vlc_fmt_info_drm_pixelformat(pic_fmti)),
                 drmu_vlc_fmt_info_drm_modifier(pic_fmti));

        if ((pic_fmti = find_fmt_fallback(vd, flist,
                                          vlc_fourcc_IsYUV(vd->fmt.i_chroma) ?
                                              vlc_fourcc_GetYUVFallback(vd->fmt.i_chroma) :
                                              vlc_fourcc_GetRGBFallback(vd->fmt.i_chroma))) == NULL &&
            (pic_fmti = find_fmt_fallback(vd, flist, fallback2)) == NULL)
        {
            msg_Warn(vd, "Failed to find any usable fallback format");
            goto error;
        }
    }

    {
        static vlc_fourcc_t const tryfmts[] = {
            VLC_CODEC_RGBA,
            VLC_CODEC_BGRA,
            VLC_CODEC_ARGB,
            VLC_CODEC_VUYA,
            VLC_CODEC_YUVA,
        };
        unsigned int n = 0;

        if ((sys->subpic_chromas = calloc(ARRAY_SIZE(tryfmts) + 1, sizeof(vlc_fourcc_t))) == NULL)
            goto error;
        for (unsigned int i = 0; i != ARRAY_SIZE(tryfmts); ++i)
        {
            const video_frame_format_t vf = {.i_chroma = tryfmts[i]};
            if (fmt_list_find(flist, drmu_vlc_fmt_info_find_vlc(&vf)) >= 0)
                sys->subpic_chromas[n++] = tryfmts[i];
        }

        if (n == 0)
            msg_Warn(vd, "No compatible subpic formats found");
    }

    {
        struct dmabufs_ctl *dbsc = sys->use_shm ? dmabufs_shm_new() : dmabufs_ctl_new();
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

    sys->bkg_w = vd->cfg->display.width;
    sys->bkg_h = vd->cfg->display.height;

    sys->region_all = wl_compositor_create_region(video_compositor(sys));
    wl_region_add(sys->region_all, 0, 0, INT32_MAX, INT32_MAX);
    sys->region_none = wl_compositor_create_region(video_compositor(sys));
    wl_region_add(sys->region_all, 0, 0, 0, 0);

    vd->fmt.i_chroma = drmu_vlc_fmt_info_vlc_chroma(pic_fmti);
    drmu_vlc_fmt_info_vlc_rgb_masks(pic_fmti, &vd->fmt.i_rmask, &vd->fmt.i_gmask, &vd->fmt.i_bmask);

    place_rects(vd, vd->cfg);

    vd->info.has_pictures_invalid = false;
    vd->info.subpicture_chromas = sys->subpic_chromas;

    vd->pool = vd_dmabuf_pool;
    vd->prepare = Prepare;
    vd->display = Display;
    vd->control = Control;

    msg_Dbg(vd, ">>> %s: OK: %.4s (%#x/%#x/%#x)", __func__,
            (char*)&vd->fmt.i_chroma,
            vd->fmt.i_rmask, vd->fmt.i_gmask, vd->fmt.i_bmask);
    return VLC_SUCCESS;

error:
    Close(obj);
    msg_Dbg(vd, ">>> %s: ERROR", __func__);
    return VLC_EGENERIC;
}

vlc_module_begin()
    set_shortname(N_("WL DMABUF"))
    set_description(N_("Wayland dmabuf video output"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 310)
    set_callbacks(Open, Close)
    add_shortcut("wl-dmabuf")
    add_bool(WL_DMABUF_DISABLE_NAME, false, WL_DMABUF_DISABLE_TEXT, WL_DMABUF_DISABLE_LONGTEXT, false)
    add_bool(WL_DMABUF_USE_SHM_NAME, false, WL_DMABUF_USE_SHM_TEXT, WL_DMABUF_USE_SHM_LONGTEXT, false)
    add_bool(WL_DMABUF_CHEQUERBOARD_NAME, false, WL_DMABUF_CHEQUERBOARD_TEXT, WL_DMABUF_CHEQUERBOARD_LONGTEXT, false)
vlc_module_end()
