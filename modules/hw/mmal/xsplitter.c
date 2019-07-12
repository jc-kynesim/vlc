#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>
#include <vlc_vout_display.h>
#include <vlc_modules.h>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#include "mmal_picture.h"

#define TRACE_ALL 0

typedef struct mmal_x11_sys_s
{
    bool use_mmal;
    vout_display_t * cur_vout;
    vout_display_t * mmal_vout;
    vout_display_t * x_vout;
    uint32_t changed;
    vlc_fourcc_t subpicture_chromas[16];
} mmal_x11_sys_t;

static void unload_display_module(vout_display_t * const x_vout)
{
    if (x_vout != NULL) {
       if (x_vout->module != NULL) {
            module_unneed(x_vout, x_vout->module);
        }
        vlc_object_release(x_vout);
    }
}

static void CloseMmalX11(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;

    msg_Dbg(vd, "<<< %s", __func__);

    if (sys == NULL)
        return;

    unload_display_module(sys->x_vout);

    unload_display_module(sys->mmal_vout);

    free(sys);

    msg_Dbg(vd, ">>> %s", __func__);
}

static void mmal_x11_event(vout_display_t * x_vd, int cmd, va_list args)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s (cmd=%d)", __func__, cmd);
#endif

    // Do not fall into the display assert if Invalid not supported
    if (cmd == VOUT_DISPLAY_EVENT_PICTURES_INVALID &&
            !vd->info.has_pictures_invalid)
        return;

    vd->owner.event(vd, cmd, args);
}

static vout_window_t * mmal_x11_window_new(vout_display_t * x_vd, unsigned type)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s (type=%d)", __func__, type);
#endif
    return vd->owner.window_new(vd, type);
}

static void mmal_x11_window_del(vout_display_t * x_vd, vout_window_t * win)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    vd->owner.window_del(vd, win);
}


static vout_display_t * load_display_module(vout_display_t * const vd,
                                            const char * const cap, const char * const module_name)
{
    vout_display_t * const x_vout = vlc_object_create(vd, sizeof(*x_vout));

    if (!x_vout)
        return NULL;

    x_vout->owner.sys = vd;
    x_vout->owner.event = mmal_x11_event;
    x_vout->owner.window_new = mmal_x11_window_new;
    x_vout->owner.window_del = mmal_x11_window_del;

    x_vout->cfg    = vd->cfg;
    x_vout->source = vd->source;
    x_vout->info   = vd->info;

    x_vout->fmt = vd->fmt;

    if ((x_vout->module = module_need(x_vout, cap, module_name, true)) == NULL)
    {
        msg_Err(vd, "Failed to open Xsplitter:%s module", module_name);
        goto fail;
    }

    msg_Dbg(vd, "R/G/B: %08x/%08x/%08x", x_vout->fmt.i_rmask, x_vout->fmt.i_gmask, x_vout->fmt.i_bmask);

    return x_vout;

fail:
    vlc_object_release(x_vout);
    return NULL;
}


/* Return a pointer over the current picture_pool_t* (mandatory).
 *
 * For performance reasons, it is best to provide at least count
 * pictures but it is not mandatory.
 * You can return NULL when you cannot/do not want to allocate
 * pictures.
 * The vout display module keeps the ownership of the pool and can
 * destroy it only when closing or on invalid pictures control.
 */
static picture_pool_t * mmal_x11_pool(vout_display_t * vd, unsigned count)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    char buf0[5];
    msg_Dbg(vd, "<<< %s (count=%d) %s:%dx%d->%s:%dx%d", __func__, count,
            str_fourcc(buf0, vd->fmt.i_chroma),
            vd->fmt.i_width, vd->fmt.i_height,
            str_fourcc(buf0, x_vd->fmt.i_chroma),
            x_vd->fmt.i_width, x_vd->fmt.i_height);
#endif
    picture_pool_t * pool = x_vd->pool(x_vd, count);
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s: %p", __func__, pool);
#endif
    return pool;
}

/* Prepare a picture and an optional subpicture for display (optional).
 *
 * It is called before the next pf_display call to provide as much
 * time as possible to prepare the given picture and the subpicture
 * for display.
 * You are guaranted that pf_display will always be called and using
 * the exact same picture_t and subpicture_t.
 * You cannot change the pixel content of the picture_t or of the
 * subpicture_t.
 */
static void mmal_x11_prepare(vout_display_t * vd, picture_t * pic, subpicture_t * sub)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    if (x_vd->prepare)
        x_vd->prepare(x_vd, pic, sub);
}

/* Display a picture and an optional subpicture (mandatory).
 *
 * The picture and the optional subpicture must be displayed as soon as
 * possible.
 * You cannot change the pixel content of the picture_t or of the
 * subpicture_t.
 *
 * This function gives away the ownership of the picture and of the
 * subpicture, so you must release them as soon as possible.
 */
static void mmal_x11_display(vout_display_t * vd, picture_t * pic, subpicture_t * sub)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
    const bool is_mmal_pic = hw_mmal_pic_is_mmal(pic);

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: fmt: %dx%d/%dx%d, pic:%dx%d, pts=%lld, mmal=%d/%d", __func__, vd->fmt.i_width, vd->fmt.i_height, x_vd->fmt.i_width, x_vd->fmt.i_height, pic->format.i_width, pic->format.i_height, (long long)pic->date,
            is_mmal_pic, sys->use_mmal);
#endif

    if (x_vd->fmt.i_chroma != pic->format.i_chroma ||
        x_vd->fmt.i_width  != pic->format.i_width ||
        x_vd->fmt.i_height != pic->format.i_height)
    {
        msg_Dbg(vd, "%s: Picture dropped", __func__);
        picture_Release(pic);
        if (sub != NULL)
            subpicture_Delete(sub);
        return;
    }

    x_vd->display(x_vd, pic, sub);
}


static int vout_display_Control(vout_display_t *vd, int query, ...)
{
    va_list args;
    int result;

    va_start(args, query);
    result = vd->control(vd, query, args);
    va_end(args);

    return result;
}

static bool want_mmal_vout(vout_display_t * vd, const mmal_x11_sys_t * const sys)
{
    return sys->mmal_vout != NULL && (sys->x_vout == NULL || var_InheritBool(vd, "fullscreen"));
}

/* Control on the module (mandatory) */
static int mmal_x11_control(vout_display_t * vd, int ctl, va_list va)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t *x_vd = sys->cur_vout;
    int rv;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s[%d] (ctl=%d)", __func__, sys->use_mmal, ctl);
#endif
    // Remember what we've told this vd - unwanted ctls ignored on replay
    if (ctl >= 0 && ctl <= 31)
        sys->changed |= (1 << ctl);

    switch (ctl) {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        {
            const vout_display_cfg_t * const cfg = va_arg(va, const vout_display_cfg_t *);
            const bool want_mmal = want_mmal_vout(vd, sys);
            const bool swap_vout = (sys->use_mmal != want_mmal);
            vout_display_t * const new_vd = want_mmal ? sys->mmal_vout : sys->x_vout;

            msg_Dbg(vd, "Change size: %d, %d: mmal_vout=%p, want_mmal=%d, fs=%d",
                    cfg->display.width, cfg->display.height, sys->mmal_vout, want_mmal,
                    var_InheritBool(vd, "fullscreen"));

            if (swap_vout) {
                if (sys->use_mmal) {
                    vout_display_Control(x_vd, VOUT_DISPLAY_CHANGE_MMAL_HIDE);
                }
                vout_display_SendEventPicturesInvalid(vd);
            }

            rv = vout_display_Control(new_vd, ctl, cfg);
            if (rv == VLC_SUCCESS) {
                vd->fmt       = new_vd->fmt;
                sys->cur_vout = new_vd;
                sys->use_mmal = want_mmal;
            }

            // Repeat any control calls that we sent to the previous vd
            if (swap_vout && sys->changed != 0) {
                const uint32_t changed = sys->changed;
                sys->changed = 0;
                if ((changed & (1 << VOUT_DISPLAY_CHANGE_DISPLAY_FILLED)) != 0)
                    vout_display_Control(new_vd, VOUT_DISPLAY_CHANGE_DISPLAY_FILLED, vd->cfg);
                if ((changed & (1 << VOUT_DISPLAY_CHANGE_ZOOM)) != 0)
                    vout_display_Control(new_vd, VOUT_DISPLAY_CHANGE_ZOOM, vd->cfg);
                if ((changed & ((1 << VOUT_DISPLAY_CHANGE_SOURCE_CROP) | (1 << VOUT_DISPLAY_CHANGE_SOURCE_ASPECT))) != 0)
                    new_vd->source = vd->source;
                if ((changed & (1 << VOUT_DISPLAY_CHANGE_SOURCE_ASPECT)) != 0)
                    vout_display_Control(new_vd, VOUT_DISPLAY_CHANGE_SOURCE_ASPECT);
                if ((changed & (1 << VOUT_DISPLAY_CHANGE_SOURCE_CROP)) != 0)
                    vout_display_Control(new_vd, VOUT_DISPLAY_CHANGE_SOURCE_CROP);
                if ((changed & (1 << VOUT_DISPLAY_CHANGE_VIEWPOINT)) != 0)
                    vout_display_Control(new_vd, VOUT_DISPLAY_CHANGE_ZOOM, vd->cfg);
            }

            break;
        }

        case VOUT_DISPLAY_RESET_PICTURES:
            msg_Dbg(vd, "<<< %s: Pic reset: fmt: %dx%d<-%dx%d, source: %dx%d/%dx%d", __func__,
                    vd->fmt.i_width, vd->fmt.i_height, x_vd->fmt.i_width, x_vd->fmt.i_height,
                    vd->source.i_width, vd->source.i_height, x_vd->source.i_width, x_vd->source.i_height);
            // If the display doesn't have has_pictures_invalid then it doesn't
            // expect RESET_PICTURES
            if (sys->x_vout->info.has_pictures_invalid) {
                rv = sys->x_vout->control(sys->x_vout, ctl, va);
            }
            if (sys->mmal_vout && sys->mmal_vout->info.has_pictures_invalid) {
                rv = sys->mmal_vout->control(sys->mmal_vout, ctl, va);
            }
            vd->fmt = x_vd->fmt;
            break;

        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
            x_vd->source = vd->source;
            /* FALLTHRU */
        default:
            rv = x_vd->control(x_vd, ctl, va);
//            vd->fmt  = x_vd->fmt;
            break;
    }
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s (rv=%d)", __func__, rv);
#endif
    return rv;
}

#define DO_MANAGE 0

#if DO_MANAGE
/* Manage pending event (optional) */
static void mmal_x11_manage(vout_display_t * vd)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    x_vd->manage(x_vd);
}
#endif

static int OpenMmalX11(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    mmal_x11_sys_t * const sys = calloc(1, sizeof(*sys));
    int ret = VLC_SUCCESS;

    if (sys == NULL) {
        return VLC_EGENERIC;
    }
    vd->sys = (vout_display_sys_t *)sys;

    vd->info = (vout_display_info_t){
        .is_slow = false,
        .has_double_click = false,
        .needs_hide_mouse = false,
        .has_pictures_invalid = true,
        .subpicture_chromas = NULL
    };

    if ((sys->x_vout = load_display_module(vd, "vout display", "opengles2")) != NULL)
        msg_Dbg(vd, "Opengles2 output found");
    else if ((sys->x_vout = load_display_module(vd, "vout display", "xcb_x11")) != NULL)
        msg_Dbg(vd, "X11 XCB output found");

    if ((sys->mmal_vout = load_display_module(vd, "vout display", "mmal_vout")) != NULL)
        msg_Dbg(vd, "MMAL output found");

    if (sys->mmal_vout == NULL && sys->x_vout == NULL) {
        char dbuf0[5], dbuf1[5];
        msg_Info(vd, "No valid output found for vout (%s/%s)", str_fourcc(dbuf0, vd->fmt.i_chroma), str_fourcc(dbuf1, vd->source.i_chroma));
        goto fail;
    }

    vd->pool = mmal_x11_pool;
    vd->prepare = mmal_x11_prepare;
    vd->display = mmal_x11_display;
    vd->control = mmal_x11_control;
#if DO_MANAGE
    vd->manage = mmal_x11_manage;
#endif

    if (want_mmal_vout(vd, sys)) {
        sys->cur_vout = sys->mmal_vout;
        sys->use_mmal = true;
    }
    else {
        sys->cur_vout = sys->x_vout;
        sys->use_mmal = false;
    }

    if (sys->mmal_vout == NULL || sys->x_vout == NULL) {
        vd->info = sys->cur_vout->info;
        vd->info.has_pictures_invalid = true;  // Should make this unwanted
    }
    else {
        // We have both - construct a combination
        vd->info = (vout_display_info_t){
            .is_slow              = false,
            .has_double_click     = sys->mmal_vout->info.has_double_click || sys->x_vout->info.has_double_click,
            .needs_hide_mouse     = sys->mmal_vout->info.needs_hide_mouse || sys->x_vout->info.needs_hide_mouse,
            .has_pictures_invalid = true,
        };
        // Construct intersection of subpicture chromas
        // sys calloced so no need to add the terminating zero
        if (sys->mmal_vout->info.subpicture_chromas != NULL && sys->x_vout->info.subpicture_chromas != NULL) {
            unsigned int n = 0;
            // N^2 - fix if we ever care
            for (const vlc_fourcc_t * p1 = sys->mmal_vout->info.subpicture_chromas; *p1 != 0 && n != 15; ++p1) {
                for (const vlc_fourcc_t * p2 = sys->x_vout->info.subpicture_chromas; *p2 != 0; ++p2) {
                    if (*p1 == *p2) {
                        sys->subpicture_chromas[n++] = *p1;
                        break;
                    }
                }
            }
            if (n != 0)
                vd->info.subpicture_chromas = sys->subpicture_chromas;
        }
    }
    vd->fmt  = sys->cur_vout->fmt;

    return VLC_SUCCESS;

fail:
    CloseMmalX11(VLC_OBJECT(vd));
    return ret == VLC_SUCCESS ? VLC_EGENERIC : ret;
}




vlc_module_begin()
    set_shortname(N_("MMAL x11 splitter"))
    set_description(N_("MMAL x11 splitter for Raspberry Pi"))
    set_capability("vout display", 300)  // Between GLES & GL
    add_shortcut("mmal_x11")
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VOUT )
    set_callbacks(OpenMmalX11, CloseMmalX11)
vlc_module_end()

