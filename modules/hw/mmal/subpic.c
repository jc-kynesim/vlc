/*****************************************************************************
 * mmal.c: MMAL-based decoder plugin for Raspberry Pi
 *****************************************************************************
 * Authors: jc@kynesim.co.uk
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
#include "config.h"
#endif

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_filter.h>
#include <vlc_threads.h>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#include "mmal_picture.h"
#include "subpic.h"

#define TRACE_ALL 0

static inline int rescale_x(int x, int mul, int div)
{
    return div == 0 ? x * mul : (x * mul + div/2) / div;
}

static void rescale_rect(MMAL_RECT_T * const d, const MMAL_RECT_T * const s, const MMAL_RECT_T * mul_rect, const MMAL_RECT_T * div_rect)
{
    d->x      = rescale_x(s->x,      mul_rect->width,  div_rect->width);
    d->y      = rescale_x(s->y,      mul_rect->height, div_rect->height);
    d->width  = rescale_x(s->width,  mul_rect->width,  div_rect->width);
    d->height = rescale_x(s->height, mul_rect->height, div_rect->height);
}

static inline bool cmp_rect(const MMAL_RECT_T * const a, const MMAL_RECT_T * const b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}


static void conv_subpic_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
#if TRACE_ALL
    msg_Dbg((filter_t *)port->userdata, "<<< %s cmd=%d, user=%p, buf=%p, flags=%#x, len=%d/%d, pts=%lld",
            __func__, buf->cmd, buf->user_data, buf, buf->flags, buf->length, buf->alloc_size, (long long)buf->pts);
#else
    VLC_UNUSED(port);
#endif

    mmal_buffer_header_release(buf);  // Will extract & release pic in pool callback
}


int hw_mmal_subpic_update(vlc_object_t * const p_filter,
    picture_t * const p_pic, const unsigned int sub_no,
    MMAL_PORT_T * const port,
    subpic_reg_stash_t * const stash,
    MMAL_PORT_T * const scale_out_port, MMAL_PORT_T * const scale_in_port,
    MMAL_POOL_T * const pool, const uint64_t pts)
{
    MMAL_STATUS_T err;
    MMAL_BUFFER_HEADER_T * const sub_buf = hw_mmal_pic_sub_buf_get(p_pic, sub_no);

    if (sub_buf == NULL)
    {
        if (port->is_enabled && stash->seq != 0)
        {
            MMAL_BUFFER_HEADER_T *const buf = mmal_queue_wait(pool->queue);

            if (buf == NULL) {
                msg_Err(p_filter, "Buffer get for subpic failed");
                return -1;
            }
#if TRACE_ALL
            msg_Dbg(p_filter, "Remove pic for sub %d", sub_no);
#endif
            buf->cmd = 0;
            buf->data = NULL;
            buf->alloc_size = 0;
            buf->offset = 0;
            buf->flags = 0;
            buf->pts = pts;
            buf->dts = MMAL_TIME_UNKNOWN;
            buf->user_data = NULL;

            if ((err = mmal_port_send_buffer(port, buf)) != MMAL_SUCCESS)
            {
                msg_Err(p_filter, "Send buffer to subput failed");
                mmal_buffer_header_release(buf);
                return -1;
            }

            stash->seq = 0;
        }
    }
    else
    {
        const unsigned int seq = hw_mmal_vzc_buf_seq(sub_buf);
        bool needs_update = (stash->seq != seq);

        if (hw_mmal_vzc_buf_set_format(sub_buf, port->format))
        {
            MMAL_DISPLAYREGION_T * const dreg = hw_mmal_vzc_buf_region(sub_buf);
            MMAL_VIDEO_FORMAT_T *const v_fmt = &port->format->es->video;

            v_fmt->frame_rate.den = p_pic->format.i_frame_rate_base;
            v_fmt->frame_rate.num = p_pic->format.i_frame_rate;
            v_fmt->par.den = p_pic->format.i_sar_den;
            v_fmt->par.num = p_pic->format.i_sar_num;
            v_fmt->color_space = MMAL_COLOR_SPACE_UNKNOWN;

            rescale_rect(&dreg->dest_rect, hw_mmal_vzc_buf_get_dest_rect(sub_buf),
                         &scale_out_port->format->es->video.crop, &scale_in_port->format->es->video.crop);

            if (needs_update || dreg->alpha != stash->alpha || !cmp_rect(&dreg->dest_rect, &stash->dest_rect)) {

                stash->alpha = dreg->alpha;
                stash->dest_rect = dreg->dest_rect;
                needs_update = true;
#if TRACE_ALL
                msg_Dbg(p_filter, "Update region for sub %d", sub_no);
#endif
                if ((err = mmal_port_parameter_set(port, &dreg->hdr)) != MMAL_SUCCESS)
                {
                    msg_Err(p_filter, "Set display region on subput failed");
                    return -1;
                }

                if ((err = mmal_port_format_commit(port)) != MMAL_SUCCESS)
                {
                    msg_Dbg(p_filter, "%s: Subpic commit fail: %d", __func__, err);
                    return -1;
                }
            }
        }

        if (!port->is_enabled)
        {
            port->buffer_num = 30;
            port->buffer_size = port->buffer_size_recommended;  // Not used but shuts up the error checking

            if ((err = mmal_port_enable(port, conv_subpic_cb)) != MMAL_SUCCESS)
            {
                msg_Dbg(p_filter, "%s: Subpic enable fail: %d", __func__, err);
                return -1;
            }
        }

        if (needs_update)
        {
#if TRACE_ALL
            msg_Dbg(p_filter, "Update pic for sub %d", sub_no);
#endif
            if ((err = port_send_replicated(port, pool, sub_buf, pts)) != MMAL_SUCCESS)
            {
                msg_Err(p_filter, "Send buffer to subput failed");
                return -1;
            }

            stash->seq = seq;
        }
    }
    return 1;
}



