/*****************************************************************************
 * avcodec.c: VDPAU decoder for libav
 *****************************************************************************
 * Copyright (C) 2012-2013 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <libavutil/mem.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_atomic.h>
#include "../../codec/avcodec/va.h"

struct vlc_va_sys_t
{
    int dummy;
};

static int Lock(vlc_va_t *va, picture_t *pic, uint8_t **data)
{
    msg_Info(va, "%s", __func__);
    return VLC_SUCCESS;
}

static int Open(vlc_va_t *va, AVCodecContext *avctx, enum PixelFormat pix_fmt,
                const es_format_t *fmt, picture_sys_t *p_sys)
{
    msg_Info(va, "%s: pix_fmt=%d", __func__, pix_fmt);

    (void) fmt;
    (void) p_sys;

    if (pix_fmt != AV_PIX_FMT_DRM_PRIME)
        return VLC_EGENERIC;

    vlc_va_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
       return VLC_ENOMEM;

    va->description = "DRM Video Accel";
    va->get = Lock;
    return VLC_SUCCESS;

error:
    free(sys);
    return VLC_EGENERIC;
}

static void Close(vlc_va_t *va, void **hwctx)
{
    vlc_va_sys_t *sys = va->sys;

    msg_Info(va, "%s", __func__);

    free(sys);
}

vlc_module_begin()
    set_description(N_("DRM video decoder"))
    set_capability("hw decoder", 100)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_callbacks(Open, Close)
    add_shortcut("drm_prime")
vlc_module_end()
