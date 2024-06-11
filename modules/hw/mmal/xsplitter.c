#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>
#include <vlc_vout_display.h>
#include <vlc_modules.h>

#define TRACE_ALL 0

// If set will reduce requested size on X
// A good idea if we have h/w scaler before X
// as it limits the work needed by GL but a disaster if we don't
#define LIMIT_X_PELS 0

#define VOUT_DISPLAY_CHANGE_MMAL_BASE 1024
#define VOUT_DISPLAY_CHANGE_MMAL_HIDE (VOUT_DISPLAY_CHANGE_MMAL_BASE + 0)

struct mmal_x11_sys_s;

typedef struct display_desc_s
{
    vout_display_t * vout;
    unsigned int max_pels;
    void (* on_swap_away)(vout_display_t *const vd, struct mmal_x11_sys_s *const sys, struct display_desc_s * const x_desc);
} display_desc_t;

typedef struct mmal_x11_sys_s
{
    bool drm_fail;  // We tried DRM but it didn't work
    display_desc_t * cur_desc;
    display_desc_t mmal_desc;
    display_desc_t x_desc;
    display_desc_t drm_desc;
    uint32_t changed;
    vlc_fourcc_t subpicture_chromas[16];
} mmal_x11_sys_t;

#define MAX_GL_PELS (1920*1080)
#define MAX_MMAL_PELS (4096*4096)  // Should never be hit
#define MAX_DRM_PELS (4096*4096)  // Should never be hit

static inline char drmu_log_safechar(int c)
{
    return (c < ' ' || c >=0x7f) ? '?' : c;
}

static inline const char * str_fourcc(char buf[5], uint32_t fcc)
{
    if (fcc == 0)
        return "----";
    buf[0] = drmu_log_safechar((fcc >> 0) & 0xff);
    buf[1] = drmu_log_safechar((fcc >> 8) & 0xff);
    buf[2] = drmu_log_safechar((fcc >> 16) & 0xff);
    buf[3] = drmu_log_safechar((fcc >> 24) & 0xff);
    buf[4] = 0;
    return buf;
}

#if 0
// Gen prog for the following table
// Not done inline in case we end up pulling in FP libs we don't want
#include <math.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    unsigned int i;
    for (i = 0; i != 64; ++i)
    {
        printf(" [%2u]=%5u,", i, (unsigned int)(0.5 + (1/sqrt((i + 5)/4.0) * 65536.0)));
        if (i % 4 == 3)
            printf("\n");
    }
}
#endif

#if LIMIT_X_PELS
static const uint16_t sqrt_tab[64] = {
    [ 0]=58617, [ 1]=53510, [ 2]=49541, [ 3]=46341,
    [ 4]=43691, [ 5]=41449, [ 6]=39520, [ 7]=37837,
    [ 8]=36353, [ 9]=35030, [10]=33843, [11]=32768,
    [12]=31790, [13]=30894, [14]=30070, [15]=29309,
    [16]=28602, [17]=27945, [18]=27330, [19]=26755,
    [20]=26214, [21]=25705, [22]=25225, [23]=24770,
    [24]=24339, [25]=23930, [26]=23541, [27]=23170,
    [28]=22817, [29]=22479, [30]=22155, [31]=21845,
    [32]=21548, [33]=21263, [34]=20988, [35]=20724,
    [36]=20470, [37]=20225, [38]=19988, [39]=19760,
    [40]=19539, [41]=19326, [42]=19119, [43]=18919,
    [44]=18725, [45]=18536, [46]=18354, [47]=18176,
    [48]=18004, [49]=17837, [50]=17674, [51]=17515,
    [52]=17361, [53]=17211, [54]=17064, [55]=16921,
    [56]=16782, [57]=16646, [58]=16514, [59]=16384,
    [60]=16257, [61]=16134, [62]=16013, [63]=15895
};
#define SQRT_MAX (sizeof(sqrt_tab)/sizeof(sqrt_tab[0]) - 1)
#endif

static bool cpy_fmt_limit_size(const display_desc_t * const dd,
                           video_format_t * const dst,
                           const video_format_t * const src)
{
#if !LIMIT_X_PELS
    VLC_UNUSED(dd);
    *dst = *src;
    return false;
#else
    const unsigned int src_pel = src->i_visible_width * src->i_visible_height;

    *dst = *src;

    if (src_pel <= dd->max_pels)
        return false;

    // scaling factor sqrt(max_pel/cur_pel)
    // sqrt done by lookup & 16 bit fixed-point maths - not exactly accurate but
    // easily good enough & avoids floating point (which may be slow)
    // src_pel > max_pel so n >= 0
    // Rounding should be such that exact sqrts work and everything else rounds
    // down
    unsigned int n = ((src_pel * 4 - 1) / dd->max_pels) - 4;
    unsigned int scale = sqrt_tab[n >= SQRT_MAX ? SQRT_MAX : n];

    // Rescale width - rounding up to 16
    unsigned int width = ((src->i_visible_width * scale + (16 << 16) - 1) >> 16) & ~15;
    // Rescale height based on new width
    unsigned int height = (src->i_visible_height * width + src->i_visible_width/2) / src->i_visible_width;

//    fprintf(stderr, "%dx%d -> %dx%d\n", src->i_visible_width, src->i_visible_height, width, height);

    dst->i_width          = width;
    dst->i_visible_width  = width;
    dst->i_height         = height;
    dst->i_visible_height = height;
    return true;
#endif
}

static void unload_display_module(vout_display_t * const x_vout)
{
    if (x_vout != NULL) {
       if (x_vout->module != NULL) {
            module_unneed(x_vout, x_vout->module);
        }
        vlc_object_release(x_vout);
    }
}

static void stop_drm(vout_display_t *const vd, mmal_x11_sys_t *const sys)
{
    VLC_UNUSED(vd);

    unload_display_module(sys->drm_desc.vout);
    sys->drm_desc.vout = NULL;
}

static void CloseMmalX11(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;

    msg_Dbg(vd, "<<< %s", __func__);

    if (sys == NULL)
        return;

    unload_display_module(sys->x_desc.vout);

    unload_display_module(sys->mmal_desc.vout);

    stop_drm(vd, sys);

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


static int load_display_module(vout_display_t * const vd,
                                display_desc_t * const dd,
                                const char * const cap,
                                const char * const module_name)
{
    vout_display_t * const x_vout = vlc_object_create(vd, sizeof(*x_vout));

    dd->vout = NULL;
    if (!x_vout)
        return -1;

    x_vout->owner.sys = vd;
    x_vout->owner.event = mmal_x11_event;
    x_vout->owner.window_new = mmal_x11_window_new;
    x_vout->owner.window_del = mmal_x11_window_del;

    x_vout->cfg    = vd->cfg;
    x_vout->info   = vd->info;
    cpy_fmt_limit_size(dd, &x_vout->source, &vd->source);
    cpy_fmt_limit_size(dd, &x_vout->fmt,    &vd->fmt);

    if ((x_vout->module = module_need(x_vout, cap, module_name, true)) == NULL)
    {
        msg_Dbg(vd, "Failed to open Xsplitter:%s module", module_name);
        goto fail;
    }

    msg_Dbg(vd, "R/G/B: %08x/%08x/%08x", x_vout->fmt.i_rmask, x_vout->fmt.i_gmask, x_vout->fmt.i_bmask);

    dd->vout = x_vout;
    return 0;

fail:
    vlc_object_release(x_vout);
    return -1;
}

static int start_drm(vout_display_t *const vd, mmal_x11_sys_t *const sys)
{
    if (sys->drm_desc.vout != NULL)
        return 0;
    msg_Info(vd, "Try drm");
    return load_display_module(vd, &sys->drm_desc, "vout display", "drm_vout");
}


/* Return a pointer over the current picture_pool_t* (mandatory).
 *
 * For performance reasons, it is best to provide at least count
 * pictures but it is not mandatory.
 * You can return NULL when you cannot/do not want to allocate
 * pictures.
 * The vout display module keeps the ownership of the pool and can
 * destroy it only when closing or on invalid pictures control.
 *
 * If the X pool doesn't have pictures invalid then it isn't safe
 * to swap pools so always use that one (MMAL & DRM can cope with
 * most stuff)
 */
static picture_pool_t * mmal_x11_pool(vout_display_t * vd, unsigned count)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = vd->info.has_pictures_invalid ? sys->cur_desc->vout : sys->x_desc.vout;
#if TRACE_ALL
    char buf0[5];
    char buf1[5];
    msg_Dbg(vd, "<<< %s (count=%d) %s:%dx%d->%s:%dx%d", __func__, count,
            str_fourcc(buf0, vd->fmt.i_chroma),
            vd->fmt.i_width, vd->fmt.i_height,
            str_fourcc(buf1, x_vd->fmt.i_chroma),
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
    vout_display_t * const x_vd = sys->cur_desc->vout;
#if TRACE_ALL
    char buf0[5];
    msg_Dbg(vd, "<<< %s: fmt=%s, %dx%d", __func__, str_fourcc(buf0, pic->format.i_chroma), pic->format.i_width, pic->format.i_height);
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
    vout_display_t * const x_vd = sys->cur_desc->vout;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: fmt: %dx%d/%dx%d, pic:%dx%d, pts=%lld", __func__, vd->fmt.i_width, vd->fmt.i_height, x_vd->fmt.i_width, x_vd->fmt.i_height, pic->format.i_width, pic->format.i_height, (long long)pic->date);
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


static int vout_display_Control(const display_desc_t * const dd, int query, ...)
{
    va_list args;
    int result;

    va_start(args, query);
    result = dd->vout->control(dd->vout, query, args);
    va_end(args);

    return result;
}

static display_desc_t* wanted_display(vout_display_t *const vd, mmal_x11_sys_t *const sys)
{
    if (sys->x_desc.vout != NULL && !var_InheritBool(vd, "fullscreen"))
        return &sys->x_desc;

    // Full screen or no X

    if (sys->mmal_desc.vout != NULL)
        return &sys->mmal_desc;

    if (!sys->drm_fail) {
        if (start_drm(vd, sys) == 0)
            return &sys->drm_desc;
        msg_Info(vd, "Drm no go");
        sys->drm_fail = true;  // Never try again
    }

    return sys->x_desc.vout != NULL ? &sys->x_desc : NULL;
}

static inline int
up_rv(const int a, const int b)
{
    return a != 0 ? a : b;
}

static int
reset_pictures(vout_display_t * const vd, const display_desc_t * const desc)
{
    int rv = 0;
    VLC_UNUSED(vd);
    if (desc->vout)
    {
        // If the display doesn't have has_pictures_invalid then it doesn't
        // expect RESET_PICTURES
        if (desc->vout->info.has_pictures_invalid)
            vout_display_Control(desc, VOUT_DISPLAY_RESET_PICTURES);
    }
    return rv;
}

static int
replay_controls(vout_display_t * const vd, const display_desc_t * const desc, const int32_t changed)
{
    if ((changed & (1 << VOUT_DISPLAY_CHANGE_DISPLAY_FILLED)) != 0)
        vout_display_Control(desc, VOUT_DISPLAY_CHANGE_DISPLAY_FILLED, vd->cfg);
    if ((changed & (1 << VOUT_DISPLAY_CHANGE_ZOOM)) != 0)
        vout_display_Control(desc, VOUT_DISPLAY_CHANGE_ZOOM, vd->cfg);
    if ((changed & ((1 << VOUT_DISPLAY_CHANGE_SOURCE_CROP) |
                    (1 << VOUT_DISPLAY_CHANGE_SOURCE_ASPECT))) != 0)
        cpy_fmt_limit_size(desc, &desc->vout->source, &vd->source);
    if ((changed & (1 << VOUT_DISPLAY_CHANGE_SOURCE_ASPECT)) != 0)
        vout_display_Control(desc, VOUT_DISPLAY_CHANGE_SOURCE_ASPECT);
    if ((changed & (1 << VOUT_DISPLAY_CHANGE_SOURCE_CROP)) != 0)
        vout_display_Control(desc, VOUT_DISPLAY_CHANGE_SOURCE_CROP);
    if ((changed & (1 << VOUT_DISPLAY_CHANGE_VIEWPOINT)) != 0)
        vout_display_Control(desc, VOUT_DISPLAY_CHANGE_VIEWPOINT, vd->cfg);
    return 0;
}

static void swap_away_null(vout_display_t *const vd, mmal_x11_sys_t *const sys, display_desc_t * const x_desc)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(sys);
    VLC_UNUSED(x_desc);
}

static void swap_away_mmal(vout_display_t *const vd, mmal_x11_sys_t *const sys, display_desc_t * const x_desc)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(sys);
    vout_display_Control(x_desc, VOUT_DISPLAY_CHANGE_MMAL_HIDE);
}

static void swap_away_drm(vout_display_t *const vd, mmal_x11_sys_t *const sys, display_desc_t * const x_desc)
{
    VLC_UNUSED(x_desc);
    stop_drm(vd, sys);
}


/* Control on the module (mandatory) */
static int mmal_x11_control(vout_display_t * vd, int ctl, va_list va)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    display_desc_t *x_desc = sys->cur_desc;
    int rv;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s[%s] (ctl=%d)", __func__, x_desc->vout->obj.object_type, ctl);
#endif
    // Remember what we've told this vd - unwanted ctls ignored on replay
    if (ctl >= 0 && ctl <= 31)
        sys->changed |= (1 << ctl);

    switch (ctl) {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        {
            const vout_display_cfg_t * const cfg = va_arg(va, const vout_display_cfg_t *);
            display_desc_t * const new_desc = wanted_display(vd, sys);
            const bool swap_vout = (new_desc != sys->cur_desc);

            msg_Dbg(vd, "Change size: %d, %d: fs=%d",
                    cfg->display.width, cfg->display.height, var_InheritBool(vd, "fullscreen"));

            // Repeat any control calls that we sent to the previous vd
            if (swap_vout && sys->changed != 0) {
                const uint32_t changed = sys->changed;
                sys->changed = 0;
                replay_controls(vd, new_desc, changed);
            }

            if (swap_vout) {
                vout_display_SendEventPicturesInvalid(vd);
                x_desc->on_swap_away(vd, sys, x_desc);
            }

            rv = vout_display_Control(new_desc, ctl, cfg);
            if (rv == VLC_SUCCESS) {
                vd->fmt       = new_desc->vout->fmt;
                sys->cur_desc = new_desc;
                // Strictly this is illegal but subpic chromas are consulted
                // on every render so it is in fact safe.
                vd->info.subpicture_chromas = sys->cur_desc->vout->info.subpicture_chromas;
            }

            break;
        }

        case VOUT_DISPLAY_RESET_PICTURES:
            {
                char dbuf0[5], dbuf1[5], dbuf2[5];
                msg_Dbg(vd, "<<< %s: Pic reset: fmt: %s,%dx%d<-%s,%dx%d, source: %s,%dx%d/%dx%d", __func__,
                        str_fourcc(dbuf0, vd->fmt.i_chroma), vd->fmt.i_width, vd->fmt.i_height,
                        str_fourcc(dbuf1, x_desc->vout->fmt.i_chroma), x_desc->vout->fmt.i_width, x_desc->vout->fmt.i_height,
                        str_fourcc(dbuf2, vd->source.i_chroma), vd->source.i_width, vd->source.i_height, x_desc->vout->source.i_width,
                        x_desc->vout->source.i_height);
            }
            rv = reset_pictures(vd, &sys->x_desc);
            rv = up_rv(rv, reset_pictures(vd, &sys->mmal_desc));
            rv = up_rv(rv, reset_pictures(vd, &sys->drm_desc));

            vd->fmt = x_desc->vout->fmt;
            break;

        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
            cpy_fmt_limit_size(x_desc, &x_desc->vout->source, &vd->source);

            /* FALLTHRU */
        default:
            rv = x_desc->vout->control(x_desc->vout, ctl, va);
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
    vout_display_t * const x_vd = sys->cur_desc->vout;
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
        .has_pictures_invalid = false,
        .subpicture_chromas = NULL
    };

    {
        char dbuf0[5];
        msg_Dbg(vd, ">>> %s: %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d", __func__,
                str_fourcc(dbuf0, vd->fmt.i_chroma),
                vd->fmt.i_width,         vd->fmt.i_height,
                vd->fmt.i_x_offset,      vd->fmt.i_y_offset,
                vd->fmt.i_visible_width, vd->fmt.i_visible_height,
                vd->fmt.i_sar_num,       vd->fmt.i_sar_den);
    }

    sys->x_desc.max_pels = MAX_GL_PELS;
    sys->x_desc.on_swap_away = swap_away_null;
    sys->mmal_desc.max_pels = MAX_MMAL_PELS;
    sys->mmal_desc.on_swap_away = swap_away_mmal;
    sys->drm_desc.max_pels = MAX_DRM_PELS;
    sys->drm_desc.on_swap_away = swap_away_drm;

    if (load_display_module(vd, &sys->x_desc, "vout display", "opengles2") == 0)
    {
        msg_Dbg(vd, "Opengles2 output found");
    }
    else if (load_display_module(vd, &sys->x_desc, "vout display", "xcb_x11") == 0)
    {
        sys->x_desc.max_pels = MAX_MMAL_PELS;
        msg_Dbg(vd, "X11 XCB output found");
    }
    else
    {
        msg_Dbg(vd, "No X output found");
        goto fail;
    }

    if ((load_display_module(vd, &sys->mmal_desc, "vout display", "mmal_vout")) == 0)
        msg_Dbg(vd, "MMAL output found");

    vd->pool = mmal_x11_pool;
    vd->prepare = mmal_x11_prepare;
    vd->display = mmal_x11_display;
    vd->control = mmal_x11_control;
#if DO_MANAGE
    vd->manage = mmal_x11_manage;
#endif

    sys->cur_desc = wanted_display(vd, sys);
    if (sys->cur_desc == NULL) {
        char dbuf0[5], dbuf1[5];
        msg_Warn(vd, "No valid output found for vout (%s/%s)", str_fourcc(dbuf0, vd->fmt.i_chroma), str_fourcc(dbuf1, vd->source.i_chroma));
        goto fail;
    }

    if (sys->mmal_desc.vout == NULL) {
        vd->info = sys->cur_desc->vout->info;
    }
    else {
        // We have both - construct a combination
        vd->info = (vout_display_info_t){
            .is_slow              = false,
            .has_double_click     = sys->mmal_desc.vout->info.has_double_click || sys->x_desc.vout->info.has_double_click,
            .needs_hide_mouse     = sys->mmal_desc.vout->info.needs_hide_mouse || sys->x_desc.vout->info.needs_hide_mouse,
            .has_pictures_invalid = true,
        };
        // Construct intersection of subpicture chromas
        // sys calloced so no need to add the terminating zero
        if (sys->mmal_desc.vout->info.subpicture_chromas != NULL && sys->x_desc.vout->info.subpicture_chromas != NULL) {
            unsigned int n = 0;
            // N^2 - fix if we ever care
            for (const vlc_fourcc_t * p1 = sys->mmal_desc.vout->info.subpicture_chromas; *p1 != 0 && n != 15; ++p1) {
                for (const vlc_fourcc_t * p2 = sys->x_desc.vout->info.subpicture_chromas; *p2 != 0; ++p2) {
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
    vd->fmt  = sys->cur_desc->vout->fmt;

#if TRACE_ALL
    {
        char dbuf0[5];
        msg_Dbg(vd, ">>> %s: (%s) %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d", __func__,
                module_get_name(sys->cur_desc->vout->module, false),
                str_fourcc(dbuf0, vd->fmt.i_chroma),
                vd->fmt.i_width,         vd->fmt.i_height,
                vd->fmt.i_x_offset,      vd->fmt.i_y_offset,
                vd->fmt.i_visible_width, vd->fmt.i_visible_height,
                vd->fmt.i_sar_num,       vd->fmt.i_sar_den);
    }
#endif
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

