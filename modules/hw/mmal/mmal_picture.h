/*****************************************************************************
 * mmal_picture.h: Shared header for MMAL pictures
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Julian Scheel <julian@jusst.de>
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

#ifndef VLC_MMAL_MMAL_PICTURE_H_
#define VLC_MMAL_MMAL_PICTURE_H_

#include <stdatomic.h>

#include <vlc_common.h>
#include <interface/mmal/mmal.h>

/* Think twice before changing this. Incorrect values cause havoc. */
#define NUM_ACTUAL_OPAQUE_BUFFERS 30

#ifndef VLC_TICK_INVALID
#define VLC_TICK_INVALID VLC_TS_INVALID
#define VLC_VER_3 1
#else
#define VLC_VER_3 0
#endif

typedef struct mmal_port_pool_ref_s
{
    atomic_uint refs;
    MMAL_POOL_T * pool;
    MMAL_PORT_T * port;
} hw_mmal_port_pool_ref_t;

typedef struct pic_ctx_subpic_s {
    picture_t * subpic;
    int x, y;
    int alpha;
} pic_ctx_subpic_t;


#define CTX_BUFS_MAX 4

typedef struct pic_ctx_mmal_s {
    picture_context_t cmn;  // PARENT: Common els at start

    MMAL_FOURCC_T fmt;

    unsigned int buf_count;
    MMAL_BUFFER_HEADER_T * bufs[CTX_BUFS_MAX];

#if 0
    MMAL_BUFFER_HEADER_T * buf;
    hw_mmal_port_pool_ref_t * ppr;

    MMAL_BUFFER_HEADER_T * sub_bufs;
    MMAL_BUFFER_HEADER_T * sub_tail;

    vlc_object_t * obj;
#endif
} pic_ctx_mmal_t;

const char * str_fourcc(char * const buf, const unsigned int fcc);

MMAL_FOURCC_T vlc_to_mmal_video_fourcc(const video_frame_format_t * const vf_vlc);
MMAL_FOURCC_T vlc_to_mmal_color_space(const video_color_space_t vlc_cs);
void vlc_to_mmal_video_fmt(MMAL_ES_FORMAT_T *const es_fmt, const video_frame_format_t * const vf_vlc);

hw_mmal_port_pool_ref_t * hw_mmal_port_pool_ref_create(MMAL_PORT_T * const port,
   const unsigned int headers, const uint32_t payload_size);
void hw_mmal_port_pool_ref_release(hw_mmal_port_pool_ref_t * const ppr, const bool in_cb);
bool hw_mmal_port_pool_ref_recycle(hw_mmal_port_pool_ref_t * const ppr, MMAL_BUFFER_HEADER_T * const buf);
MMAL_STATUS_T hw_mmal_port_pool_ref_fill(hw_mmal_port_pool_ref_t * const ppr);
static inline void hw_mmal_port_pool_ref_acquire(hw_mmal_port_pool_ref_t * const ppr)
{
    atomic_fetch_add(&ppr->refs, 1);
}
MMAL_STATUS_T hw_mmal_opaque_output(vlc_object_t * const obj,
                                    hw_mmal_port_pool_ref_t ** pppr,
                                    MMAL_PORT_T * const port,
                                    const unsigned int extra_buffers, MMAL_PORT_BH_CB_T callback);

static inline int hw_mmal_pic_has_sub_bufs(picture_t * const pic)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic->context;
    return ctx->buf_count > 1;
}

static inline void hw_mmal_pic_sub_buf_add(picture_t * const pic, MMAL_BUFFER_HEADER_T * const sub)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic->context;

    if (ctx->buf_count >= CTX_BUFS_MAX) {
        mmal_buffer_header_release(sub);
        return;
    }

    ctx->bufs[ctx->buf_count++] = sub;
}

static inline MMAL_BUFFER_HEADER_T * hw_mmal_pic_sub_buf_get(picture_t * const pic, const unsigned int n)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic->context;

    return n + 1 > ctx->buf_count ? NULL : ctx->bufs[n + 1];
}

static inline bool hw_mmal_pic_is_mmal(const picture_t * const pic)
{
    return pic->format.i_chroma == VLC_CODEC_MMAL_OPAQUE ||
        pic->format.i_chroma == VLC_CODEC_MMAL_ZC_SAND8 ||
        pic->format.i_chroma == VLC_CODEC_MMAL_ZC_SAND10 ||
        pic->format.i_chroma == VLC_CODEC_MMAL_ZC_I420 ||
        pic->format.i_chroma == VLC_CODEC_MMAL_ZC_RGB32;
}

static inline MMAL_FOURCC_T hw_mmal_pic_format(const picture_t *const pic)
{
    const pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic->context;
    return ctx->fmt;
}

picture_context_t * hw_mmal_pic_ctx_copy(picture_context_t * pic_ctx_cmn);
void hw_mmal_pic_ctx_destroy(picture_context_t * pic_ctx_cmn);
picture_context_t * hw_mmal_gen_context(const MMAL_FOURCC_T fmt,
    MMAL_BUFFER_HEADER_T * buf, hw_mmal_port_pool_ref_t * const ppr);

int hw_mmal_get_gpu_mem(void);


static inline MMAL_STATUS_T port_parameter_set_uint32(MMAL_PORT_T * port, uint32_t id, uint32_t val)
{
    const MMAL_PARAMETER_UINT32_T param = {
        .hdr = {.id = id, .size = sizeof(MMAL_PARAMETER_UINT32_T)},
        .value = val
    };
    return mmal_port_parameter_set(port, &param.hdr);
}

static inline MMAL_STATUS_T port_parameter_set_bool(MMAL_PORT_T * const port, const uint32_t id, const bool val)
{
    const MMAL_PARAMETER_BOOLEAN_T param = {
        .hdr = {.id = id, .size = sizeof(MMAL_PARAMETER_BOOLEAN_T)},
        .enable = val
    };
    return mmal_port_parameter_set(port, &param.hdr);
}

static inline MMAL_STATUS_T port_send_replicated(MMAL_PORT_T * const port, MMAL_POOL_T * const rep_pool,
                                          MMAL_BUFFER_HEADER_T * const src_buf,
                                          const uint64_t seq)
{
    MMAL_STATUS_T err;
    MMAL_BUFFER_HEADER_T *const rep_buf = mmal_queue_wait(rep_pool->queue);

    if (rep_buf == NULL)
        return MMAL_ENOSPC;

    if ((err = mmal_buffer_header_replicate(rep_buf, src_buf)) != MMAL_SUCCESS)
        return err;

    rep_buf->pts = seq;

    if ((err = mmal_port_send_buffer(port, rep_buf)) != MMAL_SUCCESS)
    {
        mmal_buffer_header_release(rep_buf);
        return err;
    }

    return MMAL_SUCCESS;
}

static inline void pic_to_buf_copy_props(MMAL_BUFFER_HEADER_T * const buf, const picture_t * const pic)
{
    if (!pic->b_progressive)
    {
        buf->flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
        buf->type->video.flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
    }
    else
    {
        buf->flags &= ~MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
        buf->type->video.flags &= ~MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED;
    }
    if (pic->b_top_field_first)
    {
        buf->flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
        buf->type->video.flags |= MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
    }
    else
    {
        buf->flags &= ~MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
        buf->type->video.flags &= ~MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
    }
    buf->pts = pic->date != VLC_TICK_INVALID ? pic->date : MMAL_TIME_UNKNOWN;
    buf->dts = buf->pts;
}

static inline void buf_to_pic_copy_props(picture_t * const pic, const MMAL_BUFFER_HEADER_T * const buf)
{
    // Contrary to docn the interlace & tff flags turn up in the header flags rather than the
    // video specific flags (which appear to be currently unused).
    pic->b_progressive = (buf->flags & MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED) == 0;
    pic->b_top_field_first = (buf->flags & MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST) != 0;

    pic->date = buf->pts != MMAL_TIME_UNKNOWN ? buf->pts :
        buf->dts != MMAL_TIME_UNKNOWN ? buf->dts :
            VLC_TICK_INVALID;
}

// Retrieve buf from pic & update with pic props
// Note that this is a weak pointer - replicate before putting in a Q
static inline MMAL_BUFFER_HEADER_T * pic_mmal_buffer(const picture_t *const pic)
{
    MMAL_BUFFER_HEADER_T * const buf = ((pic_ctx_mmal_t *)pic->context)->bufs[0];
    if (buf != NULL)
        pic_to_buf_copy_props(buf, pic);

    return buf;
}

struct vzc_pool_ctl_s;
typedef struct vzc_pool_ctl_s vzc_pool_ctl_t;

static inline bool hw_mmal_vzc_subpic_fmt_valid(const video_frame_format_t * const vf_vlc)
{
    const vlc_fourcc_t vfcc_src = vf_vlc->i_chroma;
    // At the moment we cope with any mono-planar RGBA thing
    // We could cope with many other things but they currently don't occur
    return vfcc_src == VLC_CODEC_RGBA || vfcc_src == VLC_CODEC_BGRA || vfcc_src == VLC_CODEC_ARGB;
}

bool hw_mmal_vzc_buf_set_format(MMAL_BUFFER_HEADER_T * const buf, MMAL_ES_FORMAT_T * const es_fmt);
MMAL_DISPLAYREGION_T * hw_mmal_vzc_buf_region(MMAL_BUFFER_HEADER_T * const buf);
void hw_mmal_vzc_buf_scale_dest_rect(MMAL_BUFFER_HEADER_T * const buf, const MMAL_RECT_T * const scale_rect);
void hw_mmal_vzc_buf_get_wh(MMAL_BUFFER_HEADER_T * const buf, int * const pW, int * const pH);
unsigned int hw_mmal_vzc_buf_seq(MMAL_BUFFER_HEADER_T * const buf);
MMAL_BUFFER_HEADER_T * hw_mmal_vzc_buf_from_pic(vzc_pool_ctl_t * const pc, picture_t * const pic,
                                                const MMAL_RECT_T dst_pic_rect,
                                                const int x_offset, const int y_offset,
                                                const unsigned int alpha, const bool is_first);
void hw_mmal_vzc_buf_frame_size(MMAL_BUFFER_HEADER_T * const buf,
                                uint32_t * const pWidth, uint32_t * const pHeight);

void hw_mmal_vzc_pool_flush(vzc_pool_ctl_t * const pc);
void hw_mmal_vzc_pool_release(vzc_pool_ctl_t * const pc);
void hw_mmal_vzc_pool_ref(vzc_pool_ctl_t * const pc);
vzc_pool_ctl_t * hw_mmal_vzc_pool_new(void);


static inline MMAL_RECT_T vis_mmal_rect(const video_format_t * const fmt)
{
    return (MMAL_RECT_T){
        .x      = fmt->i_x_offset,
        .y      = fmt->i_y_offset,
        .width  = fmt->i_visible_width,
        .height = fmt->i_visible_height
    };
}

#define VOUT_DISPLAY_CHANGE_MMAL_BASE 1024
#define VOUT_DISPLAY_CHANGE_MMAL_HIDE (VOUT_DISPLAY_CHANGE_MMAL_BASE + 0)

#define MMAL_COMPONENT_DEFAULT_RESIZER "vc.ril.resize"
#define MMAL_COMPONENT_ISP_RESIZER     "vc.ril.isp"
#define MMAL_COMPONENT_HVS             "vc.ril.hvs"

#endif
