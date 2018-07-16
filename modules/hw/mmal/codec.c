/*****************************************************************************
 * mmal.c: MMAL-based decoder plugin for Raspberry Pi
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Dennis Hamester <dennis.hamester@gmail.com>
 *          Julian Scheel <julian@jusst.de>
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

/*
 * This seems to be a bit high, but reducing it causes instabilities
 */
#define NUM_EXTRA_BUFFERS 10
#define NUM_DECODER_BUFFER_HEADERS 30

#define MIN_NUM_BUFFERS_IN_TRANSIT 2

#define MMAL_COMPONENT_DEFAULT_RESIZER "vc.ril.resize"
#define MMAL_COMPONENT_ISP_RESIZER "vc.ril.isp"

#define MMAL_OPAQUE_NAME "mmal-opaque"
#define MMAL_OPAQUE_TEXT N_("Decode frames directly into RPI VideoCore instead of host memory.")
#define MMAL_OPAQUE_LONGTEXT N_("Decode frames directly into RPI VideoCore instead of host memory. This option must only be used with the MMAL video output plugin.")

typedef struct decoder_sys_t
{
    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_POOL_T *input_pool;
    MMAL_PORT_T *output;
    hw_mmal_port_pool_ref_t *ppr;
    MMAL_ES_FORMAT_T *output_format;

    bool b_top_field_first;
    bool b_progressive;

    bool b_flushed;

    /* statistics */
    atomic_bool started;
} decoder_sys_t;


typedef struct supported_mmal_enc_s {
    struct {
       MMAL_PARAMETER_HEADER_T header;
       MMAL_FOURCC_T encodings[64];
    } supported;
    int n;
} supported_mmal_enc_t;

static supported_mmal_enc_t supported_mmal_enc =
{
    {{MMAL_PARAMETER_SUPPORTED_ENCODINGS, sizeof(((supported_mmal_enc_t *)0)->supported)}, {0}},
    -1
};

static const char * str_fourcc(char * buf, unsigned int fcc)
{
    if (fcc == 0)
        return "----";
    buf[0] = (fcc >> 0) & 0xff;
    buf[1] = (fcc >> 8) & 0xff;
    buf[2] = (fcc >> 16) & 0xff;
    buf[3] = (fcc >> 24) & 0xff;
    buf[4] = 0;
    return buf;
}


static bool is_enc_supported(const MMAL_FOURCC_T fcc)
{
    int i;

    if (fcc == 0)
        return false;
    if (supported_mmal_enc.n == -1)
        return true;  // Unknown - say OK
    for (i = 0; i < supported_mmal_enc.n; ++i) {
        if (supported_mmal_enc.supported.encodings[i] == fcc)
            return true;
    }
    return false;
}

static bool set_and_test_enc_supported(MMAL_PORT_T * port, const MMAL_FOURCC_T fcc)
{
    if (supported_mmal_enc.n >= 0)
        /* already done */;
    else if (mmal_port_parameter_get(port, (MMAL_PARAMETER_HEADER_T *)&supported_mmal_enc.supported) != MMAL_SUCCESS)
        supported_mmal_enc.n = 0;
    else
        supported_mmal_enc.n = (supported_mmal_enc.supported.header.size - sizeof(supported_mmal_enc.supported.header)) /
          sizeof(supported_mmal_enc.supported.encodings[0]);

    return is_enc_supported(fcc);
}

static MMAL_FOURCC_T vlc_to_mmal_es_fourcc(const unsigned int fcc)
{
    switch (fcc){
    case VLC_CODEC_MJPG:
        return MMAL_ENCODING_MJPEG;
    case VLC_CODEC_MP1V:
        return MMAL_ENCODING_MP1V;
    case VLC_CODEC_MPGV:
    case VLC_CODEC_MP2V:
        return MMAL_ENCODING_MP2V;
    case VLC_CODEC_H263:
        return MMAL_ENCODING_H263;
    case VLC_CODEC_MP4V:
        return MMAL_ENCODING_MP4V;
    case VLC_CODEC_H264:
        return MMAL_ENCODING_H264;
    case VLC_CODEC_VP6:
        return MMAL_ENCODING_VP6;
    case VLC_CODEC_VP8:
        return MMAL_ENCODING_VP8;
    case VLC_CODEC_WMV1:
        return MMAL_ENCODING_WMV1;
    case VLC_CODEC_WMV2:
        return MMAL_ENCODING_WMV2;
    case VLC_CODEC_WMV3:
        return MMAL_ENCODING_WMV3;
    case VLC_CODEC_VC1:
        return MMAL_ENCODING_WVC1;
    case VLC_CODEC_THEORA:
        return MMAL_ENCODING_THEORA;
    default:
        break;
    }
    return 0;
}

static MMAL_FOURCC_T vlc_to_mmal_pic_fourcc(const unsigned int fcc)
{
    switch (fcc){
    case VLC_CODEC_I420:
        return MMAL_ENCODING_I420;
    case VLC_CODEC_RGB32:
        return MMAL_ENCODING_BGRA;  // _RGB32 doesn't exist in mmal magic mapping table
    case VLC_CODEC_MMAL_OPAQUE:
        return MMAL_ENCODING_OPAQUE;
    default:
        break;
    }
    return 0;
}


// Buffer either attached to pic or released
static picture_t * alloc_opaque_pic(decoder_t * const dec, MMAL_BUFFER_HEADER_T * const buf)
{
    decoder_sys_t *const dec_sys = dec->p_sys;
    picture_t * const pic = decoder_NewPicture(dec);

    if (pic == NULL)
        goto fail1;

    if (buf->length == 0) {
        msg_Err(dec, "%s: Empty buffer", __func__);
        goto fail2;
    }

    if ((pic->context = hw_mmal_gen_context(buf, dec_sys->ppr, (vlc_object_t*)dec)) == NULL)
        goto fail2;

    buf_to_pic_copy_props(pic, buf);
    pic->b_force = true;

    msg_Dbg(dec, "pic: prog=%d, tff=%d, date=%lld", pic->b_progressive, pic->b_top_field_first, (long long)pic->date);

    return pic;

fail2:
    picture_Release(pic);
fail1:
    // * maybe should recycle rather than release?
    mmal_buffer_header_release(buf);
    return NULL;
}

static void control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    decoder_t *dec = (decoder_t *)port->userdata;
    MMAL_STATUS_T status;

    msg_Dbg(dec, "<<< %s: cmd=%d, data=%p", __func__, buffer->cmd, buffer->data);

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        status = *(uint32_t *)buffer->data;
        msg_Err(dec, "MMAL error %"PRIx32" \"%s\"", status,
                mmal_status_to_string(status));
    }

    mmal_buffer_header_release(buffer);
}

static void input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    block_t * const block = (block_t *)buffer->user_data;

    (void)port;  // Unused

    msg_Dbg((decoder_t *)port->userdata, "<<< %s: cmd=%d, data=%p, len=%d/%d, pts=%lld", __func__,
            buffer->cmd, buffer->data, buffer->length, buffer->alloc_size, (long long)buffer->pts);

    mmal_buffer_header_reset(buffer);
    mmal_buffer_header_release(buffer);

    if (block != NULL)
        block_Release(block);
}

static void output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // decoder structure only guaranteed valid if we have contents

    if (buffer->cmd == 0 && buffer->length != 0)
    {
        decoder_t * const dec = (decoder_t *)port->userdata;

        msg_Dbg((decoder_t *)port->userdata, "<<< %s: cmd=%d, data=%p, len=%d/%d, pts=%lld", __func__,
                buffer->cmd, buffer->data, buffer->length, buffer->alloc_size, (long long)buffer->pts);

        picture_t *pic = alloc_opaque_pic(dec, buffer);
        msg_Dbg(dec, "flags=%#x, video flags=%#x", buffer->flags, buffer->type->video.flags);
        if (pic == NULL)
            msg_Err(dec, "Failed to allocate new picture");
        else
            decoder_QueueVideo(dec, pic);
        // Buffer released or attached to pic - do not release again
        return;
    }
    else if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
    {
        decoder_t * const dec = (decoder_t *)port->userdata;
        decoder_sys_t * const sys = dec->p_sys;
        MMAL_EVENT_FORMAT_CHANGED_T * const fmt = mmal_event_format_changed_get(buffer);
        MMAL_ES_FORMAT_T * const format = mmal_format_alloc();

        if (format == NULL)
            msg_Err(dec, "Failed to allocate new format");
        else
        {
            mmal_format_full_copy(format, fmt->format);
            format->encoding = MMAL_ENCODING_OPAQUE;

            if (sys->output_format != NULL)
                mmal_format_free(sys->output_format);

            sys->output_format = format;
        }
    }

    mmal_buffer_header_reset(buffer);
    buffer->user_data = NULL;
    mmal_buffer_header_release(buffer);
}



static void fill_output_port(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;

    if (decoder_UpdateVideoFormat(dec) != 0)
    {
        // If we have a new format don't bother stuffing the buffer
        // We should get a reset RSN
        msg_Dbg(dec, "%s: Updated", __func__);

        return;
    }

    hw_mmal_port_pool_ref_fill(sys->ppr);
    return;
}

static int change_output_format(decoder_t *dec)
{
    MMAL_PARAMETER_VIDEO_INTERLACE_TYPE_T interlace_type;
    decoder_sys_t * const sys = dec->p_sys;
    MMAL_STATUS_T status;
    int ret = 0;

    msg_Dbg(dec, "%s: <<<", __func__);

    if (atomic_load(&sys->started)) {
        mmal_format_full_copy(sys->output->format, sys->output_format);
        status = mmal_port_format_commit(sys->output);
        if (status != MMAL_SUCCESS) {
            msg_Err(dec, "Failed to commit output format (status=%"PRIx32" %s)",
                    status, mmal_status_to_string(status));
            ret = -1;
            goto port_reset;
        }
        goto apply_fmt;
    }

port_reset:
    msg_Dbg(dec, "%s: Do full port reset", __func__);
    status = mmal_port_disable(sys->output);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to disable output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        ret = -1;
        goto out;
    }

    mmal_format_full_copy(sys->output->format, sys->output_format);
    status = mmal_port_format_commit(sys->output);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to commit output format (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        ret = -1;
        goto out;
    }

    sys->output->buffer_num = NUM_DECODER_BUFFER_HEADERS;
    sys->output->buffer_size = sys->output->buffer_size_recommended;

    status = mmal_port_enable(sys->output, output_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        ret = -1;
        goto out;
    }

    if (!atomic_load(&sys->started)) {
        atomic_store(&sys->started, true);

        /* we need one picture from vout for each buffer header on the output
         * port */
        dec->i_extra_picture_buffers = 10;
        msg_Dbg(dec, "Request %d extra pictures", dec->i_extra_picture_buffers);
    }

apply_fmt:
    dec->fmt_out.video.i_width = sys->output->format->es->video.width;
    dec->fmt_out.video.i_height = sys->output->format->es->video.height;
    dec->fmt_out.video.i_x_offset = sys->output->format->es->video.crop.x;
    dec->fmt_out.video.i_y_offset = sys->output->format->es->video.crop.y;
    dec->fmt_out.video.i_visible_width = sys->output->format->es->video.crop.width;
    dec->fmt_out.video.i_visible_height = sys->output->format->es->video.crop.height;
    dec->fmt_out.video.i_sar_num = sys->output->format->es->video.par.num;
    dec->fmt_out.video.i_sar_den = sys->output->format->es->video.par.den;
    dec->fmt_out.video.i_frame_rate = sys->output->format->es->video.frame_rate.num;
    dec->fmt_out.video.i_frame_rate_base = sys->output->format->es->video.frame_rate.den;

    /* Query interlaced type */
    interlace_type.hdr.id = MMAL_PARAMETER_VIDEO_INTERLACE_TYPE;
    interlace_type.hdr.size = sizeof(MMAL_PARAMETER_VIDEO_INTERLACE_TYPE_T);
    status = mmal_port_parameter_get(sys->output, &interlace_type.hdr);
    if (status != MMAL_SUCCESS) {
        msg_Warn(dec, "Failed to query interlace type from decoder output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
    } else {
        sys->b_progressive = (interlace_type.eMode == MMAL_InterlaceProgressive);
        sys->b_top_field_first = sys->b_progressive ? true :
            (interlace_type.eMode == MMAL_InterlaceFieldsInterleavedUpperFirst);
        msg_Dbg(dec, "Detected %s%s video (%d)",
                sys->b_progressive ? "progressive" : "interlaced",
                sys->b_progressive ? "" : (sys->b_top_field_first ? " tff" : " bff"),
                interlace_type.eMode);
    }

out:
    mmal_format_free(sys->output_format);
    sys->output_format = NULL;

    return ret;
}


static void flush_decoder(decoder_t *dec)
{
    decoder_sys_t *const sys = dec->p_sys;

    msg_Dbg(dec, "%s: <<<", __func__);

    if (!sys->b_flushed) {
        mmal_port_disable(sys->input);
        mmal_port_disable(sys->output);
        // We can leave the input disabled, but we want the output enabled
        // in order to sink any buffers returning from other modules
        mmal_port_enable(sys->output, output_port_cb);
        sys->b_flushed = true;
    }
    msg_Dbg(dec, "%s: >>>", __func__);
}

static int decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    MMAL_BUFFER_HEADER_T *buffer;
    uint32_t len;
    uint32_t flags = 0;
    MMAL_STATUS_T status;

    msg_Dbg(dec, "<<< %s: %lld/%lld", __func__, block == NULL ? -1LL : block->i_dts, block == NULL ? -1LL : block->i_pts);

    /*
     * Configure output port if necessary
     */
    if (sys->output_format) {
        if (change_output_format(dec) < 0)
            msg_Err(dec, "Failed to change output port format");
    }

    if (block == NULL)
        return VLCDEC_SUCCESS;

    /*
     * Check whether full flush is required
     */
    if (block->i_flags & BLOCK_FLAG_DISCONTINUITY) {
        msg_Dbg(dec, "%s: >>> Discontinuity", __func__);
        flush_decoder(dec);
    }

    if (block->i_buffer == 0)
    {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }

    // Reenable stuff if the last thing we did was flush
    if (!sys->output->is_enabled &&
        (status = mmal_port_enable(sys->output, output_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(dec, "Output port enable failed");
        goto fail;
    }

    if (!sys->input->is_enabled &&
        (status = mmal_port_enable(sys->input, input_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(dec, "Input port enable failed");
        goto fail;
    }

    // *** We cannot get a picture to put the result in 'till we have
    // reported the size & the output stages have been set up
    if (atomic_load(&sys->started))
        fill_output_port(dec);

    /*
     * Process input
     */

    if (block->i_flags & BLOCK_FLAG_CORRUPTED)
        flags |= MMAL_BUFFER_HEADER_FLAG_CORRUPTED;

    while (block != NULL)
    {
        buffer = mmal_queue_wait(sys->input_pool->queue);
        if (!buffer) {
            msg_Err(dec, "Failed to retrieve buffer header for input data");
            goto fail;
        }

        mmal_buffer_header_reset(buffer);
        buffer->cmd = 0;
        buffer->pts = block->i_pts != VLC_TICK_INVALID ? block->i_pts :
            block->i_dts != VLC_TICK_INVALID ? block->i_dts : MMAL_TIME_UNKNOWN;
        buffer->dts = block->i_dts;
        buffer->alloc_size = sys->input->buffer_size;
        buffer->user_data = NULL;

        len = block->i_buffer;
        if (len > buffer->alloc_size)
            len = buffer->alloc_size;

        buffer->data = block->p_buffer;
        block->p_buffer += len;
        block->i_buffer -= len;
        buffer->length = len;
        if (block->i_buffer == 0) {
            buffer->user_data = block;
            block = NULL;
        }
        buffer->flags = flags;

        msg_Dbg(dec, "%s: -- Send buffer: len=%d", __func__, len);
        status = mmal_port_send_buffer(sys->input, buffer);
        if (status != MMAL_SUCCESS) {
            msg_Err(dec, "Failed to send buffer to input port (status=%"PRIx32" %s)",
                    status, mmal_status_to_string(status));
            goto fail;
        }

        // Reset flushed flag once we have sent a buf
        sys->b_flushed = false;
    }
    return VLCDEC_SUCCESS;

fail:
    flush_decoder(dec);
    return VLCDEC_ECRITICAL;

}


static void CloseDecoder(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;

    msg_Dbg(dec, "%s: <<<", __func__);

    if (!sys)
        return;

    flush_decoder(dec);

    if (sys->component && sys->component->control->is_enabled)
        mmal_port_disable(sys->component->control);

    if (sys->component && sys->component->is_enabled)
        mmal_component_disable(sys->component);

    if (sys->input_pool)
        mmal_pool_destroy(sys->input_pool);

    if (sys->output_format)
        mmal_format_free(sys->output_format);

    hw_mmal_port_pool_ref_release(sys->ppr, false);
    sys->output->userdata = NULL;

    if (sys->component)
        mmal_component_release(sys->component);

    free(sys);

    bcm_host_deinit();
}

static int OpenDecoder(decoder_t *dec)
{
    int ret = VLC_EGENERIC;
    decoder_sys_t *sys;
    MMAL_STATUS_T status;
    const MMAL_FOURCC_T in_fcc = vlc_to_mmal_es_fourcc(dec->fmt_in.i_codec);

    {
        char buf1[5], buf2[5], buf2a[5];
        char buf3[5], buf4[5];
        msg_Dbg(dec, "%s: <<< (%s/%s)[%s] %dx%d -> (%s/%s) %dx%d", __func__,
                str_fourcc(buf1, dec->fmt_in.i_codec),
                str_fourcc(buf2, dec->fmt_in.video.i_chroma),
                str_fourcc(buf2a, in_fcc),
                dec->fmt_in.video.i_width, dec->fmt_in.video.i_height,
                str_fourcc(buf3, dec->fmt_out.i_codec),
                str_fourcc(buf4, dec->fmt_out.video.i_chroma),
                dec->fmt_out.video.i_width, dec->fmt_out.video.i_height);
    }

    if (!is_enc_supported(in_fcc))
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(decoder_sys_t));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    dec->p_sys = sys;

    bcm_host_init();

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->input = sys->component->input[0];
    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)dec;
    sys->input->format->encoding = in_fcc;

    if (!set_and_test_enc_supported(sys->input, in_fcc)) {
        char cbuf[5];
        msg_Dbg(dec, "Format not supported: %s", str_fourcc(cbuf, in_fcc));
        goto fail;
    }

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)dec;
    status = mmal_port_enable(sys->component->control, control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable control port %s (status=%"PRIx32" %s)",
                sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if (dec->fmt_in.i_codec == VLC_CODEC_H264) {
        if (dec->fmt_in.i_extra > 0) {
            status = mmal_format_extradata_alloc(sys->input->format,
                    dec->fmt_in.i_extra);
            if (status == MMAL_SUCCESS) {
                memcpy(sys->input->format->extradata, dec->fmt_in.p_extra,
                        dec->fmt_in.i_extra);
                sys->input->format->extradata_size = dec->fmt_in.i_extra;
            } else {
                msg_Err(dec, "Failed to allocate extra format data on input port %s (status=%"PRIx32" %s)",
                        sys->input->name, status, mmal_status_to_string(status));
            }
        }
    }

    status = mmal_port_format_commit(sys->input);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }
    sys->input->buffer_size = sys->input->buffer_size_recommended;
    sys->input->buffer_num = sys->input->buffer_num_recommended;

    status = mmal_port_enable(sys->input, input_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->output = sys->component->output[0];
    sys->output->userdata = (struct MMAL_PORT_USERDATA_T *)dec;

    status = port_parameter_set_uint32(sys->output, MMAL_PARAMETER_EXTRA_BUFFERS, NUM_EXTRA_BUFFERS);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to set MMAL_PARAMETER_EXTRA_BUFFERS on output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        goto fail;
    }

    status = port_parameter_set_bool(sys->output, MMAL_PARAMETER_ZERO_COPY, 1);
    if (status != MMAL_SUCCESS) {
       msg_Err(dec, "Failed to set zero copy on port %s (status=%"PRIx32" %s)",
                sys->output->name, status, mmal_status_to_string(status));
       goto fail;
    }

    sys->output->format->encoding = MMAL_ENCODING_OPAQUE;
    if ((status = mmal_port_format_commit(sys->output)) != MMAL_SUCCESS)
    {
        msg_Err(dec, "Failed to commit format on port %s (status=%"PRIx32" %s)",
                 sys->output->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->output->buffer_num = NUM_DECODER_BUFFER_HEADERS;
    sys->output->buffer_size = sys->output->buffer_size_recommended;

    sys->ppr = hw_mmal_port_pool_ref_create(sys->output, NUM_DECODER_BUFFER_HEADERS, sys->output->buffer_size);
    if (sys->ppr == NULL) {
        msg_Err(dec, "Failed to create output pool");
        goto fail;
    }

    status = mmal_port_enable(sys->output, output_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable output port %s (status=%"PRIx32" %s)",
                sys->output->name, status, mmal_status_to_string(status));
        goto fail;
    }

    status = mmal_component_enable(sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable component %s (status=%"PRIx32" %s)",
                sys->component->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if ((sys->input_pool = mmal_pool_create(sys->input->buffer_num, 0)) == NULL)
    {
        msg_Err(dec, "Failed to create input pool");
        goto fail;
    }

    sys->b_flushed = true;
    dec->fmt_out.i_codec = VLC_CODEC_MMAL_OPAQUE;
    dec->fmt_out.video.i_chroma = VLC_CODEC_MMAL_OPAQUE;

    dec->pf_decode = decode;
    dec->pf_flush  = flush_decoder;

    msg_Dbg(dec, ">>> %s: ok", __func__);
    return 0;

fail:
    CloseDecoder(dec);
    msg_Dbg(dec, ">>> %s: FAIL: ret=%d", __func__, ret);
    return ret;
}

// ----------------------------

typedef struct filter_sys_t {
    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_PORT_T *output;
    MMAL_POOL_T *out_pool;  // Free output buffers
    MMAL_POOL_T *in_pool;   // Input pool to get BH for replication

    picture_t * ret_pics;
    picture_t * ret_pics_tail;

    vlc_sem_t sem;
    vlc_mutex_t lock;

    bool b_top_field_first;
    bool b_progressive;

    int in_count;

    atomic_int out_port_count;
} filter_sys_t;

static void vlc_to_mmal_pic_fmt(MMAL_PORT_T * const port, const es_format_t * const es_vlc)
{
    const video_format_t *const vf_vlc = &es_vlc->video;
    MMAL_VIDEO_FORMAT_T * vf_mmal = &port->format->es->video;

    vf_mmal->width          = vf_vlc->i_width;
    vf_mmal->height         = vf_vlc->i_height;
    vf_mmal->crop.x         = vf_vlc->i_x_offset;
    vf_mmal->crop.y         = vf_vlc->i_y_offset;
    vf_mmal->crop.width     = vf_vlc->i_visible_width;
    vf_mmal->crop.height    = vf_vlc->i_visible_height;
    if (vf_vlc->i_sar_num == 0 || vf_vlc->i_sar_den == 0) {
        vf_mmal->par.num        = 1;
        vf_mmal->par.den        = 1;
    } else {
        vf_mmal->par.num        = vf_vlc->i_sar_num;
        vf_mmal->par.den        = vf_vlc->i_sar_den;
    }
    vf_mmal->frame_rate.num = vf_vlc->i_frame_rate;
    vf_mmal->frame_rate.den = vf_vlc->i_frame_rate_base;
}

static void conv_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    msg_Dbg((filter_t *)port->userdata, "%s: <<< cmd=%d, data=%p, pic=%p", __func__, buffer->cmd, buffer->data, buffer->user_data);

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        filter_t * const p_filter = (filter_t *)port->userdata;
        MMAL_STATUS_T status = *(uint32_t *)buffer->data;

        msg_Err(p_filter, "MMAL error %"PRIx32" \"%s\"", status,
                mmal_status_to_string(status));
    }

    mmal_buffer_header_release(buffer);
}

#include "../../../src/misc/picture.h"

static void conv_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    picture_context_t * ctx = buf->user_data;
//    filter_sys_t *const sys = ((filter_t *)port->userdata)->p_sys;

    msg_Dbg((filter_t *)port->userdata, "<<< %s cmd=%d, ctx=%p, buf=%p, flags=%#x, len=%d/%d, pts=%lld",
            __func__, buf->cmd, ctx, buf, buf->flags, buf->length, buf->alloc_size, (long long)buf->pts);

    mmal_buffer_header_release(buf);

    msg_Dbg((filter_t *)port->userdata, ">>> %s", __func__);
}

static void conv_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    filter_t * const p_filter = (filter_t *)port->userdata;
    filter_sys_t * const sys = p_filter->p_sys;
    int n = atomic_fetch_sub(&sys->out_port_count, 1);

    msg_Dbg(p_filter, "<<< %s[%d]: <<< cmd=%d, flags=%#x, pic=%p, data=%p, len=%d/%d, pts=%lld", __func__, n,
            buf->cmd, buf->flags, buf->user_data, buf->data, buf->length, buf->alloc_size, (long long)buf->pts);

    if (buf->cmd == 0) {
        picture_t * const pic = (picture_t *)buf->user_data;

        if (pic == NULL) {
            msg_Err(p_filter, "%s: Buffer has no attached picture", __func__);
        }
        else if (buf->data == NULL || buf->length == 0)
        {
            msg_Dbg(p_filter, "%s: Buffer has no data", __func__);
            picture_Release(pic);
        }
        else
        {
            buf_to_pic_copy_props(pic, buf);

            pic->p_next = NULL;

            vlc_mutex_lock(&sys->lock);
            if (sys->ret_pics_tail == NULL)
                sys->ret_pics = pic;
            else
                sys->ret_pics_tail->p_next = pic;
            sys->ret_pics_tail = pic;
            vlc_mutex_unlock(&sys->lock);

            vlc_sem_post(&sys->sem);
        }
    }

    buf->user_data = NULL; // Zap here to make sure we can't reuse later
    mmal_buffer_header_release(buf);
}



static picture_t *conv_filter(filter_t *p_filter, picture_t *p_pic)
{
    filter_sys_t * const sys = p_filter->p_sys;
    picture_t * ret_pics;
    MMAL_STATUS_T err;

    msg_Dbg(p_filter, "<<< %s", __func__);

    // Reenable stuff if the last thing we did was flush
    if (!sys->output->is_enabled &&
        (err = mmal_port_enable(sys->output, conv_output_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(p_filter, "Output port enable failed");
        goto fail;
    }

    if (!sys->input->is_enabled &&
        (err = mmal_port_enable(sys->input, conv_input_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(p_filter, "Input port enable failed");
        goto fail;
    }

    // Stuff into input
    // We assume the BH is already set up with values reflecting pic date etc.
    {
        MMAL_BUFFER_HEADER_T * const pic_buf = pic_mmal_buffer(p_pic);
        MMAL_BUFFER_HEADER_T *const buf = mmal_queue_wait(sys->in_pool->queue);

        if ((err = mmal_buffer_header_replicate(buf, pic_buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Failed to replicate input buffer: %d", err);
            goto fail;
        }

        msg_Dbg(p_filter, "In buf send: pic=%p, buf=%p/%p, ctx=%p, flags=%#x, len=%d/%d, pts=%lld",
                p_pic, pic_buf, buf, pic_buf->user_data, buf->flags, buf->length, buf->alloc_size, (long long)buf->pts);

        picture_Release(p_pic);

        if ((err = mmal_port_send_buffer(sys->input, buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Send buffer to input failed");
            goto fail;
        }

        --sys->in_count;
    }

    // Poke return pic buffer into output
    {
        MMAL_BUFFER_HEADER_T * out_buf;

        while ((out_buf = sys->in_count < 0 ?
                mmal_queue_wait(sys->out_pool->queue) : mmal_queue_get(sys->out_pool->queue)) != NULL)
        {
            picture_t * const out_pic = filter_NewPicture( p_filter );

            if (out_pic == NULL) {
                msg_Warn(p_filter, "Failed to alloc new filter output pic");
                mmal_buffer_header_release(out_buf);
                break;
            }

            mmal_buffer_header_reset(out_buf);
            out_buf->user_data = out_pic;
            out_buf->data = out_pic->p[0].p_pixels;
            out_buf->alloc_size = out_pic->p[0].i_pitch * out_pic->p[0].i_lines;
            //**** stride ????

            msg_Dbg(p_filter, "Out buf send: pic=%p, buf=%p, flags=%#x, len=%d/%d, pts=%lld",
                    p_pic, out_buf->user_data, out_buf->flags,
                    out_buf->length, out_buf->alloc_size, (long long)out_buf->pts);

            if ((err = mmal_port_send_buffer(sys->output, out_buf)) != MMAL_SUCCESS)
            {
                msg_Err(p_filter, "Send buffer to output failed");
                mmal_buffer_header_release(out_buf);
                break;
            }

            ++sys->in_count;
        }
    }

    if (sys->in_count < 0)
        goto fail;

    // Avoid being more than 1 pic behind
    vlc_sem_wait(&sys->sem);

    // Return all pending buffers
    vlc_mutex_lock(&sys->lock);
    ret_pics = sys->ret_pics;
    sys->ret_pics = NULL;
    sys->ret_pics_tail = NULL;
    vlc_mutex_unlock(&sys->lock);

    // Sink as many sem posts as we have pics
    // (shouldn't normally wait, but there is a small race)
    if (ret_pics != NULL)
    {
        picture_t *next_pic = ret_pics->p_next;
        while (next_pic != NULL) {
            vlc_sem_wait(&sys->sem);
            next_pic = next_pic->p_next;
        }
    }

    msg_Dbg(p_filter, ">>> %s: pic=%p", __func__, ret_pics);

    return ret_pics;

fail:
    picture_Release(p_pic);
    return NULL;
}


static void conv_flush(filter_t * p_filter)
{
    filter_sys_t * const sys = p_filter->p_sys;

    msg_Dbg(p_filter, "<<< %s", __func__);

    if (sys->input != NULL && sys->input->is_enabled)
        mmal_port_disable(sys->input);

    if (sys->output != NULL && sys->output->is_enabled)
        mmal_port_disable(sys->output);

    // Free up anything we may have already lying around
    // Don't need lock as the above disables should have prevented anything
    // happening in the background
    {
        picture_t * ret_pics = sys->ret_pics;
        sys->ret_pics = NULL;
        sys->ret_pics_tail = NULL;

        while (ret_pics != NULL) {
            picture_t * const pic = ret_pics;
            ret_pics = pic->p_next;
            picture_Release(pic);
            vlc_sem_wait(&sys->sem);  // Drain sem
        }
    }
    // No buffers in either port now
    sys->in_count = 0;

    msg_Dbg(p_filter, ">>> %s", __func__);
}

static void CloseConverter(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    filter_sys_t * const sys = p_filter->p_sys;

    msg_Dbg(obj, "<<< %s", __func__);

    if (sys == NULL)
        return;

    // Disables input & output ports
    conv_flush(p_filter);

    if (sys->component && sys->component->control->is_enabled)
        mmal_port_disable(sys->component->control);

    if (sys->component && sys->component->is_enabled)
        mmal_component_disable(sys->component);

    if (sys->out_pool)
        mmal_pool_destroy(sys->out_pool);

    if (sys->in_pool != NULL)
        mmal_pool_destroy(sys->in_pool);

    if (sys->component)
        mmal_component_release(sys->component);

    vlc_sem_destroy(&sys->sem);
    vlc_mutex_destroy(&sys->lock);

    free(sys);
}


static int conv_set_output(filter_t * const p_filter, filter_sys_t * const sys, picture_t * const pic)
{
    MMAL_STATUS_T status;

    sys->output = sys->component->output[0];
    sys->output->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    sys->output->format->type = MMAL_ES_TYPE_VIDEO;
    sys->output->format->encoding = vlc_to_mmal_pic_fourcc(p_filter->fmt_out.i_codec);
    sys->output->format->encoding_variant = sys->output->format->encoding;
    vlc_to_mmal_pic_fmt(sys->output, &p_filter->fmt_out);

    if (pic != NULL) {
        unsigned int bpp = (pic->format.i_bits_per_pixel + 7) >> 3;
        sys->output->format->es->video.width = pic->p[0].i_pitch / bpp;
    }

    mmal_log_dump_format(sys->output->format);

    status = mmal_port_format_commit(sys->output);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to commit format for output port %s (status=%"PRIx32" %s)",
                sys->output->name, status, mmal_status_to_string(status));
        return -1;
    }

    sys->output->buffer_num = __MAX(2, sys->output->buffer_num_recommended);
    sys->output->buffer_size = sys->output->buffer_size_recommended;

    status = mmal_port_enable(sys->output, conv_output_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable output port %s (status=%"PRIx32" %s)",
                sys->output->name, status, mmal_status_to_string(status));
        return -1;
    }
    return 0;
}

static int OpenConverter(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    int ret = VLC_EGENERIC;
    filter_sys_t *sys;
    MMAL_STATUS_T status;
    MMAL_FOURCC_T enc_out;
    const MMAL_FOURCC_T enc_in = MMAL_ENCODING_OPAQUE;

    char dbuf0[5], dbuf1[5];
    msg_Dbg(p_filter, "%s: %s,%dx%d->%s,%dx%d", __func__,
            str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma), p_filter->fmt_in.video.i_height, p_filter->fmt_in.video.i_width,
            str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma), p_filter->fmt_out.video.i_height, p_filter->fmt_out.video.i_width);

    if (enc_in != vlc_to_mmal_pic_fourcc(p_filter->fmt_in.i_codec) ||
        (enc_out = vlc_to_mmal_pic_fourcc(p_filter->fmt_out.i_codec)) == 0)
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(filter_sys_t));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    p_filter->p_sys = sys;

    vlc_mutex_init(&sys->lock);

    status = mmal_component_create(MMAL_COMPONENT_ISP_RESIZER, &sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    status = mmal_port_enable(sys->component->control, conv_control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable control port %s (status=%"PRIx32" %s)",
                sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->input = sys->component->input[0];
    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    sys->input->format->type = MMAL_ES_TYPE_VIDEO;
    sys->input->format->encoding = enc_in;
    sys->input->format->encoding_variant = MMAL_ENCODING_I420;
    vlc_to_mmal_pic_fmt(sys->input, &p_filter->fmt_in);
    port_parameter_set_bool(sys->input, MMAL_PARAMETER_ZERO_COPY, 1);

    mmal_log_dump_format(sys->input->format);

    status = mmal_port_format_commit(sys->input);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }
    sys->input->buffer_size = sys->input->buffer_size_recommended;
    sys->input->buffer_num = NUM_DECODER_BUFFER_HEADERS;

    status = mmal_port_enable(sys->input, conv_input_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }

    {
        picture_t *pic = filter_NewPicture(p_filter);
        int err = conv_set_output(p_filter, sys, pic);
        picture_Release(pic);
        if (err != 0) {
            goto fail;
        }
    }

    status = mmal_component_enable(sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable component %s (status=%"PRIx32" %s)",
                sys->component->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->out_pool = mmal_pool_create(sys->output->buffer_num, 0);
    sys->in_pool = mmal_pool_create(sys->input->buffer_num, 0);

    vlc_sem_init(&sys->sem, 1);

    p_filter->pf_video_filter = conv_filter;
    p_filter->pf_flush = conv_flush;
    // video_drain NIF in filter structure

    msg_Dbg(p_filter, ">>> %s: ok", __func__);

    return VLC_SUCCESS;

fail:
    CloseConverter(obj);
    msg_Dbg(p_filter, ">>> %s: FAIL: %d", __func__, ret);
    return ret;
}

vlc_module_begin()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_shortname(N_("MMAL decoder"))
    set_description(N_("MMAL-based decoder plugin for Raspberry Pi"))
    set_capability("video decoder", 90)
    add_shortcut("mmal_decoder")
    add_bool(MMAL_OPAQUE_NAME, true, MMAL_OPAQUE_TEXT, MMAL_OPAQUE_LONGTEXT, false)
    set_callbacks(OpenDecoder, CloseDecoder)

    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("MMAL converterer"))
    set_description(N_("MMAL conversion filter"))
    add_shortcut("mmal_converter")
    set_capability( "video converter", 900 )
    set_callbacks(OpenConverter, CloseConverter)

vlc_module_end()


