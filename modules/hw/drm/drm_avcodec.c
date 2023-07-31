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

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include "../../codec/avcodec/va.h"

// Dummy - not expected to be called
static int drm_va_get(vlc_va_t *va, picture_t *pic, uint8_t **data)
{
    VLC_UNUSED(va);
    VLC_UNUSED(pic);
    VLC_UNUSED(data);

    return VLC_SUCCESS;
}

static int Open(vlc_va_t *va, AVCodecContext *avctx, enum PixelFormat pix_fmt,
                const es_format_t *fmt, picture_sys_t *p_sys)
{
    VLC_UNUSED(fmt);
    VLC_UNUSED(p_sys);

    msg_Dbg(va, "%s: pix_fmt=%d", __func__, pix_fmt);

    if (pix_fmt != AV_PIX_FMT_DRM_PRIME)
        return VLC_EGENERIC;

    // This gives us whatever the decode requires + 6 frames that will be
    // alloced by ffmpeg before it blocks (at least for Pi HEVC)
    avctx->extra_hw_frames = 6;

    va->description = "DRM Video Accel";
    va->get = drm_va_get;
    return VLC_SUCCESS;
}

static void Close(vlc_va_t *va, void **hwctx)
{
    VLC_UNUSED(hwctx);

    msg_Dbg(va, "%s", __func__);
}

vlc_module_begin()
    set_description(N_("DRM video decoder"))
    set_capability("hw decoder", 100)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_callbacks(Open, Close)
    add_shortcut("drm_prime")
vlc_module_end()
