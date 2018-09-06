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

typedef struct pic_ctx_mmal_s {
    picture_context_t cmn;  // PARENT: Common els at start

    MMAL_BUFFER_HEADER_T * buf;
    hw_mmal_port_pool_ref_t * ppr;
    pic_ctx_subpic_t sub;

    vlc_object_t * obj;
} pic_ctx_mmal_t;


void vlc_to_mmal_pic_fmt(MMAL_PORT_T * const port, const es_format_t * const es_vlc);

hw_mmal_port_pool_ref_t * hw_mmal_port_pool_ref_create(MMAL_PORT_T * const port,
   const unsigned int headers, const uint32_t payload_size);
void hw_mmal_port_pool_ref_release(hw_mmal_port_pool_ref_t * const ppr, const bool in_cb);
bool hw_mmal_port_pool_ref_recycle(hw_mmal_port_pool_ref_t * const ppr, MMAL_BUFFER_HEADER_T * const buf);
MMAL_STATUS_T hw_mmal_port_pool_ref_fill(hw_mmal_port_pool_ref_t * const ppr);
static inline void hw_mmal_port_pool_ref_acquire(hw_mmal_port_pool_ref_t * const ppr)
{
    atomic_fetch_add(&ppr->refs, 1);
}

static inline void hw_mmal_pic_set_subpic(picture_t * pic, const picture_t * subpic, int x, int y, int alpha)
{
    pic_ctx_subpic_t * const sub = &((pic_ctx_mmal_t *)pic->context)->sub;
    sub->subpic = picture_Hold((picture_t *)subpic);
    sub->x = x;
    sub->y = y;
    sub->alpha = alpha;
}

static inline void hw_mmal_pic_unset_subpic(picture_t * pic)
{
    pic_ctx_subpic_t * const sub = &((pic_ctx_mmal_t *)pic->context)->sub;
    if (sub->subpic != NULL)
    {
        picture_Release(sub->subpic);
        sub->subpic = NULL;
    }
}

picture_context_t * hw_mmal_pic_ctx_copy(picture_context_t * pic_ctx_cmn);
void hw_mmal_pic_ctx_destroy(picture_context_t * pic_ctx_cmn);
picture_context_t * hw_mmal_gen_context(MMAL_BUFFER_HEADER_T * buf,
    hw_mmal_port_pool_ref_t * const ppr, vlc_object_t * obj);

int hw_mmal_get_gpu_mem(void);


static inline MMAL_STATUS_T port_parameter_set_uint32(MMAL_PORT_T * port, uint32_t id, uint32_t val)
{
    const MMAL_PARAMETER_UINT32_T param = {
        .hdr = {.id = id, .size = sizeof(MMAL_PARAMETER_UINT32_T)},
        .value = val
    };
    return mmal_port_parameter_set(port, &param.hdr);
}

static inline MMAL_STATUS_T port_parameter_set_bool(MMAL_PORT_T * port, uint32_t id, int val)
{
    const MMAL_PARAMETER_BOOLEAN_T param = {
        .hdr = {.id = id, .size = sizeof(MMAL_PARAMETER_BOOLEAN_T)},
        .enable = val
    };
    return mmal_port_parameter_set(port, &param.hdr);
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
static inline MMAL_BUFFER_HEADER_T * pic_mmal_buffer(const picture_t *const pic)
{
    MMAL_BUFFER_HEADER_T * const buf = ((pic_ctx_mmal_t *)pic->context)->buf;
    if (buf != NULL)
        pic_to_buf_copy_props(buf, pic);

    return buf;
}



#endif
