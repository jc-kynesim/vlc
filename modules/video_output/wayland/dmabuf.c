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

typedef struct subplane_s {
    struct dmabuf_h * db;
    struct wl_surface * surface;
    struct wl_subsurface * subsurface;
} subplane_t;

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

    video_format_t curr_aspect;

    picpool_ctl_t * subpic_pool;
    subplane_t subplanes[MAX_SUBPICS];
};


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
    int w;
    int h;
};

static struct dmabuf_w_env_s *
dmabuf_w_env_new(vout_display_t * const vd, picture_context_t * pic_ctx)
{
    struct dmabuf_w_env_s * const dbe = malloc(sizeof(*dbe));
    if (!dbe)
        return NULL;
    dbe->pic_ctx = pic_ctx->copy(pic_ctx);
    dbe->vd = vd;
    dbe->w = vd->cfg->display.width;
    dbe->h = vd->cfg->display.height;

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

static void
create_wl_dmabuf_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params,
         struct wl_buffer *new_buffer)
{
    struct dmabuf_w_env_s * const dbe = data;
    vout_display_t * const vd = dbe->vd;
    vout_display_sys_t * const sys = vd->sys;
    struct wl_surface * const surface = sys->embed->handle.wl;

#if 0
    ConstructBufferData *d = data;

    g_mutex_lock(&d->lock);
    d->wbuf = new_buffer;
    g_cond_signal(&d->cond);
    g_mutex_unlock(&d->lock);
#endif
    msg_Dbg(vd, "%s: ok data=%p, %dx%d", __func__, data, dbe->w, dbe->h);
    zwp_linux_buffer_params_v1_destroy(params);

    wl_buffer_add_listener(new_buffer, &w_buffer_listener, dbe);

#if 0
    // *************
    assert(es->sig == ES_SIG);
    if (0)
    {
    wl_buffer_destroy(new_buffer);
    dmabuf_w_env_delete(dbe);
    new_buffer = draw_frame(es);
    }
    // *************

//  wl_surface_attach(es->w_surface2, draw_frame(es), 0, 0);
//  wl_surface_damage(es->w_surface2, 0, 0, INT32_MAX, INT32_MAX);
//    wl_surface_commit(es->w_surface2);
#endif

    wl_surface_attach(surface, new_buffer, 0, 0);
    wp_viewport_set_destination(sys->viewport, dbe->w, dbe->h);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
}

static void
create_wl_dmabuf_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    struct dmabuf_w_env_s * const dbe = data;
#if 0
    ConstructBufferData *d = data;

    g_mutex_lock(&d->lock);
    d->wbuf = NULL;
    g_cond_signal(&d->cond);
    g_mutex_unlock(&d->lock);
#endif
    (void)data;
    msg_Err(dbe->vd, "%s: FAILED", __func__);
    zwp_linux_buffer_params_v1_destroy(params);
    dmabuf_w_env_delete(dbe);
}

static const struct zwp_linux_buffer_params_v1_listener params_wl_dmabuf_listener = {
    create_wl_dmabuf_succeeded,
    create_wl_dmabuf_failed
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
    zwp_linux_buffer_params_v1_add_listener(params, &params_wl_dmabuf_listener,
                                            dmabuf_w_env_new(vd, pic->context));

    zwp_linux_buffer_params_v1_create(params, width, height, format, flags);

#if 0
    /* Wait for the request answer */
    wl_display_flush(gst_wl_display_get_display(display));
    data.wbuf = (gpointer)0x1;
    timeout = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (data.wbuf == (gpointer)0x1) {
        if (!g_cond_wait_until(&data.cond, &data.lock, timeout)) {
            GST_ERROR_OBJECT(mem->allocator, "zwp_linux_buffer_params_v1 time out");
            zwp_linux_buffer_params_v1_destroy(params);
            data.wbuf = NULL;
        }
    }

out:
    if (!data.wbuf) {
        GST_ERROR_OBJECT(mem->allocator, "can't create linux-dmabuf buffer");
    } else {
        GST_DEBUG_OBJECT(mem->allocator, "created linux_dmabuf wl_buffer (%p):"
                 "%dx%d, fmt=%.4s, %d planes",
                 data.wbuf, width, height, (char *)&format, nplanes);
    }

    g_mutex_unlock(&data.lock);
    g_mutex_clear(&data.lock);
    g_cond_clear(&data.cond);

    return data.wbuf;
#endif
    return NULL;
}

static void Prepare(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t *sys = vd->sys;
    sys->x = 0;
    sys->y = 0;

    (void)pic;
    (void) subpic;
}

static void Display(vout_display_t *vd, picture_t *pic, subpicture_t *subpic)
{
    vout_display_sys_t *sys = vd->sys;
    struct wl_display *display = sys->embed->display.wl;

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

            vout_display_PlacePicture(&place, &sys->curr_aspect, vd->cfg, false);
            sys->x += place.width / 2;
            sys->y += place.height / 2;

            vout_display_PlacePicture(&place, &vd->source, cfg, false);
            sys->x -= place.width / 2;
            sys->y -= place.height / 2;

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
                                place.width, place.height);
            }
            else
                vout_display_SendEventPicturesInvalid(vd);
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
    (void)zwp_linux_dmabuf_v1;
    (void)format;
    msg_Dbg(vd, "%s[%p], %.4s", __func__, (void*)vd, (const char *)&format);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
         struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
         uint32_t format,
         uint32_t modifier_hi,
         uint32_t modifier_lo)
{
    vout_display_t * const vd = data;
    (void)zwp_linux_dmabuf_v1;

    msg_Dbg(vd, "%s[%p], %.4s %08x%08x", __func__, (void*)vd, (const char *)&format, modifier_hi, modifier_lo);
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
        sys->linux_dmabuf_v1_bind = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 2);
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

static struct wl_buffer *
draw_frame(vout_display_sys_t *sys)
{
	const int width = 640, height = 480;
	int stride = width * 4;
	int size = stride * height;
    uint32_t *data = sys->shm_mmap;

	struct wl_buffer *buffer = wl_shm_pool_create_buffer(sys->shm_pool, 0,
			width, height, stride, WL_SHM_FORMAT_XRGB8888);

    chequerboard(data, stride, width, height);

	wl_buffer_add_listener(buffer, &shm_buffer_listener, NULL);
	return buffer;
}


static int Open(vlc_object_t *obj)
{
    vout_display_t * const vd = (vout_display_t *)obj;
    vout_display_sys_t *sys;

    msg_Info(vd, "<<< %s: %.4s %dx%d", __func__,
             (const char*)&vd->fmt.i_chroma, vd->fmt.i_width, vd->fmt.i_height);

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

    struct wl_surface *surface = sys->embed->handle.wl;
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

    {
        unsigned int i;
        struct wl_compositor * const compositor = sys->embed->compositor.wl;

        if (compositor == NULL)
        {
            msg_Err(vd, "Can't get compositor");
            goto error;
        }

        for (i = 0; i != MAX_SUBPICS; ++i)
        {
            subplane_t *plane = sys->subplanes + i;
            plane->surface = wl_compositor_create_surface(compositor);
            plane->subsurface = wl_subcompositor_get_subsurface(sys->subcompositor, plane->surface, sys->embed->handle.wl);
            wl_subsurface_set_sync(plane->subsurface);
        }
#if 0
        // *****
        wl_subsurface_set_position(sys->subplanes[0].subsurface, 20, 20);
        wl_subsurface_place_above(sys->subplanes[0].subsurface, sys->embed->handle.wl);
        wl_surface_attach(sys->subplanes[0].surface, draw_frame(sys), 0, 0);
        wl_surface_commit(sys->subplanes[0].surface);
#endif
    }

    {
        unsigned int width = 640;
        unsigned int height = 480;
        unsigned int stride = 640 * 4;
        struct zwp_linux_buffer_params_v1 *params;
        struct wl_buffer * w_buffer;
        struct dmabuf_h *dh = picpool_get(sys->subpic_pool, stride * height);
        dmabuf_write_start(dh);
        chequerboard(dmabuf_map(dh), stride, width, height);
        dmabuf_write_end(dh);
        sys->subplanes[0].db = dh;

        params = zwp_linux_dmabuf_v1_create_params(sys->linux_dmabuf_v1_bind);
        if (!params)
        {
            msg_Err(vd, "zwp_linux_dmabuf_v1_create_params FAILED");
            goto error;
        }
        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
        w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_ARGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);

        wl_subsurface_set_position(sys->subplanes[0].subsurface, 20, 20);
        wl_subsurface_place_above(sys->subplanes[0].subsurface, sys->embed->handle.wl);
        wl_surface_attach(sys->subplanes[0].surface, w_buffer, 0, 0);
        wl_surface_commit(sys->subplanes[0].surface);

    }

    sys->curr_aspect = vd->source;

    vd->info.has_pictures_invalid = sys->viewport == NULL;

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
