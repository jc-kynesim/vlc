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

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_window.h>

struct vout_window_sys_t
{
    struct wl_compositor *compositor;
    struct xdg_wm_base *shell;
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct org_kde_kwin_server_decoration_manager *deco_manager;
    struct org_kde_kwin_server_decoration *deco;

    struct wl_seat *wl_seat;
    struct wl_pointer *wl_pointer;

    bool req_fullscreen;
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
            xdg_surface_set_window_geometry(sys->xdg_surface, 0, 0, width, height);
            wl_surface_commit(wnd->handle.wl);
            break;
        }

        case VOUT_WINDOW_SET_FULLSCREEN:
        {
            bool fs = va_arg(ap, int);
            msg_Dbg(wnd, "Set fullscreen: %d->%d", sys->req_fullscreen, fs);

            if (sys->req_fullscreen == fs)
                break;

            if (fs)
                xdg_toplevel_set_fullscreen(sys->toplevel, NULL);
            else {
                xdg_toplevel_unset_fullscreen(sys->toplevel);
                xdg_surface_set_window_geometry(sys->xdg_surface, 0, 0, sys->req_width, sys->req_height);
            }
            wl_surface_commit(wnd->handle.wl);
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

//    enum xdg_toplevel_state *p;
//    LOG("%s: %dx%d\n", __func__, w, h);
//    wl_array_for_each(p, states) {
//        LOG("    State: %d\n", *p);
//    }

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
    (void)data;
    (void)xdg_toplevel;
}

static void
xdg_toplevel_configure_bounds_cb(void *data,
                                 struct xdg_toplevel *xdg_toplevel,
                                 int32_t width, int32_t height)
{
    (void)data;
    (void)xdg_toplevel;
    (void)width;
    (void)height;
//    LOG("%s: %dx%d\n", __func__, width, height);
}

static void
xdg_toplevel_wm_capabilities_cb(void *data,
                                struct xdg_toplevel *xdg_toplevel,
                                struct wl_array *capabilities)
{
    (void)data;
    (void)xdg_toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure_cb,
    .close = xdg_toplevel_close_cb,
    .configure_bounds = xdg_toplevel_configure_bounds_cb,
    .wm_capabilities = xdg_toplevel_wm_capabilities_cb,
};

// ---------------------------------------------------------------------------

static void
xdg_surface_configure_cb(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    vout_window_t *wnd = data;
    vout_window_sys_t *const sys = wnd->sys;

    msg_Dbg(wnd, "new configuration: (serial: %"PRIu32", %dx%d)", serial, sys->conf_width, sys->conf_height);

    /* Zero width or zero height means client (we) should choose.
     * DO NOT REPORT those values to video output... */
    if (sys->conf_width != 0 && sys->conf_height != 0)
        vout_window_ReportSize(wnd, sys->conf_width, sys->conf_height);

    /* TODO: report fullscreen state, not implemented in VLC */
    xdg_surface_ack_configure(xdg_surface, serial);
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

/**
 * enter event
 *
 * Notification that this seat's pointer is focused on a certain
 * surface.
 *
 * When a seat's focus enters a surface, the pointer image is
 * undefined and a client should respond to this event by setting
 * an appropriate pointer image with the set_cursor request.
 * @param serial serial number of the enter event
 * @param surface surface entered by the pointer
 * @param surface_x surface-local x coordinate
 * @param surface_y surface-local y coordinate
 */
static void pointer_enter_cb(void *data,
          struct wl_pointer *wl_pointer,
          uint32_t serial,
          struct wl_surface *surface,
          wl_fixed_t surface_x,
          wl_fixed_t surface_y)
{
}

/**
 * leave event
 *
 * Notification that this seat's pointer is no longer focused on
 * a certain surface.
 *
 * The leave notification is sent before the enter notification for
 * the new focus.
 * @param serial serial number of the leave event
 * @param surface surface left by the pointer
 */
static void pointer_leave_cb(void *data,
          struct wl_pointer *wl_pointer,
          uint32_t serial,
          struct wl_surface *surface)
{
}

/**
 * pointer motion event
 *
 * Notification of pointer location change. The arguments
 * surface_x and surface_y are the location relative to the focused
 * surface.
 * @param time timestamp with millisecond granularity
 * @param surface_x surface-local x coordinate
 * @param surface_y surface-local y coordinate
 */
static void pointer_motion_cb(void *data,
           struct wl_pointer *wl_pointer,
           uint32_t time,
           wl_fixed_t surface_x,
           wl_fixed_t surface_y)
{
}

/**
 * pointer button event
 *
 * Mouse button click and release notifications.
 *
 * The location of the click is given by the last motion or enter
 * event. The time argument is a timestamp with millisecond
 * granularity, with an undefined base.
 *
 * The button is a button code as defined in the Linux kernel's
 * linux/input-event-codes.h header file, e.g. BTN_LEFT.
 *
 * Any 16-bit button code value is reserved for future additions to
 * the kernel's event code list. All other button codes above
 * 0xFFFF are currently undefined but may be used in future
 * versions of this protocol.
 * @param serial serial number of the button event
 * @param time timestamp with millisecond granularity
 * @param button button that produced the event
 * @param state physical state of the button
 */
static void 
pointer_button_cb(void *data,
           struct wl_pointer *wl_pointer,
           uint32_t serial,
           uint32_t time,
           uint32_t button,
           uint32_t state)
{
}

/**
 * axis event
 *
 * Scroll and other axis notifications.
 *
 * For scroll events (vertical and horizontal scroll axes), the
 * value parameter is the length of a vector along the specified
 * axis in a coordinate space identical to those of motion events,
 * representing a relative movement along the specified axis.
 *
 * For devices that support movements non-parallel to axes multiple
 * axis events will be emitted.
 *
 * When applicable, for example for touch pads, the server can
 * choose to emit scroll events where the motion vector is
 * equivalent to a motion event vector.
 *
 * When applicable, a client can transform its content relative to
 * the scroll distance.
 * @param time timestamp with millisecond granularity
 * @param axis axis type
 * @param value length of vector in surface-local coordinate space
 */
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

/**
 * end of a pointer event sequence
 *
 * Indicates the end of a set of events that logically belong
 * together. A client is expected to accumulate the data in all
 * events within the frame before proceeding.
 *
 * All wl_pointer events before a wl_pointer.frame event belong
 * logically together. For example, in a diagonal scroll motion the
 * compositor will send an optional wl_pointer.axis_source event,
 * two wl_pointer.axis events (horizontal and vertical) and finally
 * a wl_pointer.frame event. The client may use this information to
 * calculate a diagonal vector for scrolling.
 *
 * When multiple wl_pointer.axis events occur within the same
 * frame, the motion vector is the combined motion of all events.
 * When a wl_pointer.axis and a wl_pointer.axis_stop event occur
 * within the same frame, this indicates that axis movement in one
 * axis has stopped but continues in the other axis. When multiple
 * wl_pointer.axis_stop events occur within the same frame, this
 * indicates that these axes stopped in the same instance.
 *
 * A wl_pointer.frame event is sent for every logical event group,
 * even if the group only contains a single wl_pointer event.
 * Specifically, a client may get a sequence: motion, frame,
 * button, frame, axis, frame, axis_stop, frame.
 *
 * The wl_pointer.enter and wl_pointer.leave events are logical
 * events generated by the compositor and not the hardware. These
 * events are also grouped by a wl_pointer.frame. When a pointer
 * moves from one surface to another, a compositor should group the
 * wl_pointer.leave event within the same wl_pointer.frame.
 * However, a client must not rely on wl_pointer.leave and
 * wl_pointer.enter being in the same wl_pointer.frame.
 * Compositor-specific policies may require the wl_pointer.leave
 * and wl_pointer.enter event being split across multiple
 * wl_pointer.frame groups.
 * @since 5
 */
static void pointer_frame_cb(void *data,
          struct wl_pointer *wl_pointer)
{
}

/**
 * axis source event
 *
 * Source information for scroll and other axes.
 *
 * This event does not occur on its own. It is sent before a
 * wl_pointer.frame event and carries the source information for
 * all events within that frame.
 *
 * The source specifies how this event was generated. If the source
 * is wl_pointer.axis_source.finger, a wl_pointer.axis_stop event
 * will be sent when the user lifts the finger off the device.
 *
 * If the source is wl_pointer.axis_source.wheel,
 * wl_pointer.axis_source.wheel_tilt or
 * wl_pointer.axis_source.continuous, a wl_pointer.axis_stop event
 * may or may not be sent. Whether a compositor sends an axis_stop
 * event for these sources is hardware-specific and
 * implementation-dependent; clients must not rely on receiving an
 * axis_stop event for these scroll sources and should treat scroll
 * sequences from these scroll sources as unterminated by default.
 *
 * This event is optional. If the source is unknown for a
 * particular axis event sequence, no event is sent. Only one
 * wl_pointer.axis_source event is permitted per frame.
 *
 * The order of wl_pointer.axis_discrete and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis_source source of the axis event
 * @since 5
 */
static void
pointer_axis_source_cb(void *data,
            struct wl_pointer *wl_pointer,
            uint32_t axis_source)
{
    VLC_UNUSED(data);
    VLC_UNUSED(wl_pointer);
    VLC_UNUSED(axis_source);
}

/**
 * axis stop event
 *
 * Stop notification for scroll and other axes.
 *
 * For some wl_pointer.axis_source types, a wl_pointer.axis_stop
 * event is sent to notify a client that the axis sequence has
 * terminated. This enables the client to implement kinetic
 * scrolling. See the wl_pointer.axis_source documentation for
 * information on when this event may be generated.
 *
 * Any wl_pointer.axis events with the same axis_source after this
 * event should be considered as the start of a new axis motion.
 *
 * The timestamp is to be interpreted identical to the timestamp in
 * the wl_pointer.axis event. The timestamp value may be the same
 * as a preceding wl_pointer.axis event.
 * @param time timestamp with millisecond granularity
 * @param axis the axis stopped with this event
 * @since 5
 */
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

/**
 * axis click event
 *
 * Discrete step information for scroll and other axes.
 *
 * This event carries the axis value of the wl_pointer.axis event
 * in discrete steps (e.g. mouse wheel clicks).
 *
 * This event is deprecated with wl_pointer version 8 - this event
 * is not sent to clients supporting version 8 or later.
 *
 * This event does not occur on its own, it is coupled with a
 * wl_pointer.axis event that represents this axis value on a
 * continuous scale. The protocol guarantees that each
 * axis_discrete event is always followed by exactly one axis event
 * with the same axis number within the same wl_pointer.frame. Note
 * that the protocol allows for other events to occur between the
 * axis_discrete and its coupled axis event, including other
 * axis_discrete or axis events. A wl_pointer.frame must not
 * contain more than one axis_discrete event per axis type.
 *
 * This event is optional; continuous scrolling devices like
 * two-finger scrolling on touchpads do not have discrete steps and
 * do not generate this event.
 *
 * The discrete value carries the directional information. e.g. a
 * value of -2 is two steps towards the negative direction of this
 * axis.
 *
 * The axis number is identical to the axis number in the
 * associated axis event.
 *
 * The order of wl_pointer.axis_discrete and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis axis type
 * @param discrete number of steps
 * @since 5
 */
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

/**
 * axis high-resolution scroll event
 *
 * Discrete high-resolution scroll information.
 *
 * This event carries high-resolution wheel scroll information,
 * with each multiple of 120 representing one logical scroll step
 * (a wheel detent). For example, an axis_value120 of 30 is one
 * quarter of a logical scroll step in the positive direction, a
 * value120 of -240 are two logical scroll steps in the negative
 * direction within the same hardware event. Clients that rely on
 * discrete scrolling should accumulate the value120 to multiples
 * of 120 before processing the event.
 *
 * The value120 must not be zero.
 *
 * This event replaces the wl_pointer.axis_discrete event in
 * clients supporting wl_pointer version 8 or later.
 *
 * Where a wl_pointer.axis_source event occurs in the same
 * wl_pointer.frame, the axis source applies to this event.
 *
 * The order of wl_pointer.axis_value120 and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis axis type
 * @param value120 scroll distance as fraction of 120
 * @since 8
 */
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

/**
 * axis relative physical direction event
 *
 * Relative directional information of the entity causing the
 * axis motion.
 *
 * For a wl_pointer.axis event, the
 * wl_pointer.axis_relative_direction event specifies the movement
 * direction of the entity causing the wl_pointer.axis event. For
 * example: - if a user's fingers on a touchpad move down and this
 * causes a wl_pointer.axis vertical_scroll down event, the
 * physical direction is 'identical' - if a user's fingers on a
 * touchpad move down and this causes a wl_pointer.axis
 * vertical_scroll up scroll up event ('natural scrolling'), the
 * physical direction is 'inverted'.
 *
 * A client may use this information to adjust scroll motion of
 * components. Specifically, enabling natural scrolling causes the
 * content to change direction compared to traditional scrolling.
 * Some widgets like volume control sliders should usually match
 * the physical direction regardless of whether natural scrolling
 * is active. This event enables clients to match the scroll
 * direction of a widget to the physical direction.
 *
 * This event does not occur on its own, it is coupled with a
 * wl_pointer.axis event that represents this axis value. The
 * protocol guarantees that each axis_relative_direction event is
 * always followed by exactly one axis event with the same axis
 * number within the same wl_pointer.frame. Note that the protocol
 * allows for other events to occur between the
 * axis_relative_direction and its coupled axis event.
 *
 * The axis number is identical to the axis number in the
 * associated axis event.
 *
 * The order of wl_pointer.axis_relative_direction,
 * wl_pointer.axis_discrete and wl_pointer.axis_source is not
 * guaranteed.
 * @param axis axis type
 * @param direction physical direction relative to axis motion
 * @since 9
 */
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
	.axis_value120 = pointer_axis_value120_cb,
	.axis_relative_direction = pointer_axis_relative_direction_cb,
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
        sys->wl_pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(sys->wl_pointer, &pointer_cbs, wnd);
    }
    else
    {
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

static void registry_global_cb(void *data, struct wl_registry *registry,
                               uint32_t name, const char *iface, uint32_t vers)
{
    vout_window_t *wnd = data;
    vout_window_sys_t *sys = wnd->sys;

    msg_Dbg(wnd, "global %3"PRIu32": %s version %"PRIu32, name, iface, vers);

    if (!strcmp(iface, "wl_compositor"))
        sys->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           (vers < 2) ? vers : 2);
    else
    if (!strcmp(iface, xdg_wm_base_interface.name))
        sys->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    if (!strcmp(iface, wl_seat_interface.name))
    {
        sys->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 2);
        wl_seat_add_listener(sys->wl_seat, &seat_cbs, wnd);
    } else
    if (!strcmp(iface, "org_kde_kwin_server_decoration_manager"))
        sys->deco_manager = wl_registry_bind(registry, name,
                         &org_kde_kwin_server_decoration_manager_interface, 1);
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

/**
 * Creates a Wayland shell surface.
 */
static int Open(vout_window_t *wnd, const vout_window_cfg_t *cfg)
{
    msg_Info(wnd, "<<< WL XDG, type=%d", cfg->type);
    if (cfg->type != VOUT_WINDOW_TYPE_INVALID
     && cfg->type != VOUT_WINDOW_TYPE_WAYLAND)
        return VLC_EGENERIC;

    vout_window_sys_t *sys = calloc(1, sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    wnd->sys = sys;

    /* Connect to the display server */
    char *dpy_name = var_InheritString(wnd, "wl-display");
    struct wl_display *display = wl_display_connect(dpy_name);

    free(dpy_name);

    if (display == NULL)
    {
        msg_Info(wnd, ">>> WL XDG No display");
        free(sys);
        return VLC_EGENERIC;
    }

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

    xdg_wm_base_add_listener(sys->shell, &xdg_shell_cbs, NULL);

    /* Create a surface */
    struct wl_surface *surface = wl_compositor_create_surface(sys->compositor);
    if (surface == NULL)
        goto error;

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

    xdg_surface_set_window_geometry(xdg_surface, 0, 0,
                                    cfg->width, cfg->height);
    vout_window_ReportSize(wnd, cfg->width, cfg->height);

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

    wnd->type = VOUT_WINDOW_TYPE_WAYLAND;
    wnd->handle.wl = surface;
    wnd->display.wl = display;
    wnd->control = Control;

    vout_window_SetFullScreen(wnd, cfg->is_fullscreen);

    if (vlc_clone(&sys->thread, Thread, wnd, VLC_THREAD_PRIORITY_LOW))
        goto error;

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
    if (sys->compositor != NULL)
        wl_compositor_destroy(sys->compositor);
    wl_display_disconnect(display);
    free(sys);
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
    xdg_toplevel_destroy(sys->toplevel);
    xdg_surface_destroy(sys->xdg_surface);
    wl_surface_destroy(wnd->handle.wl);
    xdg_wm_base_destroy(sys->shell);
    pointer_destroy(&sys->wl_pointer);
    wl_seat_destroy(sys->wl_seat);
    wl_compositor_destroy(sys->compositor);
    wl_display_disconnect(wnd->display.wl);
    free(sys);
}


#define DISPLAY_TEXT N_("Wayland display")
#define DISPLAY_LONGTEXT N_( \
    "Video will be rendered with this Wayland display. " \
    "If empty, the default display will be used.")

vlc_module_begin()
    set_shortname(N_("WL XDG shell"))
    set_description(N_("Wayland XDG shell surface"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout window", 21)
    set_callbacks(Open, Close)

    add_string("wl-display", NULL, DISPLAY_TEXT, DISPLAY_LONGTEXT, true)
vlc_module_end()
