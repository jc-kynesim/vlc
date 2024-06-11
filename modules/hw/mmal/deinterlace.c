/*****************************************************************************
 * mmal.c: MMAL-based deinterlace plugin for Raspberry Pi
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Julian Scheel <julian@jusst.de>
 *          Dennis Hamester <dennis.hamester@gmail.com>
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
#include <vlc_picture_pool.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>

#include "mmal_picture.h"

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#define MMAL_DEINTERLACE_NO_QPU "mmal-deinterlace-no-qpu"
#define MMAL_DEINTERLACE_NO_QPU_TEXT N_("Do not use QPUs for advanced HD deinterlacing.")
#define MMAL_DEINTERLACE_NO_QPU_LONGTEXT N_("Do not make use of the QPUs to allow higher quality deinterlacing of HD content.")

#define MMAL_DEINTERLACE_ADV "mmal-deinterlace-adv"
#define MMAL_DEINTERLACE_ADV_TEXT N_("Force advanced deinterlace")
#define MMAL_DEINTERLACE_ADV_LONGTEXT N_("Force advanced deinterlace")

#define MMAL_DEINTERLACE_FAST "mmal-deinterlace-fast"
#define MMAL_DEINTERLACE_FAST_TEXT N_("Force fast deinterlace")
#define MMAL_DEINTERLACE_FAST_LONGTEXT N_("Force fast deinterlace")

#define MMAL_DEINTERLACE_NONE "mmal-deinterlace-none"
#define MMAL_DEINTERLACE_NONE_TEXT N_("Force no deinterlace")
#define MMAL_DEINTERLACE_NONE_LONGTEXT N_("Force no interlace. Simply strips off the interlace markers and passes the frame straight through. "\
    "This is the default for > SD if < 96M gpu-mem")

#define MMAL_DEINTERLACE_HALF_RATE "mmal-deinterlace-half-rate"
#define MMAL_DEINTERLACE_HALF_RATE_TEXT N_("Halve output framerate")
#define MMAL_DEINTERLACE_HALF_RATE_LONGTEXT N_("Halve output framerate. 1 output frame for each pair of interlaced fields input")

#define MMAL_DEINTERLACE_FULL_RATE "mmal-deinterlace-full-rate"
#define MMAL_DEINTERLACE_FULL_RATE_TEXT N_("Full output framerate")
#define MMAL_DEINTERLACE_FULL_RATE_LONGTEXT N_("Full output framerate. 1 output frame for each interlaced field input")


typedef struct filter_sys_t
{
    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_PORT_T *output;
    MMAL_POOL_T *in_pool;

    MMAL_QUEUE_T * out_q;

    // Bind this lot somehow into ppr????
    bool is_cma;
    cma_buf_pool_t * cma_out_pool;
    MMAL_POOL_T * out_pool;

    hw_mmal_port_pool_ref_t *out_ppr;

    bool half_rate;
    bool use_qpu;
    bool use_fast;
    bool use_passthrough;
    unsigned int seq_in;    // Seq of next frame to submit (1-15) [Init=1]
    unsigned int seq_out;   // Seq of last frame received  (1-15) [Init=15]

    vcsm_init_type_t vcsm_init_type;

} filter_sys_t;


#define MMAL_COMPONENT_DEFAULT_DEINTERLACE "vc.ril.image_fx"

#define TRACE_ALL 0



// Buffer attached to pic on success, is still valid on failure
static picture_t * di_alloc_opaque(filter_t * const p_filter, MMAL_BUFFER_HEADER_T * const buf)
{
    filter_sys_t *const filter_sys = p_filter->p_sys;
    picture_t * const pic = filter_NewPicture(p_filter);

    if (pic == NULL)
        goto fail1;

    if (buf->length == 0) {
        msg_Err(p_filter, "%s: Empty buffer", __func__);
        goto fail2;
    }

    if ((pic->context = hw_mmal_gen_context(buf, filter_sys->out_ppr)) == NULL)
        goto fail2;

    buf_to_pic_copy_props(pic, buf);

#if TRACE_ALL
    msg_Dbg(p_filter, "pic: prog=%d, tff=%d, date=%lld", pic->b_progressive, pic->b_top_field_first, (long long)pic->date);
#endif

    return pic;

fail2:
    picture_Release(pic);
fail1:
//    mmal_buffer_header_release(buf);
    return NULL;
}

static void di_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
#if TRACE_ALL
    pic_ctx_mmal_t * ctx = buffer->user_data;
//    filter_sys_t *const sys = ((filter_t *)port->userdata)->p_sys;

    msg_Dbg((filter_t *)port->userdata, "<<< %s: cmd=%d, ctx=%p, buf=%p, flags=%#x, pts=%lld", __func__, buffer->cmd, ctx, buffer,
            buffer->flags, (long long)buffer->pts);
#else
    VLC_UNUSED(port);
#endif

    mmal_buffer_header_release(buffer);

#if TRACE_ALL
    msg_Dbg((filter_t *)port->userdata, ">>> %s", __func__);
#endif
}

static void di_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    if (buf->cmd == 0 && buf->length != 0)
    {
        // The filter structure etc. should always exist if we have contents
        // but might not on later flushes as we shut down
        filter_t * const p_filter = (filter_t *)port->userdata;
        filter_sys_t * const sys = p_filter->p_sys;

#if TRACE_ALL
        msg_Dbg(p_filter, "<<< %s: cmd=%d; flags=%#x, pts=%lld", __func__, buf->cmd, buf->flags, (long long) buf->pts);
#endif
        mmal_queue_put(sys->out_q, buf);
#if TRACE_ALL
        msg_Dbg(p_filter, ">>> %s: out Q len=%d", __func__, mmal_queue_length(sys->out_q));
#endif
        return;
    }

    mmal_buffer_header_reset(buf);   // User data stays intact so release will kill pic
    mmal_buffer_header_release(buf);
}



static MMAL_STATUS_T fill_output_from_q(filter_t * const p_filter, filter_sys_t * const sys, MMAL_QUEUE_T * const q)
{
    MMAL_BUFFER_HEADER_T * out_buf;

    while ((out_buf = mmal_queue_get(q)) != NULL)
    {
        MMAL_STATUS_T err;
        if ((err = mmal_port_send_buffer(sys->output, out_buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Send buffer to output failed");
            mmal_queue_put_back(q, out_buf);
            return err;
        }
    }
    return MMAL_SUCCESS;
}

// Output buffers may contain a pic ref on error or flush
// Free it
static MMAL_BOOL_T out_buffer_pre_release_cb(MMAL_BUFFER_HEADER_T *header, void *userdata)
{
    VLC_UNUSED(userdata);

    cma_buf_t * const cb = header->user_data;
    header->user_data = NULL;
    cma_buf_unref(cb);  // Copes fine with NULL

    return MMAL_FALSE;
}

static inline unsigned int seq_inc(unsigned int x)
{
    return x + 1 >= 16 ? 1 : x + 1;
}

static inline unsigned int seq_delta(unsigned int sseq, unsigned int fseq)
{
    return fseq == 0 ? 0 : fseq <= sseq ? sseq - fseq : 15 - (fseq - sseq);
}

static picture_t *deinterlace(filter_t * p_filter, picture_t * p_pic)
{
    filter_sys_t * const sys = p_filter->p_sys;
    picture_t *ret_pics = NULL;
    MMAL_STATUS_T err;
    MMAL_BUFFER_HEADER_T * out_buf = NULL;

#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif

    if (hw_mmal_vlc_pic_to_mmal_fmt_update(sys->input->format, p_pic))
    {
        // ****** Breaks on opaque (at least)

        if (sys->input->is_enabled)
            mmal_port_disable(sys->input);
#if 0
        if (sys->output->is_enabled)
            mmal_port_disable(sys->output);

        mmal_format_full_copy(sys->output->format, sys->input->format);
        mmal_port_format_commit(sys->output);
        sys->output->buffer_num = 30;
        sys->output->buffer_size = sys->input->buffer_size_recommended;
        mmal_port_enable(sys->output, di_output_port_cb);
#endif
        if (mmal_port_format_commit(sys->input) != MMAL_SUCCESS)
            msg_Err(p_filter, "Failed to update pic format");
        sys->input->buffer_num = 30;
        sys->input->buffer_size = sys->input->buffer_size_recommended;
        mmal_log_dump_format(sys->input->format);
    }

    // Reenable stuff if the last thing we did was flush
    // Output should always be enabled
    if (!sys->input->is_enabled &&
        (err = mmal_port_enable(sys->input, di_input_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(p_filter, "Input port reenable failed");
        goto fail;
    }

    if (!sys->is_cma)
    {
        // Fill output from anything that has turned up in pool Q
        if (hw_mmal_port_pool_ref_fill(sys->out_ppr) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Out port fill fail");
            goto fail;
        }
    }
    else
    {
        // We are expecting one in - one out so simply wedge a new bufer
        // into the output port.  Flow control will happen on cma alloc.

        if ((out_buf = mmal_queue_get(sys->out_pool->queue)) == NULL)
        {
            // Should never happen
            msg_Err(p_filter, "Failed to get output buffer");
            goto fail;
        }
        mmal_buffer_header_reset(out_buf);

        // Attach cma_buf to the buffer & ensure it is freed when the buffer is released
        // On a good send callback the pic will be extracted to avoid this
        mmal_buffer_header_pre_release_cb_set(out_buf, out_buffer_pre_release_cb, p_filter);

        cma_buf_t * const cb = cma_buf_pool_alloc_buf(sys->cma_out_pool, sys->output->buffer_size);
        if ((out_buf->user_data = cb) == NULL)  // Check & attach cb to buf
        {
            char dbuf0[5];
            msg_Err(p_filter, "Failed to alloc CMA buf: fmt=%s, size=%d",
                    str_fourcc(dbuf0, p_pic->format.i_chroma),
                    sys->output->buffer_size);
            goto fail;
        }
        const unsigned int vc_h = cma_buf_vc_handle(cb);  // Cannot coerce without going via variable
        out_buf->data = (uint8_t *)vc_h;
        out_buf->alloc_size = sys->output->buffer_size;

#if TRACE_ALL
        msg_Dbg(p_filter, "Out buf send: pic=%p, data=%p, user=%p, flags=%#x, len=%d/%d, pts=%lld",
                p_pic, out_buf->data, out_buf->user_data, out_buf->flags,
                out_buf->length, out_buf->alloc_size, (long long)out_buf->pts);
#endif

        if ((err = mmal_port_send_buffer(sys->output, out_buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Send buffer to output failed");
            goto fail;
        }
        out_buf = NULL;
    }

    // Stuff into input
    // We assume the BH is already set up with values reflecting pic date etc.
    {
        MMAL_BUFFER_HEADER_T * const pic_buf = hw_mmal_pic_buf_replicated(p_pic, sys->in_pool);

        if (pic_buf == NULL)
        {
            msg_Err(p_filter, "Pic has not attached buffer");
            goto fail;
        }

        picture_Release(p_pic);

        // Add a sequence to the flags so we can track what we have actually
        // deinterlaced
        pic_buf->flags = (pic_buf->flags & ~(0xfU * MMAL_BUFFER_HEADER_FLAG_USER0)) | (sys->seq_in * (MMAL_BUFFER_HEADER_FLAG_USER0));
        sys->seq_in = seq_inc(sys->seq_in);

        if ((err = mmal_port_send_buffer(sys->input, pic_buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Send buffer to input failed");
            mmal_buffer_header_release(pic_buf);
            goto fail;
        }
    }

    // Return anything that is in the out Q
    {
        picture_t ** pp_pic = &ret_pics;

        // Advanced di has a 3 frame latency, so if the seq delta is greater
        // than that then we are expecting at least two frames of output. Wait
        // for one of those.
        // seq_in is seq of the next frame we are going to submit (1-15, no 0)
        // seq_out is last frame we removed from Q
        // So after 4 frames sent (1st time we want to wait), 0 rx seq_in=5, seq_out=15, delta=5

        while ((out_buf = (seq_delta(sys->seq_in, sys->seq_out) >= 5 ? mmal_queue_timedwait(sys->out_q, 1000) : mmal_queue_get(sys->out_q))) != NULL)
        {
            const unsigned int seq_out = (out_buf->flags / MMAL_BUFFER_HEADER_FLAG_USER0) & 0xf;
            int rv;

            picture_t * out_pic;

            if (sys->is_cma)
            {
                // Alloc pic
                if ((out_pic = filter_NewPicture(p_filter)) == NULL)
                {
                    // Can't alloc pic - just stop extraction
                    mmal_queue_put_back(sys->out_q, out_buf);
                    out_buf = NULL;
                    msg_Warn(p_filter, "Failed to alloc new filter output pic");
                    break;
                }

                // Extract cma_buf from buf & attach to pic
                cma_buf_t * const cb = (cma_buf_t *)out_buf->user_data;
                if ((rv = cma_buf_pic_attach(cb, out_pic)) != VLC_SUCCESS)
                {
                    char dbuf0[5];
                    msg_Err(p_filter, "Failed to attach CMA to pic: fmt=%s err=%d",
                            str_fourcc(dbuf0, out_pic->format.i_chroma),
                            rv);
                    // cb still attached to buffer and will be freed with it
                    goto fail;
                }
                out_buf->user_data = NULL;

                buf_to_pic_copy_props(out_pic, out_buf);

                // Set pic data pointers from buf aux info now it has it
                if ((rv = cma_pic_set_data(out_pic, sys->output->format, out_buf)) != VLC_SUCCESS)
                {
                    char dbuf0[5];
                    msg_Err(p_filter, "Failed to set data: fmt=%s, rv=%d",
                            str_fourcc(dbuf0, sys->output->format->encoding),
                            rv);
                }

                out_buf->user_data = NULL;  // Responsability for this pic no longer with buffer
                mmal_buffer_header_release(out_buf);
            }
            else
            {
                out_pic = di_alloc_opaque(p_filter, out_buf);

                if (out_pic == NULL) {
                    msg_Warn(p_filter, "Failed to alloc new filter output pic");
                    mmal_queue_put_back(sys->out_q, out_buf);  // Wedge buf back into Q in the hope we can alloc a pic later
                    out_buf = NULL;
                    break;
                }
            }
            out_buf = NULL;  // Now attached to pic or recycled

#if TRACE_ALL
            msg_Dbg(p_filter, "-- %s: Q pic=%p: seq_in=%d, seq_out=%d, delta=%d", __func__, out_pic, sys->seq_in, seq_out, seq_delta(sys->seq_in, seq_out));
#endif

            *pp_pic = out_pic;
            pp_pic = &out_pic->p_next;

            // Ignore 0 seqs
            // Don't think these should actually happen
            if (seq_out != 0)
                sys->seq_out = seq_out;
        }

        // Crash on lockup
        assert(ret_pics != NULL || seq_delta(sys->seq_in, sys->seq_out) < 5);
    }

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s: pic=%p", __func__, ret_pics);
#endif

    return ret_pics;

fail:
    if (out_buf != NULL)
        mmal_buffer_header_release(out_buf);
    picture_Release(p_pic);
    return NULL;
}

static void di_flush(filter_t *p_filter)
{
    filter_sys_t * const sys = p_filter->p_sys;

#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif

    if (sys->input != NULL && sys->input->is_enabled)
        mmal_port_disable(sys->input);

    if (sys->output != NULL && sys->output->is_enabled)
    {
        if (sys->is_cma)
        {
            MMAL_BUFFER_HEADER_T * buf;
            mmal_port_disable(sys->output);
            while ((buf = mmal_queue_get(sys->out_q)) != NULL)
                mmal_buffer_header_release(buf);
        }
        else
        {
            // Wedge anything we've got into the output port as that will free the underlying buffers
            fill_output_from_q(p_filter, sys, sys->out_q);

            mmal_port_disable(sys->output);

            // If that dumped anything real into the out_q then have another go
            if (mmal_queue_length(sys->out_q) != 0)
            {
                mmal_port_enable(sys->output, di_output_port_cb);
                fill_output_from_q(p_filter, sys, sys->out_q);
                mmal_port_disable(sys->output);
                // Out q should now be empty & should remain so until the input is reenabled
            }
        }
        mmal_port_enable(sys->output, di_output_port_cb);

        // Leaving the input disabled is fine - but we want to leave the output enabled
        // so we can retrieve buffers that are still bound to pictures
    }

    sys->seq_in = 1;
    sys->seq_out = 15;

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s", __func__);
#endif
}


static void pass_flush(filter_t *p_filter)
{
    // Nothing to do
    VLC_UNUSED(p_filter);
}

static picture_t * pass_deinterlace(filter_t * p_filter, picture_t * p_pic)
{
    VLC_UNUSED(p_filter);

    p_pic->b_progressive = true;
    return p_pic;
}


static void control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    filter_t *filter = (filter_t *)port->userdata;
    MMAL_STATUS_T status;

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        status = *(uint32_t *)buffer->data;
        msg_Err(filter, "MMAL error %"PRIx32" \"%s\"", status,
                mmal_status_to_string(status));
    }

    mmal_buffer_header_reset(buffer);
    mmal_buffer_header_release(buffer);
}

static void CloseMmalDeinterlace(filter_t *filter)
{
    filter_sys_t * const sys = filter->p_sys;

#if TRACE_ALL
    msg_Dbg(filter, "<<< %s", __func__);
#endif

    if (sys == NULL)
        return;

    if (sys->use_passthrough)
    {
        free(sys);
        return;
    }

    di_flush(filter);

    if (sys->component && sys->component->control->is_enabled)
        mmal_port_disable(sys->component->control);

    if (sys->component && sys->component->is_enabled)
        mmal_component_disable(sys->component);

    if (sys->in_pool != NULL)
        mmal_pool_destroy(sys->in_pool);

    hw_mmal_port_pool_ref_release(sys->out_ppr, false);
    // Once we exit filter & sys are invalid so mark as such
    if (sys->output != NULL)
        sys->output->userdata = NULL;

    if (sys->is_cma)
    {
        if (sys->output && sys->output->is_enabled)
            mmal_port_disable(sys->output);

        cma_buf_pool_deletez(&sys->cma_out_pool);

        if (sys->out_pool != NULL)
            mmal_pool_destroy(sys->out_pool);
    }

    if (sys->out_q != NULL)
        mmal_queue_destroy(sys->out_q);

    if (sys->component)
        mmal_component_release(sys->component);

    cma_vcsm_exit(sys->vcsm_init_type);

    free(sys);
}


static bool is_fmt_valid_in(const vlc_fourcc_t fmt)
{
    return fmt == VLC_CODEC_MMAL_OPAQUE ||
           fmt == VLC_CODEC_MMAL_ZC_I420 ||
           fmt == VLC_CODEC_MMAL_ZC_SAND8;
}

static int OpenMmalDeinterlace(filter_t *filter)
{
    int32_t frame_duration = filter->fmt_in.video.i_frame_rate != 0 ?
            CLOCK_FREQ * filter->fmt_in.video.i_frame_rate_base /
            filter->fmt_in.video.i_frame_rate : 0;

    int ret = VLC_EGENERIC;
    MMAL_STATUS_T status;
    filter_sys_t *sys;

    msg_Dbg(filter, "<<< %s", __func__);

    if (!is_fmt_valid_in(filter->fmt_in.video.i_chroma) ||
        filter->fmt_out.video.i_chroma != filter->fmt_in.video.i_chroma)
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(filter_sys_t));
    if (!sys)
        return VLC_ENOMEM;
    filter->p_sys = sys;

    sys->seq_in = 1;
    sys->seq_out = 15;
    sys->is_cma = is_cma_buf_pic_chroma(filter->fmt_out.video.i_chroma);

    if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
        msg_Err(filter, "VCSM init failed");
        goto fail;
    }

    if (!rpi_use_qpu_deinterlace())
    {
        sys->half_rate = true;
        sys->use_qpu = false;
        sys->use_fast = true;
    }
    else
    {
        sys->half_rate = false;
        sys->use_qpu = true;
        sys->use_fast = false;
    }
    sys->use_passthrough = false;

    if (filter->fmt_in.video.i_width * filter->fmt_in.video.i_height > 768 * 576)
    {
        // We get stressed if we have to try too hard - so make life easier
        sys->half_rate = true;
        // Also check we actually have enough memory to do this
        // Memory always comes from GPU if Opaque
        // Assume we have plenty of memory if it comes from CMA
        if ((!sys->is_cma || sys->vcsm_init_type == VCSM_INIT_LEGACY) &&
            hw_mmal_get_gpu_mem() < (96 << 20))
        {
            sys->use_passthrough = true;
            msg_Warn(filter, "Deinterlace bypassed due to lack of GPU memory");
        }
    }

    if (var_InheritBool(filter, MMAL_DEINTERLACE_NO_QPU))
        sys->use_qpu = false;
    if (var_InheritBool(filter, MMAL_DEINTERLACE_ADV))
    {
        sys->use_fast = false;
        sys->use_passthrough = false;
    }
    if (var_InheritBool(filter, MMAL_DEINTERLACE_FAST))
    {
        sys->use_fast = true;
        sys->use_passthrough = false;
    }
    if (var_InheritBool(filter, MMAL_DEINTERLACE_NONE))
        sys->use_passthrough = true;
    if (var_InheritBool(filter, MMAL_DEINTERLACE_FULL_RATE))
        sys->half_rate = false;
    if (var_InheritBool(filter, MMAL_DEINTERLACE_HALF_RATE))
        sys->half_rate = true;

    if (sys->use_passthrough)
    {
        filter->pf_video_filter = pass_deinterlace;
        filter->pf_flush = pass_flush;
        // Don't need VCSM - get rid of it now
        cma_vcsm_exit(sys->vcsm_init_type);
        sys->vcsm_init_type = VCSM_INIT_NONE;
        return 0;
    }

    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(filter, "%s: %s,%dx%d [(%d,%d) %d/%d] -> %s,%dx%d [(%d,%d) %dx%d]: %s %s %s", __func__,
                str_fourcc(dbuf0, filter->fmt_in.video.i_chroma),
                filter->fmt_in.video.i_width, filter->fmt_in.video.i_height,
                filter->fmt_in.video.i_x_offset, filter->fmt_in.video.i_y_offset,
                filter->fmt_in.video.i_visible_width, filter->fmt_in.video.i_visible_height,
                str_fourcc(dbuf1, filter->fmt_out.video.i_chroma),
                filter->fmt_out.video.i_width, filter->fmt_out.video.i_height,
                filter->fmt_out.video.i_x_offset, filter->fmt_out.video.i_y_offset,
                filter->fmt_out.video.i_visible_width, filter->fmt_out.video.i_visible_height,
                sys->use_qpu ? "QPU" : "VPU",
                sys->use_fast ? "FAST" : "ADV",
                sys->use_passthrough ? "PASS" : sys->half_rate ? "HALF" : "FULL");
    }

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_DEINTERLACE, &sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(filter, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                MMAL_COMPONENT_DEFAULT_DEINTERLACE, status, mmal_status_to_string(status));
        goto fail;
    }

    {
        const MMAL_PARAMETER_IMAGEFX_PARAMETERS_T imfx_param = {
            { MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS, sizeof(imfx_param) },
            sys->use_fast ?
                MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST :
                MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV,
            4,
            { 5 /* Frame type: mixed */, frame_duration, sys->half_rate, sys->use_qpu }
        };

        status = mmal_port_parameter_set(sys->component->output[0], &imfx_param.hdr);
        if (status != MMAL_SUCCESS) {
            msg_Err(filter, "Failed to configure MMAL component %s (status=%"PRIx32" %s)",
                    MMAL_COMPONENT_DEFAULT_DEINTERLACE, status, mmal_status_to_string(status));
            goto fail;
        }
    }

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)filter;
    status = mmal_port_enable(sys->component->control, control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(filter, "Failed to enable control port %s (status=%"PRIx32" %s)",
                sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->input = sys->component->input[0];
    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)filter;
    sys->input->format->encoding = vlc_to_mmal_video_fourcc(&filter->fmt_in.video);
    hw_mmal_vlc_fmt_to_mmal_fmt(sys->input->format, &filter->fmt_in.video);

    es_format_Copy(&filter->fmt_out, &filter->fmt_in);
    if (!sys->half_rate)
        filter->fmt_out.video.i_frame_rate *= 2;

    status = mmal_port_format_commit(sys->input);
    if (status != MMAL_SUCCESS) {
        msg_Err(filter, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                        sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }
    sys->input->buffer_size = sys->input->buffer_size_recommended;
    sys->input->buffer_num = 30;
//    sys->input->buffer_num = sys->input->buffer_num_recommended;

    if ((sys->in_pool = mmal_pool_create(sys->input->buffer_num, 0)) == NULL)
    {
        msg_Err(filter, "Failed to create input pool");
        goto fail;
    }

    status = port_parameter_set_bool(sys->input, MMAL_PARAMETER_ZERO_COPY, true);
    if (status != MMAL_SUCCESS) {
       msg_Err(filter, "Failed to set zero copy on port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
       goto fail;
    }

    status = mmal_port_enable(sys->input, di_input_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(filter, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }


    if ((sys->out_q = mmal_queue_create()) == NULL)
    {
        msg_Err(filter, "Failed to create out Q");
        goto fail;
    }

    sys->output = sys->component->output[0];
    mmal_format_full_copy(sys->output->format, sys->input->format);

    if (!sys->is_cma)
    {
        if ((status = hw_mmal_opaque_output(VLC_OBJECT(filter), &sys->out_ppr, sys->output, 5, di_output_port_cb)) != MMAL_SUCCESS)
            goto fail;
    }
    else
    {
        // CMA stuff
        sys->output->userdata = (struct MMAL_PORT_USERDATA_T *)filter;

        if ((sys->cma_out_pool = cma_buf_pool_new(8, 8, true, "deinterlace")) == NULL)
        {
            msg_Err(filter, "Failed to alloc cma buf pool");
            goto fail;
        }

        // Rate control done by CMA in flight logic, so have "inexhaustable" pool here
        if ((sys->out_pool = mmal_pool_create(30, 0)) == NULL)
        {
            msg_Err(filter, "Failed to alloc out pool");
            goto fail;
        }

        port_parameter_set_bool(sys->output, MMAL_PARAMETER_ZERO_COPY, true);

        if ((status = mmal_port_format_commit(sys->output)) != MMAL_SUCCESS)
        {
            msg_Err(filter, "Output port format commit failed");
            goto fail;
        }

        sys->output->buffer_num = 30;
        sys->output->buffer_size = sys->output->buffer_size_recommended;

        // CB just drops all bufs into out_q
        if ((status = mmal_port_enable(sys->output, di_output_port_cb)) != MMAL_SUCCESS)
        {
            msg_Err(filter, "Failed to enable output port %s (status=%"PRIx32" %s)",
                    sys->output->name, status, mmal_status_to_string(status));
            goto fail;
        }
    }

    status = mmal_component_enable(sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(filter, "Failed to enable component %s (status=%"PRIx32" %s)",
                sys->component->name, status, mmal_status_to_string(status));
        goto fail;
    }

    filter->pf_video_filter = deinterlace;
    filter->pf_flush = di_flush;
    return 0;

fail:
    CloseMmalDeinterlace(filter);
    return ret;
}

vlc_module_begin()
    set_shortname(N_("MMAL deinterlace"))
    set_description(N_("MMAL-based deinterlace filter plugin"))
    set_capability("video filter", 900)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_callbacks(OpenMmalDeinterlace, CloseMmalDeinterlace)
    add_shortcut("deinterlace")
    add_bool(MMAL_DEINTERLACE_NO_QPU, false, MMAL_DEINTERLACE_NO_QPU_TEXT,
                    MMAL_DEINTERLACE_NO_QPU_LONGTEXT, true);
    add_bool(MMAL_DEINTERLACE_ADV, false, MMAL_DEINTERLACE_ADV_TEXT,
                    MMAL_DEINTERLACE_ADV_LONGTEXT, true);
    add_bool(MMAL_DEINTERLACE_FAST, false, MMAL_DEINTERLACE_FAST_TEXT,
                    MMAL_DEINTERLACE_FAST_LONGTEXT, true);
    add_bool(MMAL_DEINTERLACE_NONE, false, MMAL_DEINTERLACE_NONE_TEXT,
                    MMAL_DEINTERLACE_NONE_LONGTEXT, true);
    add_bool(MMAL_DEINTERLACE_HALF_RATE, false, MMAL_DEINTERLACE_HALF_RATE_TEXT,
                    MMAL_DEINTERLACE_HALF_RATE_LONGTEXT, true);
    add_bool(MMAL_DEINTERLACE_FULL_RATE, false, MMAL_DEINTERLACE_FULL_RATE_TEXT,
                    MMAL_DEINTERLACE_FULL_RATE_LONGTEXT, true);

vlc_module_end()


