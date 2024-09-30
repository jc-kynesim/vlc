/**
 * @file xdg-shell.c
 * @brief XDG shell surface provider module for VLC media player
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
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "server-decoration-client-protocol.h"
#ifdef HAVE_WAYLAND_CURSOR_SHAPE
#include "cursor-shape-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#endif
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_window.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#define WL_XDG_SHELL_NAME "wl-xdg-shell"
#define WL_XDG_SHELL_TEXT N_("WL XDG Shell enable")
#define WL_XDG_SHELL_LONGTEXT N_("WL XDG Shell enable/disable; enabled by default. "\
    "Use --no-wl-xdg-shell to disable. This provides a simple wayland window " \
    " if something like Qt isn't in use, and works with command-line VLCs.")

#define DISPLAY_NAME "wl-display"
#define DISPLAY_TEXT N_("Wayland display")
#define DISPLAY_LONGTEXT N_( \
    "Video will be rendered with this Wayland display. " \
    "If unset the WAYLAND_DISPLAY environment variable will be used; " \
    "if both unset then wl-xdg-shell will be disabled. " \
    "Special values are: \"auto\": use default; \"none\": disable.")

#define LAYER_NAME "wl-layer-pos"
#define LAYER_TEXT "Create window using layer-shell"
#define LAYER_LONGTEXT "Use wlr-layer-shell rather than xdg-shell. This " \
    "(probably) allows positioning via --video-x,--video-y to work, but the " \
    "window has a fixed Z which will either be on top of all normal windows " \
    "or behind them and likely won't decorate."


struct vout_window_sys_t
{
    struct wl_compositor *compositor;
    struct wl_output *output;
    struct xdg_wm_base *shell;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;  // Only one of xdg_surface & layer_surface should be in use
    struct xdg_toplevel *toplevel;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct org_kde_kwin_server_decoration_manager *deco_manager;
    struct org_kde_kwin_server_decoration *deco;
    struct wl_seat *wl_seat;
    struct wl_pointer *wl_pointer;
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_INTERFACE
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
#endif

    vlc_mutex_t lock;
    vlc_cond_t cond;

    bool use_layer;
    bool config1_done;

    uint32_t pointer_enter_serial;

    bool req_fullscreen;
    uint32_t output_width;
    uint32_t output_height;

    // 0   Off
    // 1   On
    // -1  Off until 1st movement
    int req_cursor;
    unsigned int req_width;
    unsigned int req_height;

    unsigned int conf_width;
    unsigned int conf_height;

    vlc_thread_t thread;
};

static void cleanup_wl_display_read(void *data)
{
    struct wl_display *display = data;

    wl_display_cancel_read(display);
}

/** Background thread for Wayland shell events handling */
static void *Thread(void *data)
{
    vout_window_t *wnd = data;
    struct wl_display *display = wnd->display.wl;
    struct pollfd ufd[1];

    int canc = vlc_savecancel();
    vlc_cleanup_push(cleanup_wl_display_read, display);

    ufd[0].fd = wl_display_get_fd(display);
    ufd[0].events = POLLIN;

    for (;;)
    {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);

        wl_display_flush(display);
        vlc_restorecancel(canc);

        while (poll(ufd, 1, -1) < 0);

        canc = vlc_savecancel();
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);
    }
    vlc_assert_unreachable();
    vlc_cleanup_pop();
    //vlc_restorecancel(canc);
    //return NULL;
}

static void
set_cursor(vout_window_t * const wnd, vout_window_sys_t * const sys)
{
    if (!sys->wl_pointer)
        return;
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_INTERFACE
    if (sys->req_cursor > 0 && sys->cursor_shape_device != NULL)
        wp_cursor_shape_device_v1_set_shape(sys->cursor_shape_device,
                                            sys->pointer_enter_serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    else
#endif
        wl_pointer_set_cursor(sys->wl_pointer, sys->pointer_enter_serial, NULL, 0, 0);
    wl_display_flush(wnd->display.wl);
}

static void
set_fullscreen(vout_window_sys_t *sys, bool fs)
{
    sys->req_fullscreen = fs;
    if (fs)
        xdg_toplevel_set_fullscreen(sys->toplevel, NULL);
    else {
        xdg_toplevel_unset_fullscreen(sys->toplevel);
        xdg_surface_set_window_geometry(sys->xdg_surface, 0, 0, sys->req_width, sys->req_height);
    }
}

static int Control(vout_window_t *wnd, int cmd, va_list ap)
{
    vout_window_sys_t *sys = wnd->sys;
    struct wl_display *display = wnd->display.wl;

    switch (cmd)
    {
        case VOUT_WINDOW_SET_STATE:
            return VLC_EGENERIC;

        case VOUT_WINDOW_SET_SIZE:
        {
            unsigned width = va_arg(ap, unsigned);
            unsigned height = va_arg(ap, unsigned);

            msg_Dbg(wnd, "Set size: %dx%d", width, height);
            if (width == sys->req_width && height == sys->req_height)
                break;
            sys->req_width = width;
            sys->req_height = height;
            if (sys->req_fullscreen)
                break;

            /* Unlike X11, the client basically gets to choose its size, which
             * is the size of the buffer attached to the surface.
             * Note that it is unspecified who "wins" in case of a race
             * (e.g. if trying to resize the window, and changing the zoom
             * at the same time). With X11, the race is arbitrated by the X11
             * server. With Wayland, it is arbitrated in the client windowing
             * code. In this case, it is arbitrated by the window core code.
             */
//            vout_window_ReportSize(wnd, width, height);
            if (sys->xdg_surface)
                xdg_surface_set_window_geometry(sys->xdg_surface, 0, 0, width, height);
            wl_surface_commit(wnd->handle.wl);
            break;
        }

        case VOUT_WINDOW_SET_FULLSCREEN:
        {
            bool fs = va_arg(ap, int);
            msg_Dbg(wnd, "Set fullscreen: %d->%d", sys->req_fullscreen, fs);

            if (sys->req_fullscreen == fs || sys->use_layer)
                break;

            set_fullscreen(sys, fs);
            wl_surface_commit(wnd->handle.wl);
            break;
        }

        case VOUT_WINDOW_HIDE_MOUSE: /* int b_hide */
        {
            const bool hide_req = va_arg(ap, int);
            vlc_mutex_lock(&sys->lock);
            sys->req_cursor = !hide_req;
            set_cursor(wnd, sys);
            vlc_mutex_unlock(&sys->lock);
            break;
        }

        default:
            msg_Err(wnd, "request %d not implemented", cmd);
            return VLC_EGENERIC;
    }

    wl_display_flush(display);
    return VLC_SUCCESS;
}

// ---------------------------------------------------------------------------
//
// XDG Toplevel callbacks
// Mostly ignored - except resize

static void
xdg_toplevel_configure_cb(void *data,
                          struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
                          struct wl_array *states)
{
    vout_window_t *const wnd = data;
    vout_window_sys_t *const sys = wnd->sys;

    enum xdg_toplevel_state *p;
    msg_Dbg(wnd, "%s: %dx%d", __func__, w, h);
    wl_array_for_each(p, states) {
        msg_Dbg(wnd, "    State: %d", *p);
    }

    (void)xdg_toplevel;
    (void)states;

    // no window geometry event, ignore
    if (w <= 0 || h <= 0)
        return;

    msg_Dbg(wnd, "%s: Width=%"PRId32", Height=%"PRId32, __func__, w, h);
    sys->conf_width = w;
    sys->conf_height = h;
}

static void
xdg_toplevel_close_cb(void *data, struct xdg_toplevel *xdg_toplevel)
{
    vout_window_t *const wnd = data;
    (void)xdg_toplevel;

    vout_window_ReportClose(wnd);
}

#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
static void
xdg_toplevel_configure_bounds_cb(void *data,
                                 struct xdg_toplevel *xdg_toplevel,
                                 int32_t width, int32_t height)
{
    vout_window_t *const wnd = data;
    (void)data;
    (void)xdg_toplevel;
    (void)width;
    (void)height;
    msg_Dbg(wnd, "%s: %dx%d", __func__, width, height);
}
#endif

#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
static void
xdg_toplevel_wm_capabilities_cb(void *data,
                                struct xdg_toplevel *xdg_toplevel,
                                struct wl_array *capabilities)
{
    (void)data;
    (void)xdg_toplevel;
    (void)capabilities;
}
#endif

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure_cb,
    .close = xdg_toplevel_close_cb,
#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
    .configure_bounds = xdg_toplevel_configure_bounds_cb,
#endif
#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
    .wm_capabilities = xdg_toplevel_wm_capabilities_cb,
#endif
};

// ---------------------------------------------------------------------------

static void
xdg_surface_configure_cb(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    vout_window_t *const wnd = data;
    vout_window_sys_t *const sys = wnd->sys;

    msg_Dbg(wnd, "new configuration: (serial: %"PRIu32", %dx%d)", serial, sys->conf_width, sys->conf_height);

    /* Zero width or zero height means client (we) should choose.
     * DO NOT REPORT those values to video output... */
    if (sys->conf_width != 0 && sys->conf_height != 0)
        vout_window_ReportSize(wnd, sys->conf_width, sys->conf_height);
    else if (sys->req_fullscreen)
    {
        msg_Dbg(wnd, "Use fullscreen size %dx%d", sys->output_width, sys->output_height);
        vout_window_ReportSize(wnd, sys->output_width, sys->output_height);
    }

    sys->conf_width = 0;
    sys->conf_height = 0;

    /* TODO: report fullscreen state, not implemented in VLC */
    xdg_surface_ack_configure(xdg_surface, serial);

    if (!sys->config1_done)
    {
        sys->config1_done = true;
        vlc_cond_broadcast(&sys->cond);
    }
}

static const struct xdg_surface_listener xdg_surface_cbs =
{
    xdg_surface_configure_cb,
};

static void xdg_shell_ping_cb(void *data, struct xdg_wm_base *shell,
                              uint32_t serial)
{
    (void) data;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_shell_cbs =
{
    xdg_shell_ping_cb,
};

// ----------------------------------------------------------------------------

static void layer_surface_configure(void * data, struct zwlr_layer_surface_v1 * layer_surface, uint32_t serial, uint32_t w, uint32_t h)
{
    vout_window_t * const wnd = data;
    vout_window_sys_t * const sys = wnd->sys;

    msg_Info(wnd, "%s: ser: %d, %dx%d", __func__, serial, w, h);

    if (w != 0 && h != 0)
        vout_window_ReportSize(wnd, w, h);

    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    if (!sys->config1_done)
    {
        sys->config1_done = true;
        vlc_cond_broadcast(&sys->cond);
    }
}

static void layer_surface_closed(void * data, struct zwlr_layer_surface_v1 * layer_surface)
{
    vout_window_t *wnd = data;
    VLC_UNUSED(layer_surface);

    msg_Info(wnd, "%s", __func__);
    vout_window_ReportClose(wnd);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_cbs =
{
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

// ----------------------------------------------------------------------------
//
// Mouse

static void
pointer_destroy(struct wl_pointer **ppPointer)
{
    struct wl_pointer * const p = *ppPointer;
    if (p == NULL)
        return;
    *ppPointer = NULL;
    wl_pointer_destroy(p);
}

#ifdef WP_CURSOR_SHAPE_DEVICE_V1_INTERFACE
static void
cursor_shape_manager_destroy(struct wp_cursor_shape_manager_v1 **ppCursor_shape_manager)
{
    struct wp_cursor_shape_manager_v1 * const p = *ppCursor_shape_manager;
    if (p == NULL)
        return;
    *ppCursor_shape_manager = NULL;
    wp_cursor_shape_manager_v1_destroy(p);
}

static void
cursor_shape_device_destroy(struct wp_cursor_shape_device_v1 **ppCursor_shape_device)
{
    struct wp_cursor_shape_device_v1 * const p = *ppCursor_shape_device;
    if (p == NULL)
        return;
    *ppCursor_shape_device = NULL;
    wp_cursor_shape_device_v1_destroy(p);
}
#else
// Avoid having to ifdef these on use
#define cursor_shape_manager_destroy(x)
#define cursor_shape_device_destroy(x)
#endif

static void pointer_enter_cb(void *data,
          struct wl_pointer *wl_pointer,
          uint32_t serial,
          struct wl_surface *surface,
          wl_fixed_t surface_x,
          wl_fixed_t surface_y)
{
    vout_window_t * const wnd = data;
    vout_window_sys_t * const sys = wnd->sys;

    if (surface != wnd->handle.wl)
    {
        msg_Warn(wnd, "%s: Surface mismatch", __func__);
        return;
    }

    if (wl_pointer != sys->wl_pointer)
    {
        msg_Warn(wnd, "%s: Pointer mismatch", __func__);
    }
    else
    {
        vlc_mutex_lock(&sys->lock);
        sys->pointer_enter_serial = serial;
        set_cursor(wnd, sys);
        vlc_mutex_unlock(&sys->lock);
    }

    vout_window_ReportMouseMoved(wnd, wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));

    msg_Dbg(wnd, "%s[%u]: @%d,%d", __func__, serial, wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));
}

static void pointer_leave_cb(void *data,
          struct wl_pointer *wl_pointer,
          uint32_t serial,
          struct wl_surface *surface)
{
    vout_window_t * const wnd = data;

    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(serial);
    VLC_UNUSED(surface);

    msg_Dbg(wnd, "%s[%u]", __func__, serial);
}

static void pointer_motion_cb(void *data,
           struct wl_pointer *wl_pointer,
           uint32_t time,
           wl_fixed_t surface_x,
           wl_fixed_t surface_y)
{
    vout_window_t * const wnd = data;
    vout_window_sys_t * const sys = wnd->sys;

    if (sys->req_cursor < 0)
    {
        vlc_mutex_lock(&sys->lock);
        if (sys->req_cursor < 0)
            sys->req_cursor = 1;
        set_cursor(wnd, sys);
        vlc_mutex_unlock(&sys->lock);
    }

    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(time);

    vout_window_ReportMouseMoved(wnd, wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));
//    msg_Dbg(wnd, "%s: @%d,%d", __func__, wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));
}

static void 
pointer_button_cb(void *data,
           struct wl_pointer *wl_pointer,
           uint32_t serial,
           uint32_t time,
           uint32_t button,
           uint32_t state)
{
    vout_window_t * const wnd = data;

    // The button is a button code as defined in the Linux kernel's
    // linux/input-event-codes.h header file, e.g. BTN_LEFT.

    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(serial);
    VLC_UNUSED(time);

    if (state == WL_POINTER_BUTTON_STATE_RELEASED)
        vout_window_ReportMouseReleased(wnd, button);
    else if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        vout_window_ReportMousePressed(wnd, button);

    msg_Dbg(wnd, "%s: Button %d, State: %d", __func__, button, state);
}

static void pointer_axis_cb(void *data,
         struct wl_pointer *wl_pointer,
         uint32_t time,
         uint32_t axis,
         wl_fixed_t value)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(time);
    VLC_UNUSED(axis);
    VLC_UNUSED(value);
}

static void pointer_frame_cb(void *data,
          struct wl_pointer *wl_pointer)
{
    // Maybe accumulate pointer & buttons then report in bulk here?
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
}

static void
pointer_axis_source_cb(void *data,
            struct wl_pointer *wl_pointer,
            uint32_t axis_source)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(axis_source);
}

static void
pointer_axis_stop_cb(void *data,
          struct wl_pointer *wl_pointer,
          uint32_t time,
          uint32_t axis)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(time);
    VLC_UNUSED(axis);
}

static void
pointer_axis_discrete_cb(void *data,
              struct wl_pointer *wl_pointer,
              uint32_t axis,
              int32_t discrete)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(axis);
    VLC_UNUSED(discrete);
}

#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
static void pointer_axis_value120_cb(void *data,
              struct wl_pointer *wl_pointer,
              uint32_t axis,
              int32_t value120)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(axis);
    VLC_UNUSED(value120);
}
#endif

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void pointer_axis_relative_direction_cb(void *data,
                struct wl_pointer *wl_pointer,
                uint32_t axis,
                uint32_t direction)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(axis);
    VLC_UNUSED(direction);
}
#endif

static const struct wl_pointer_listener pointer_cbs = {
    .enter = pointer_enter_cb,
    .leave = pointer_leave_cb,
    .motion = pointer_motion_cb,
    .button = pointer_button_cb,
    .axis = pointer_axis_cb,
    .frame = pointer_frame_cb,
    .axis_source = pointer_axis_source_cb,
    .axis_stop = pointer_axis_stop_cb,
    .axis_discrete = pointer_axis_discrete_cb,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    .axis_value120 = pointer_axis_value120_cb,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = pointer_axis_relative_direction_cb,
#endif
};

static void
seat_capabilities_cb(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
    vout_window_t * const wnd = data;
    vout_window_sys_t * const sys = wnd->sys;
    VLC_UNUSED(wl_seat);
    msg_Dbg(wnd, "%s: Caps: %#"PRIx32, __func__, capabilities);

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0)
    {
        if (sys->wl_pointer != NULL)
            /* Do nothing */;
        else if ((sys->wl_pointer = wl_seat_get_pointer(wl_seat)) == NULL)
            msg_Dbg(wnd, "%s: Ponter capability but no pointer", __func__);
        else
        {
            wl_pointer_add_listener(sys->wl_pointer, &pointer_cbs, wnd);
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_INTERFACE
            if (sys->cursor_shape_manager != NULL)
                sys->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(sys->cursor_shape_manager, sys->wl_pointer);
#endif
        }
    }
    else
    {
        cursor_shape_device_destroy(&sys->cursor_shape_device);
        pointer_destroy(&sys->wl_pointer);
    }
}

static void
seat_name_cb(void *data, struct wl_seat *wl_seat, const char *name)
{
    vout_window_t * const wnd = data;
    VLC_UNUSED(wl_seat);
    msg_Dbg(wnd, "%s: %s", __func__, name);
}

static const struct wl_seat_listener seat_cbs = {
    .capabilities = seat_capabilities_cb,
    .name = seat_name_cb
};

// ----------------------------------------------------------------------------
//
// Get output info
//
// Maybe don't need/want this. It is an attempt to get the fullscreen size out
// earlier due to XDG apparently requiring a buffer attached before the
// fullscreen config occurs.
//
// If XDG did what I think it should (i.e. return the size before a buffer is
// attached so you can size the buffer correctly) this would be completely
// redundant

static void
output_destroy(struct wl_output ** const ppOutput)
{
    struct wl_output * const output = *ppOutput;

    if (output == NULL)
        return;
    *ppOutput = NULL;
    wl_output_destroy(output);
}

static void
output_geometry_cb(void *data,
                   struct wl_output *wl_output,
                   int32_t x,
                   int32_t y,
                   int32_t physical_width,
                   int32_t physical_height,
                   int32_t subpixel,
                   const char *make,
                   const char *model,
                   int32_t transform)
{
    vout_window_t *const wnd = data;
    VLC_UNUSED(wl_output);
    msg_Dbg(wnd, "%s: @%d,%d, %dx%dmm, subpixel %d, make='%s', model='%s', transform=%d", __func__,
            x, y, physical_width, physical_height,
            subpixel, make, model, transform);
}

static void
output_mode_cb(void *data,
               struct wl_output *wl_output,
               uint32_t flags,
               int32_t width,
               int32_t height,
               int32_t refresh)
{
    vout_window_t *const wnd = data;
    vout_window_sys_t * const sys = wnd->sys;
    VLC_UNUSED(wl_output);

    sys->output_width = width;
    sys->output_height = height;

    msg_Dbg(wnd, "%s: flags=%#x %dx%dpels, refresh=%d", __func__,
            flags, width, height, refresh);
}

static void
output_done_cb(void *data,
               struct wl_output *wl_output)
{
    vout_window_t *const wnd = data;
    VLC_UNUSED(wl_output);
    msg_Dbg(wnd, "%s", __func__);
}

static void
output_scale_cb(void *data,
                struct wl_output *wl_output,
                int32_t factor)
{
    vout_window_t *const wnd = data;
    VLC_UNUSED(wl_output);
    msg_Dbg(wnd, "%s: %d", __func__, factor);
}

static void
output_name_cb(void *data,
               struct wl_output *wl_output,
               const char *name)
{
    vout_window_t *const wnd = data;
    VLC_UNUSED(wl_output);
    msg_Dbg(wnd, "%s: %s", __func__, name);
}

static void
output_description_cb(void *data,
                      struct wl_output *wl_output,
                      const char *description)
{
    vout_window_t *const wnd = data;
    VLC_UNUSED(wl_output);
    msg_Dbg(wnd, "%s: %s", __func__, description);
}

static const struct wl_output_listener output_cbs =
{
    .geometry = output_geometry_cb,
    .mode = output_mode_cb,
    .done = output_done_cb,
    .scale = output_scale_cb,
    .name = output_name_cb,
    .description = output_description_cb,
};

// ----------------------------------------------------------------------------

static void registry_global_cb(void *data, struct wl_registry *registry,
                               uint32_t name, const char *iface, uint32_t vers)
{
    vout_window_t *wnd = data;
    vout_window_sys_t *sys = wnd->sys;

    msg_Dbg(wnd, "global %3"PRIu32": %s version %"PRIu32, name, iface, vers);

    if (!strcmp(iface, wl_compositor_interface.name))
    {
        sys->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface, MIN(6, vers));
    } else
    if (!strcmp(iface, xdg_wm_base_interface.name))
    {
        sys->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(sys->shell, &xdg_shell_cbs, wnd);
    } else
    if (!strcmp(iface, wl_output_interface.name) && vers >= 3)
    {
        sys->output = wl_registry_bind(registry, name, &wl_output_interface, MIN(4, vers));
        wl_output_add_listener(sys->output, &output_cbs, wnd);
    } else
    if (!strcmp(iface, wl_seat_interface.name) && vers >= 5)
    {
        sys->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, MIN(9, vers));
        wl_seat_add_listener(sys->wl_seat, &seat_cbs, wnd);
    } else
    if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
    {
        sys->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, MIN(5, vers));
    } else
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_INTERFACE
    if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name))
    {
        sys->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else
#endif
    if (!strcmp(iface, "org_kde_kwin_server_decoration_manager"))
    {
        sys->deco_manager = wl_registry_bind(registry, name,
                         &org_kde_kwin_server_decoration_manager_interface, 1);
    }
}

static void registry_global_remove_cb(void *data, struct wl_registry *registry,
                                      uint32_t name)
{
    vout_window_t *wnd = data;

    msg_Dbg(wnd, "global remove %3"PRIu32, name);
    (void) registry;
}

static const struct wl_registry_listener registry_cbs =
{
    registry_global_cb,
    registry_global_remove_cb,
};

static void
layer_shell_destroy(struct zwlr_layer_shell_v1 ** ppLayer_shell)
{
    struct zwlr_layer_shell_v1 * const layer_shell = *ppLayer_shell;
    *ppLayer_shell = NULL;
    if (layer_shell)
        zwlr_layer_shell_v1_destroy(layer_shell);
}

static void
layer_surface_destroy(struct zwlr_layer_surface_v1 ** ppLayer_surface)
{
    struct zwlr_layer_surface_v1 * const layer_surface = *ppLayer_surface;
    *ppLayer_surface = NULL;
    if (layer_surface)
        zwlr_layer_surface_v1_destroy(layer_surface);
}

// Look for a wayland display name first in an explicit option then in the
// environment
static struct wl_display *
get_wl_display(vout_window_t * const wnd)
{
    char *const dpy_opt = var_InheritString(wnd, DISPLAY_NAME);
    char * dpy_name = dpy_opt;
    struct wl_display * display;

    if (dpy_name == NULL)
        dpy_name = getenv("WAYLAND_DISPLAY");
    if (dpy_name == NULL || strcmp(dpy_name, "none") == 0)
        return NULL;

    if ((display = wl_display_connect(strcmp(dpy_name, "auto") == 0 ? NULL : dpy_name)) == NULL)
        msg_Warn(wnd, "WL XDG No display: '%s'", dpy_name);

    free(dpy_opt);
    return display;
}

static int
wants_layer_shell(vout_window_t * const wnd, vout_window_sys_t * const sys)
{
    const bool wants_layer = var_InheritBool(wnd, LAYER_NAME);

    if (!wants_layer)
        return false;

    if (sys->layer_shell == NULL)
    {
        msg_Err(wnd, "WLR Layer Shell not supported on this system");
        return false;
    }

    return true;
}

/**
 * Creates a Wayland shell surface.
 */
static int Open(vout_window_t *wnd, const vout_window_cfg_t *cfg)
{
    if (cfg->type != VOUT_WINDOW_TYPE_INVALID
     && cfg->type != VOUT_WINDOW_TYPE_WAYLAND)
        return VLC_EGENERIC;

    if (!var_InheritBool(wnd, WL_XDG_SHELL_NAME))
        return VLC_EGENERIC;

    struct wl_display *const display = get_wl_display(wnd);
    if (display == NULL)
        return VLC_EGENERIC;

    vout_window_sys_t *const sys = calloc(1, sizeof (*sys));
    if (unlikely(sys == NULL))
    {
        wl_display_disconnect(display);
        return VLC_ENOMEM;
    }

    wnd->sys = sys;
    vlc_mutex_init(&sys->lock);
    vlc_cond_init(&sys->cond);

    /* Connect to the display server */

    msg_Info(wnd, "<<< WL XDG: %dx%d fs %d standalone %d", cfg->width, cfg->height, cfg->is_fullscreen, cfg->is_standalone);

    /* Find the interesting singleton(s) */
    struct wl_registry *registry = wl_display_get_registry(display);
    if (registry == NULL)
        goto error;

    wl_registry_add_listener(registry, &registry_cbs, wnd);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    if (sys->compositor == NULL || sys->shell == NULL)
    {
        msg_Info(wnd, ">>> WL XDG No compositor or shell");
        goto error;
    }

    /* Create a surface */
    struct wl_surface *surface = wl_compositor_create_surface(sys->compositor);
    if (surface == NULL)
        goto error;

    sys->req_width = cfg->width;
    sys->req_height = cfg->height;

    if (wants_layer_shell(wnd, sys))
    {
        int posX = var_InheritInteger(wnd, "video-x");
        int posY = var_InheritInteger(wnd, "video-y");

        msg_Info(wnd, "Create layer shell @ %d,%d", posX, posY);

        sys->use_layer = true;

        sys->layer_surface = zwlr_layer_shell_v1_get_layer_surface(sys->layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "vlc-video");
        zwlr_layer_surface_v1_add_listener(sys->layer_surface, &layer_surface_cbs, wnd);
        zwlr_layer_surface_v1_set_anchor(sys->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_margin(sys->layer_surface, posY, 0, 0, posX);
    }
    else
    {
        struct xdg_surface *xdg_surface =
            xdg_wm_base_get_xdg_surface(sys->shell, surface);
        if (xdg_surface == NULL)
            goto error;

        sys->xdg_surface = xdg_surface;
        xdg_surface_add_listener(xdg_surface, &xdg_surface_cbs, wnd);

        sys->toplevel = xdg_surface_get_toplevel(sys->xdg_surface);
        xdg_toplevel_add_listener(sys->toplevel, &xdg_toplevel_listener, wnd);

        char *title = var_InheritString(wnd, "video-title");
        xdg_toplevel_set_title(sys->toplevel,
                              (title != NULL) ? title : _("VLC media player"));
        free(title);

        char *app_id = var_InheritString(wnd, "app-id");
        if (app_id != NULL)
        {
            xdg_toplevel_set_app_id(sys->toplevel, app_id);
            free(app_id);
        }

        set_fullscreen(sys, cfg->is_fullscreen);
    }

    const uint_fast32_t deco_mode =
        var_InheritBool(wnd, "video-deco")
            ? ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER
            : ORG_KDE_KWIN_SERVER_DECORATION_MODE_CLIENT;

    if (sys->deco_manager != NULL)
        sys->deco = org_kde_kwin_server_decoration_manager_create(
                                                   sys->deco_manager, surface);
    if (sys->deco != NULL)
        org_kde_kwin_server_decoration_request_mode(sys->deco, deco_mode);
    else if (deco_mode == ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER)
        msg_Err(wnd, "server-side decoration not supported");

    wl_surface_commit(surface);

    //if (var_InheritBool (wnd, "keyboard-events"))
    //    do_something();

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    wnd->type = VOUT_WINDOW_TYPE_WAYLAND;
    wnd->handle.wl = surface;
    wnd->display.wl = display;
    wnd->control = Control;
    wnd->info.has_double_click = false;

    if (vlc_clone(&sys->thread, Thread, wnd, VLC_THREAD_PRIORITY_LOW))
        goto error;

    vlc_mutex_lock(&sys->lock);
    mutex_cleanup_push(&sys->lock); // release the mutex in case of cancellation

    vlc_tick_t timeout = mdate() + 1000000;
    while (!sys->config1_done)
        if (vlc_cond_timedwait(&sys->cond, &sys->lock, timeout))
        {
            msg_Err(wnd, "Window configure timed out");
            break;
        }

    vlc_cleanup_pop();
    vlc_mutex_unlock(&sys->lock);

    msg_Dbg(wnd, ">>> WL XDG: OK");
    return VLC_SUCCESS;

error:
    if (sys->deco != NULL)
        org_kde_kwin_server_decoration_destroy(sys->deco);
    if (sys->deco_manager != NULL)
        org_kde_kwin_server_decoration_manager_destroy(sys->deco_manager);
    if (sys->toplevel != NULL)
        xdg_toplevel_destroy(sys->toplevel);
    if (sys->xdg_surface != NULL)
        xdg_surface_destroy(sys->xdg_surface);
    if (sys->shell != NULL)
        xdg_wm_base_destroy(sys->shell);
    if (sys->wl_seat)
        wl_seat_destroy(sys->wl_seat);
    layer_shell_destroy(&sys->layer_shell);
    output_destroy(&sys->output);
    if (sys->compositor != NULL)
        wl_compositor_destroy(sys->compositor);
    wl_display_disconnect(display);

    vlc_cond_destroy(&sys->cond);
    vlc_mutex_destroy(&sys->lock);
    free(sys);

    msg_Dbg(wnd, ">>> WL XDG: Error");
    return VLC_EGENERIC;
}

/**
 * Destroys a XDG shell surface.
 */
static void Close(vout_window_t *wnd)
{
    vout_window_sys_t *sys = wnd->sys;

    vlc_cancel(sys->thread);
    vlc_join(sys->thread, NULL);

    if (sys->deco != NULL)
        org_kde_kwin_server_decoration_destroy(sys->deco);
    if (sys->deco_manager != NULL)
        org_kde_kwin_server_decoration_manager_destroy(sys->deco_manager);
    if (sys->toplevel)
        xdg_toplevel_destroy(sys->toplevel);
    if (sys->xdg_surface)
        xdg_surface_destroy(sys->xdg_surface);
    layer_surface_destroy(&sys->layer_surface);
    wl_surface_destroy(wnd->handle.wl);
    xdg_wm_base_destroy(sys->shell);
    cursor_shape_device_destroy(&sys->cursor_shape_device);
    pointer_destroy(&sys->wl_pointer);
    cursor_shape_manager_destroy(&sys->cursor_shape_manager);
    layer_shell_destroy(&sys->layer_shell);
    output_destroy(&sys->output);
    wl_seat_destroy(sys->wl_seat);
    wl_compositor_destroy(sys->compositor);
    wl_display_disconnect(wnd->display.wl);

    vlc_cond_destroy(&sys->cond);
    vlc_mutex_destroy(&sys->lock);
    free(sys);
}

vlc_module_begin()
    set_shortname(N_("WL XDG shell"))
    set_description(N_("Wayland XDG shell surface"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout window", 21)
    set_callbacks(Open, Close)

    add_string(DISPLAY_NAME, NULL, DISPLAY_TEXT, DISPLAY_LONGTEXT, true)
    add_bool(LAYER_NAME, false, LAYER_TEXT, LAYER_LONGTEXT, false)
    add_bool(WL_XDG_SHELL_NAME, true, WL_XDG_SHELL_TEXT, WL_XDG_SHELL_LONGTEXT, false)

vlc_module_end()
