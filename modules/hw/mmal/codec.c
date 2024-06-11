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
#include <vlc_cpu.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_filter.h>
#include <vlc_threads.h>

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#include <interface/vcsm/user-vcsm.h>

#include "mmal_cma.h"
#include "mmal_picture.h"

#include "subpic.h"
#include "blend_rgba_neon.h"

#define TRACE_ALL 0

#define OPT_TO_FROM_ZC 0

/*
 * This seems to be a bit high, but reducing it causes instabilities
 */
#define NUM_EXTRA_BUFFERS 5
//#define NUM_EXTRA_BUFFERS 10
#define NUM_DECODER_BUFFER_HEADERS 30

#define CONVERTER_BUFFERS 4  // Buffers on the output of the converter

#define MMAL_SLICE_HEIGHT 16
#define MMAL_ALIGN_W      32
#define MMAL_ALIGN_H      16

#define MMAL_OPAQUE_NAME "mmal-opaque"
#define MMAL_OPAQUE_TEXT N_("Decode frames directly into RPI VideoCore instead of host memory.")
#define MMAL_OPAQUE_LONGTEXT N_("Decode frames directly into RPI VideoCore instead of host memory. This option must only be used with the MMAL video output plugin.")

#define MMAL_RESIZE_NAME "mmal-resize"
#define MMAL_RESIZE_TEXT N_("Use mmal resizer rather than hvs.")
#define MMAL_RESIZE_LONGTEXT N_("Use mmal resizer rather than isp. This uses less gpu memory than the ISP but is slower.")

#define MMAL_ISP_NAME "mmal-isp"
#define MMAL_ISP_TEXT N_("Use mmal isp rather than hvs.")
#define MMAL_ISP_LONGTEXT N_("Use mmal isp rather than hvs. This may be faster but has no blend.")

#define MMAL_DECODE_ENABLE_NAME "mmal-decode-enable"
#define MMAL_DECODE_ENABLE_TEXT N_("Enable mmal decode even if normally disabled")
#define MMAL_DECODE_ENABLE_LONGTEXT N_("Enable mmal decode even if normally disabled. MMAL decode is normally disabled on Pi4 or later.")

typedef struct decoder_sys_t
{
    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_POOL_T *input_pool;
    MMAL_PORT_T *output;
    hw_mmal_port_pool_ref_t *ppr;
    MMAL_ES_FORMAT_T *output_format;

    MMAL_STATUS_T err_stream;
    bool b_top_field_first;
    bool b_progressive;

    bool b_flushed;

    vcsm_init_type_t vcsm_init_type;

    // Lock to avoid pic update & allocate happenening simultainiously
    // * We should be able to arrange life s.t. this isn't needed
    //   but while we are confused apply belt & braces
    vlc_mutex_t pic_lock;

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

#define SUPPORTED_MMAL_ENC_INIT \
{ \
    {{MMAL_PARAMETER_SUPPORTED_ENCODINGS, sizeof(((supported_mmal_enc_t *)0)->supported)}, {0}}, \
    -1 \
}

static supported_mmal_enc_t supported_decode_in_enc = SUPPORTED_MMAL_ENC_INIT;

static bool is_enc_supported(supported_mmal_enc_t * const support, const MMAL_FOURCC_T fcc)
{
    int i;

    if (fcc == 0)
        return false;
    if (support->n == -1)
        return true;  // Unknown - say OK
    for (i = 0; i < support->n; ++i) {
        if (support->supported.encodings[i] == fcc)
            return true;
    }
    return false;
}

static bool set_and_test_enc_supported(supported_mmal_enc_t * const support, MMAL_PORT_T * port, const MMAL_FOURCC_T fcc)
{
    if (support->n >= 0)
        /* already done */;
    else if (mmal_port_parameter_get(port, (MMAL_PARAMETER_HEADER_T *)&support->supported) != MMAL_SUCCESS)
        support->n = 0;
    else
        support->n = (support->supported.header.size - sizeof(support->supported.header)) /
          sizeof(support->supported.encodings[0]);

    return is_enc_supported(support, fcc);
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

static MMAL_FOURCC_T pic_to_slice_mmal_fourcc(const MMAL_FOURCC_T fcc)
{
    switch (fcc){
    case MMAL_ENCODING_I420:
        return MMAL_ENCODING_I420_SLICE;
    case MMAL_ENCODING_I422:
        return MMAL_ENCODING_I422_SLICE;
    case MMAL_ENCODING_ARGB:
        return MMAL_ENCODING_ARGB_SLICE;
    case MMAL_ENCODING_RGBA:
        return MMAL_ENCODING_RGBA_SLICE;
    case MMAL_ENCODING_ABGR:
        return MMAL_ENCODING_ABGR_SLICE;
    case MMAL_ENCODING_BGRA:
        return MMAL_ENCODING_BGRA_SLICE;
    case MMAL_ENCODING_RGB16:
        return MMAL_ENCODING_RGB16_SLICE;
    case MMAL_ENCODING_RGB24:
        return MMAL_ENCODING_RGB24_SLICE;
    case MMAL_ENCODING_RGB32:
        return MMAL_ENCODING_RGB32_SLICE;
    case MMAL_ENCODING_BGR16:
        return MMAL_ENCODING_BGR16_SLICE;
    case MMAL_ENCODING_BGR24:
        return MMAL_ENCODING_BGR24_SLICE;
    case MMAL_ENCODING_BGR32:
        return MMAL_ENCODING_BGR32_SLICE;
    default:
        break;
    }
    return 0;
}

#define DEBUG_SQUARES 0
#if DEBUG_SQUARES
static void draw_square(void * pic_buf, size_t pic_stride, unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t val)
{
    uint32_t * p = (uint32_t *)pic_buf + y * pic_stride + x;
    unsigned int i;
    for (i = 0; i != h; ++i) {
        unsigned int j;
        for (j = 0; j != w; ++j) {
            p[j] = val;
        }
        p += pic_stride;
    }
}
#endif

#if 0
static inline void draw_line(void * pic_buf, size_t pic_stride, unsigned int x, unsigned int y, unsigned int len, int inc)
{
    uint32_t * p = (uint32_t *)pic_buf + y * pic_stride + x;
    while (len-- != 0) {
        *p = ~0U;
        p += inc;
    }
}


static void draw_corners(void * pic_buf, size_t pic_stride, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
    const unsigned int len = 20;
    draw_line(pic_buf, pic_stride, x, y, len, 1);
    draw_line(pic_buf, pic_stride, x, y, len, pic_stride);
    draw_line(pic_buf, pic_stride, x + w - 1, y, len, -1);
    draw_line(pic_buf, pic_stride, x + w - 1, y, len, pic_stride);
    draw_line(pic_buf, pic_stride, x + w - 1, y + h - 1, len, -1);
    draw_line(pic_buf, pic_stride, x + w - 1, y + h - 1, len, -(int)pic_stride);
    draw_line(pic_buf, pic_stride, x, y + h - 1, len, 1);
    draw_line(pic_buf, pic_stride, x, y + h - 1, len, -(int)pic_stride);
}
#endif

static MMAL_RATIONAL_T
rationalize_sar(unsigned int num, unsigned int den)
{
    static const unsigned int primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 0};
    const unsigned int * p = primes;

    // If either num or den is 0 then return a well formed "unknown"
    if (num == 0 || den == 0) {
        return (MMAL_RATIONAL_T){.num = 0, .den = 0};
    }

    while (*p != 0 && num >= *p && den >= *p) {
        if (num % *p != 0 || den % *p != 0)
            ++p;
        else {
            num /= *p;
            den /= *p;
        }
    }
    return (MMAL_RATIONAL_T){.num = num, .den = den};
}

// Buffer either attached to pic or released
static picture_t * alloc_opaque_pic(decoder_t * const dec, MMAL_BUFFER_HEADER_T * const buf)
{
    decoder_sys_t *const dec_sys = dec->p_sys;

    vlc_mutex_lock(&dec_sys->pic_lock);
    picture_t * const pic = decoder_NewPicture(dec);
    vlc_mutex_unlock(&dec_sys->pic_lock);

    if (pic == NULL)
        goto fail1;

    if (buf->length == 0) {
        msg_Err(dec, "%s: Empty buffer", __func__);
        goto fail2;
    }

    if ((pic->context = hw_mmal_gen_context(buf, dec_sys->ppr)) == NULL)
        goto fail2;

    buf_to_pic_copy_props(pic, buf);

#if TRACE_ALL
    msg_Dbg(dec, "pic: prog=%d, tff=%d, date=%lld", pic->b_progressive, pic->b_top_field_first, (long long)pic->date);
#endif

    return pic;

fail2:
    picture_Release(pic);
fail1:
    // Recycle rather than release to avoid buffer starvation if NewPic fails
    hw_mmal_port_pool_ref_recycle(dec_sys->ppr, buf);
    return NULL;
}

static void control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    decoder_t *dec = (decoder_t *)port->userdata;
    MMAL_STATUS_T status;

#if TRACE_ALL
    msg_Dbg(dec, "<<< %s: cmd=%d, data=%p", __func__, buffer->cmd, buffer->data);
#endif

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        status = *(uint32_t *)buffer->data;
        dec->p_sys->err_stream = status;
        msg_Err(dec, "MMAL error %"PRIx32" \"%s\"", status,
                mmal_status_to_string(status));
    }

    mmal_buffer_header_release(buffer);
}

static void input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    block_t * const block = (block_t *)buffer->user_data;

    (void)port;  // Unused

#if TRACE_ALL
    msg_Dbg((decoder_t *)port->userdata, "<<< %s: cmd=%d, data=%p, len=%d/%d, pts=%lld", __func__,
            buffer->cmd, buffer->data, buffer->length, buffer->alloc_size, (long long)buffer->pts);
#endif

    mmal_buffer_header_reset(buffer);
    mmal_buffer_header_release(buffer);

    if (block != NULL)
        block_Release(block);
}

static void decoder_output_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    decoder_t * const dec = (decoder_t *)port->userdata;

    if (buffer->cmd == 0 && buffer->length != 0)
    {
#if TRACE_ALL
        msg_Dbg(dec, "<<< %s: cmd=%d, data=%p, len=%d/%d, pts=%lld", __func__,
                buffer->cmd, buffer->data, buffer->length, buffer->alloc_size, (long long)buffer->pts);
#endif

        picture_t *pic = alloc_opaque_pic(dec, buffer);
#if TRACE_ALL
        msg_Dbg(dec, "flags=%#x, video flags=%#x", buffer->flags, buffer->type->video.flags);
#endif
        if (pic == NULL)
            msg_Err(dec, "Failed to allocate new picture");
        else
            decoder_QueueVideo(dec, pic);
        // Buffer released or attached to pic - do not release again
        return;
    }

    if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
    {
        decoder_sys_t * const sys = dec->p_sys;
        MMAL_EVENT_FORMAT_CHANGED_T * const fmt = mmal_event_format_changed_get(buffer);
        MMAL_ES_FORMAT_T * const format = mmal_format_alloc();

        if (format == NULL)
            msg_Err(dec, "Failed to allocate new format");
        else
        {
            mmal_format_full_copy(format, fmt->format);
            format->encoding = MMAL_ENCODING_OPAQUE;

            // If no PAR in the stream - see if we've got one from the demux
            if (format->es->video.par.den <= 0 || format->es->video.par.num <= 0) {
                unsigned int n = dec->fmt_in.video.i_sar_num;
                unsigned int d = dec->fmt_in.video.i_sar_den;

                if (n == 0 || d == 0) {
                    // Guesswork required
                    const unsigned int w = format->es->video.width;
                    const unsigned int h = format->es->video.height;
                    if ((w == 704 || w == 720) && (h == 480 || h == 576)) {
                        // Very likely SD 4:3
                        n = w * 3;
                        d = h * 4;
                    }
                    else
                    {
                        // Otherwise guess SAR 1:1
                        n = 1;
                        d = 1;
                    }
                }

                format->es->video.par = rationalize_sar(n, d);
            }

            if (sys->output_format != NULL)
                mmal_format_free(sys->output_format);

            sys->output_format = format;
        }
    }
    else if (buffer->cmd != 0) {
        char buf0[5];
        msg_Warn(dec, "Unexpected output cb event: %s", str_fourcc(buf0, buffer->cmd));
    }

    // If we get here then we were flushing (cmd == 0 && len == 0) or
    // that was an EVENT - in either case we want to release the buffer
    // back to its pool rather than recycle it.
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
#if TRACE_ALL
        msg_Dbg(dec, "%s: Updated", __func__);
#endif

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

#if TRACE_ALL
    msg_Dbg(dec, "%s: <<<", __func__);
#endif

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
#if TRACE_ALL
    msg_Dbg(dec, "%s: Do full port reset", __func__);
#endif
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

    status = mmal_port_enable(sys->output, decoder_output_cb);
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
#if TRACE_ALL
        msg_Dbg(dec, "Request %d extra pictures", dec->i_extra_picture_buffers);
#endif
    }

apply_fmt:
    dec->fmt_out.video.i_width = sys->output->format->es->video.width;
    dec->fmt_out.video.i_height = sys->output->format->es->video.height;
    dec->fmt_out.video.i_x_offset = sys->output->format->es->video.crop.x;
    dec->fmt_out.video.i_y_offset = sys->output->format->es->video.crop.y;
    dec->fmt_out.video.i_visible_width = sys->output->format->es->video.crop.width;
    dec->fmt_out.video.i_visible_height = sys->output->format->es->video.crop.height;
    dec->fmt_out.video.i_sar_num = sys->output_format->es->video.par.num;  // SAR can be killed by commit
    dec->fmt_out.video.i_sar_den = sys->output_format->es->video.par.den;
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
#if TRACE_ALL
        msg_Dbg(dec, "Detected %s%s video (%d)",
                sys->b_progressive ? "progressive" : "interlaced",
                sys->b_progressive ? "" : (sys->b_top_field_first ? " tff" : " bff"),
                interlace_type.eMode);
#endif
    }

    // Tell the rest of the world we have changed format
    vlc_mutex_lock(&sys->pic_lock);
    ret = decoder_UpdateVideoFormat(dec);
    vlc_mutex_unlock(&sys->pic_lock);

out:
    mmal_format_free(sys->output_format);
    sys->output_format = NULL;

    return ret;
}

static MMAL_STATUS_T
set_extradata_and_commit(decoder_t * const dec, decoder_sys_t * const sys)
{
    MMAL_STATUS_T status;

    status = mmal_port_format_commit(sys->input);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
    }
    return status;
}

static MMAL_STATUS_T decoder_send_extradata(decoder_t * const dec, decoder_sys_t *const sys)
{
    if (dec->fmt_in.i_codec == VLC_CODEC_H264 &&
        dec->fmt_in.i_extra > 0)
    {
        MMAL_BUFFER_HEADER_T * const buf = mmal_queue_wait(sys->input_pool->queue);
        MMAL_STATUS_T status;

        mmal_buffer_header_reset(buf);
        buf->cmd = 0;
        buf->user_data = NULL;
        buf->alloc_size = sys->input->buffer_size;
        buf->length = dec->fmt_in.i_extra;
        buf->data = dec->fmt_in.p_extra;
        buf->flags = MMAL_BUFFER_HEADER_FLAG_CONFIG;

        status = mmal_port_send_buffer(sys->input, buf);
        if (status != MMAL_SUCCESS) {
            msg_Err(dec, "Failed to send extradata buffer to input port (status=%"PRIx32" %s)",
                    status, mmal_status_to_string(status));
            return status;
        }
    }

    return MMAL_SUCCESS;
}

static void flush_decoder(decoder_t *dec)
{
    decoder_sys_t *const sys = dec->p_sys;

#if TRACE_ALL
    msg_Dbg(dec, "%s: <<<", __func__);
#endif

    if (!sys->b_flushed) {
        mmal_port_disable(sys->input);
        mmal_port_disable(sys->output);
        // We can leave the input disabled, but we want the output enabled
        // in order to sink any buffers returning from other modules
        mmal_port_enable(sys->output, decoder_output_cb);
        sys->b_flushed = true;
    }
#if TRACE_ALL
    msg_Dbg(dec, "%s: >>>", __func__);
#endif
}

static int decode(decoder_t *dec, block_t *block)
{
    decoder_sys_t *sys = dec->p_sys;
    MMAL_BUFFER_HEADER_T *buffer;
    uint32_t len;
    uint32_t flags = MMAL_BUFFER_HEADER_FLAG_FRAME_START;
    MMAL_STATUS_T status;

#if TRACE_ALL
    msg_Dbg(dec, "<<< %s: %lld/%lld", __func__, block == NULL ? -1LL : block->i_dts, block == NULL ? -1LL : block->i_pts);
#endif

    if (sys->err_stream != MMAL_SUCCESS) {
        msg_Err(dec, "MMAL error reported by ctrl");
        flush_decoder(dec);
        return VLCDEC_ECRITICAL;  /// I think they are all fatal
    }

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
#if TRACE_ALL
        msg_Dbg(dec, "%s: >>> Discontinuity", __func__);
#endif
        flush_decoder(dec);
    }

    if (block->i_buffer == 0)
    {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }

    // Reenable stuff if the last thing we did was flush
    if (!sys->output->is_enabled &&
        (status = mmal_port_enable(sys->output, decoder_output_cb)) != MMAL_SUCCESS)
    {
        msg_Err(dec, "Output port enable failed");
        goto fail;
    }

    if (!sys->input->is_enabled)
    {
        if ((status = set_extradata_and_commit(dec, sys)) != MMAL_SUCCESS)
            goto fail;

        if ((status = mmal_port_enable(sys->input, input_port_cb)) != MMAL_SUCCESS)
        {
            msg_Err(dec, "Input port enable failed");
            goto fail;
        }

        if ((status = decoder_send_extradata(dec, sys)) != MMAL_SUCCESS)
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
            flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;
            if (block->i_flags & BLOCK_FLAG_END_OF_SEQUENCE) {
                msg_Dbg(dec, "EOS sent");
                flags |= MMAL_BUFFER_HEADER_FLAG_EOS;
            }
            buffer->user_data = block;
            block = NULL;
        }
        buffer->flags = flags;

#if TRACE_ALL
        msg_Dbg(dec, "%s: -- Send buffer: cmd=%d, data=%p, size=%d, len=%d, offset=%d, flags=%#x, pts=%lld, dts=%lld", __func__,\
                buffer->cmd, buffer->data, buffer->alloc_size, buffer->length, buffer->offset,
                buffer->flags, (long long)buffer->pts, (long long)buffer->dts);
#endif
        status = mmal_port_send_buffer(sys->input, buffer);
        if (status != MMAL_SUCCESS) {
            msg_Err(dec, "Failed to send buffer to input port (status=%"PRIx32" %s)",
                    status, mmal_status_to_string(status));
            goto fail;
        }

        // Reset flushed flag once we have sent a buf
        sys->b_flushed = false;
        flags &= ~MMAL_BUFFER_HEADER_FLAG_FRAME_START;
    }
    return VLCDEC_SUCCESS;

fail:
    flush_decoder(dec);
    return VLCDEC_ECRITICAL;

}


static void CloseDecoder(decoder_t *dec)
{
    decoder_sys_t *sys = dec->p_sys;

#if TRACE_ALL
    msg_Dbg(dec, "%s: <<<", __func__);
#endif

    if (!sys)
        return;

    if (sys->component != NULL) {
        if (sys->input->is_enabled)
            mmal_port_disable(sys->input);

        if (sys->output->is_enabled)
            mmal_port_disable(sys->output);

        if (sys->component->control->is_enabled)
            mmal_port_disable(sys->component->control);

        if (sys->component->is_enabled)
            mmal_component_disable(sys->component);

        mmal_component_release(sys->component);
    }

    if (sys->input_pool != NULL)
        mmal_pool_destroy(sys->input_pool);

    if (sys->output_format != NULL)
        mmal_format_free(sys->output_format);

    hw_mmal_port_pool_ref_release(sys->ppr, false);

    cma_vcsm_exit(sys->vcsm_init_type);

    vlc_mutex_destroy(&sys->pic_lock);
    free(sys);
}

static int OpenDecoder(decoder_t *dec)
{
    int ret = VLC_EGENERIC;
    decoder_sys_t *sys;
    MMAL_STATUS_T status;
    const MMAL_FOURCC_T in_fcc = vlc_to_mmal_es_fourcc(dec->fmt_in.i_codec);
    static bool is_kms = false;
    static bool kms_checked = false;

    if (!kms_checked) {
        // This is true for Pi3 and earlier
        // On Pi4 and later it is almost certainly better to be using V4L2
        is_kms = !rpi_use_qpu_deinterlace();
        kms_checked = true;
    }
    if (is_kms && !var_InheritBool(dec, MMAL_DECODE_ENABLE_NAME)) {
#if TRACE_ALL
        msg_Dbg(dec, "%s: <<< Disabled: Is KMS", __func__);
#endif
        return VLC_EGENERIC;
    }

#if TRACE_ALL || 1
    {
        char buf1[5], buf2[5], buf2a[5];
        char buf3[5], buf4[5];
        MMAL_RATIONAL_T r = rationalize_sar(dec->fmt_in.video.i_sar_num, dec->fmt_in.video.i_sar_den);

        msg_Dbg(dec, "%s: <<< (%s/%s)[%s] %dx%d %d/%d=%d/%d o:%#x -> (%s/%s) %dx%d %d/%d o:%#x", __func__,
                str_fourcc(buf1, dec->fmt_in.i_codec),
                str_fourcc(buf2, dec->fmt_in.video.i_chroma),
                str_fourcc(buf2a, in_fcc),
                dec->fmt_in.video.i_width, dec->fmt_in.video.i_height,
                dec->fmt_in.video.i_sar_num, dec->fmt_in.video.i_sar_den,
                r.num, r.den,
                (int)dec->fmt_in.video.orientation,
                str_fourcc(buf3, dec->fmt_out.i_codec),
                str_fourcc(buf4, dec->fmt_out.video.i_chroma),
                dec->fmt_out.video.i_width, dec->fmt_out.video.i_height,
                dec->fmt_out.video.i_sar_num, dec->fmt_out.video.i_sar_den,
                (int)dec->fmt_out.video.orientation);
    }
#endif

    if (!is_enc_supported(&supported_decode_in_enc, in_fcc))
        return VLC_EGENERIC;

    sys = calloc(1, sizeof(decoder_sys_t));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    dec->p_sys = sys;
    vlc_mutex_init(&sys->pic_lock);

    if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
        msg_Err(dec, "VCSM init failed");
        goto fail;
    }
    msg_Info(dec, "VCSM init succeeded: %s", cma_vcsm_init_str(sys->vcsm_init_type));

    sys->err_stream = MMAL_SUCCESS;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->input = sys->component->input[0];
    sys->output = sys->component->output[0];

    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)dec;
    sys->input->format->encoding = in_fcc;

    if (!set_and_test_enc_supported(&supported_decode_in_enc, sys->input, in_fcc)) {
#if TRACE_ALL
        char cbuf[5];
        msg_Dbg(dec, "Format not supported: %s", str_fourcc(cbuf, in_fcc));
#endif
        goto fail;
    }

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)dec;
    status = mmal_port_enable(sys->component->control, control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable control port %s (status=%"PRIx32" %s)",
                sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if ((status = set_extradata_and_commit(dec, sys)) != MMAL_SUCCESS)
        goto fail;

    sys->input->buffer_size = sys->input->buffer_size_recommended;
    sys->input->buffer_num = sys->input->buffer_num_recommended;

    status = mmal_port_enable(sys->input, input_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(dec, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }

    // Set vanishingly unlikely shape (or at least crop)
    // to ensure that we get a resolution changed event
    // Small wxh are rejected (128x128 is rejected) so pick a
    // plausible size.
    // Crop doesn't seem to be checked for being constrained by wxh
    // so we could place it outside the pic to be sure that it is
    // never matched but stick with something legal in case it is ever
    // actually checked
    sys->output->format->es->video.height = 256;
    sys->output->format->es->video.width = 256;
    sys->output->format->es->video.crop.height = 4;
    sys->output->format->es->video.crop.width = 2;
    sys->output->format->es->video.crop.x = 66;
    sys->output->format->es->video.crop.y = 88;

    if ((status = hw_mmal_opaque_output(VLC_OBJECT(dec), &sys->ppr,
                                        sys->output, NUM_EXTRA_BUFFERS, decoder_output_cb)) != MMAL_SUCCESS)
        goto fail;

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

    if ((status = decoder_send_extradata(dec, sys)) != MMAL_SUCCESS)
        goto fail;

    // Given no better ideas at this point copy input format to output
    // This also copies container stuff (such as orientation) that we do not
    // decode from the ES but may be important to display
    video_format_Copy(&dec->fmt_out.video, &dec->fmt_in.video);
    dec->fmt_out.i_codec = VLC_CODEC_MMAL_OPAQUE;
    dec->fmt_out.video.i_chroma = VLC_CODEC_MMAL_OPAQUE;


    dec->pf_decode = decode;
    dec->pf_flush  = flush_decoder;

#if TRACE_ALL
    msg_Dbg(dec, ">>> %s: ok", __func__);
#endif
    return 0;

fail:
    CloseDecoder(dec);
#if TRACE_ALL
msg_Dbg(dec, ">>> %s: FAIL: ret=%d", __func__, ret);
#endif
    return ret;
}

// ----------------------------

#define CONV_MAX_LATENCY 1  // In frames

typedef struct pic_fifo_s {
    picture_t * head;
    picture_t * tail;
} pic_fifo_t;

static inline picture_t * pic_fifo_get(pic_fifo_t * const pf)
{
    picture_t * const pic = pf->head;;
    if (pic != NULL) {
        pf->head = pic->p_next;
        pic->p_next = NULL;
    }
    return pic;
}

static inline picture_t * pic_fifo_get_all(pic_fifo_t * const pf)
{
    picture_t * const pic = pf->head;;
    pf->head = NULL;
    return pic;
}

static inline void pic_fifo_release_all(pic_fifo_t * const pf)
{
    picture_t * pic;
    while ((pic = pic_fifo_get(pf)) != NULL) {
        picture_Release(pic);
    }
}

static inline void pic_fifo_init(pic_fifo_t * const pf)
{
    pf->head = NULL;
    pf->tail = NULL;  // Not strictly needed
}

static inline void pic_fifo_put(pic_fifo_t * const pf, picture_t * pic)
{
    pic->p_next = NULL;
    if (pf->head == NULL)
        pf->head = pic;
    else
        pf->tail->p_next = pic;
    pf->tail = pic;
}

#define SUBS_MAX 3

typedef enum filter_resizer_e {
    FILTER_RESIZER_RESIZER,
    FILTER_RESIZER_ISP,
    FILTER_RESIZER_HVS
} filter_resizer_t;

typedef struct conv_frame_stash_s
{
    mtime_t pts;
    MMAL_BUFFER_HEADER_T * sub_bufs[SUBS_MAX];
} conv_frame_stash_t;

typedef struct filter_sys_t {
    filter_resizer_t resizer_type;
    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_PORT_T *output;
    MMAL_POOL_T *out_pool;  // Free output buffers
    MMAL_POOL_T *in_pool;   // Input pool to get BH for replication

    cma_buf_pool_t * cma_in_pool;
    cma_buf_pool_t * cma_out_pool;

    subpic_reg_stash_t subs[SUBS_MAX];

    pic_fifo_t ret_pics;

    unsigned int pic_n;
    vlc_sem_t sem;
    vlc_mutex_t lock;

    MMAL_STATUS_T err_stream;

    bool needs_copy_in;
    bool is_cma;
    bool is_sliced;
    bool out_fmt_set;
    const char * component_name;
    MMAL_PORT_BH_CB_T in_port_cb_fn;
    MMAL_PORT_BH_CB_T out_port_cb_fn;

    uint64_t frame_seq;
    conv_frame_stash_t stash[16];

    // Slice specific tracking stuff
    struct {
        pic_fifo_t pics;
        unsigned int line;  // Lines filled
    } slice;

    vcsm_init_type_t vcsm_init_type;
} filter_sys_t;


static MMAL_STATUS_T pic_to_format(MMAL_ES_FORMAT_T * const es_fmt, const picture_t * const pic)
{
    unsigned int bpp = (pic->format.i_bits_per_pixel + 7) >> 3;
    MMAL_VIDEO_FORMAT_T * const v_fmt = &es_fmt->es->video;

    es_fmt->type = MMAL_ES_TYPE_VIDEO;
    es_fmt->encoding = vlc_to_mmal_video_fourcc(&pic->format);
    es_fmt->encoding_variant = 0;

    // Fill in crop etc.
    hw_mmal_vlc_fmt_to_mmal_fmt(es_fmt, &pic->format);
    // Override width / height with strides if appropriate
    if (bpp != 0) {
        v_fmt->width = pic->p[0].i_pitch / bpp;
        v_fmt->height = pic->p[0].i_lines;
    }
    return MMAL_SUCCESS;
}


static MMAL_STATUS_T conv_enable_in(filter_t * const p_filter, filter_sys_t * const sys)
{
    MMAL_STATUS_T err = MMAL_SUCCESS;

    if (!sys->input->is_enabled &&
        (err = mmal_port_enable(sys->input, sys->in_port_cb_fn)) != MMAL_SUCCESS)
    {
        msg_Err(p_filter, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, err, mmal_status_to_string(err));
    }
    return err;
}

static MMAL_STATUS_T conv_enable_out(filter_t * const p_filter, filter_sys_t * const sys)
{
    MMAL_STATUS_T err = MMAL_SUCCESS;

    if (sys->is_cma)
    {
        if (sys->cma_out_pool == NULL &&
            (sys->cma_out_pool = cma_buf_pool_new(CONVERTER_BUFFERS, CONVERTER_BUFFERS, true, "mmal_resizer")) == NULL)
        {
            msg_Err(p_filter, "Failed to alloc cma buf pool");
            return MMAL_ENOMEM;
        }
    }
    else
    {
        cma_buf_pool_deletez(&sys->cma_out_pool);
    }

    if (!sys->output->is_enabled &&
        (err = mmal_port_enable(sys->output, sys->out_port_cb_fn)) != MMAL_SUCCESS)
    {
        msg_Err(p_filter, "Failed to enable output port %s (status=%"PRIx32" %s)",
                sys->output->name, err, mmal_status_to_string(err));
    }
    return err;
}

static void conv_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    filter_t * const p_filter = (filter_t *)port->userdata;

#if TRACE_ALL
    msg_Dbg(p_filter, "%s: <<< cmd=%d, data=%p, pic=%p", __func__, buffer->cmd, buffer->data, buffer->user_data);
#endif

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        MMAL_STATUS_T status = *(uint32_t *)buffer->data;

        p_filter->p_sys->err_stream = status;

        msg_Err(p_filter, "MMAL error %"PRIx32" \"%s\"", status,
                mmal_status_to_string(status));
    }

    mmal_buffer_header_release(buffer);
}

static void conv_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
#if TRACE_ALL
    picture_context_t * ctx = buf->user_data;
//    filter_sys_t *const sys = ((filter_t *)port->userdata)->p_sys;

    msg_Dbg((filter_t *)port->userdata, "<<< %s cmd=%d, ctx=%p, buf=%p, flags=%#x, len=%d/%d, pts=%lld",
            __func__, buf->cmd, ctx, buf, buf->flags, buf->length, buf->alloc_size, (long long)buf->pts);
#else
    VLC_UNUSED(port);
#endif

    mmal_buffer_header_release(buf);

#if TRACE_ALL
    msg_Dbg((filter_t *)port->userdata, ">>> %s", __func__);
#endif
}

static void conv_out_q_pic(filter_sys_t * const sys, picture_t * const pic)
{
    pic->p_next = NULL;

    vlc_mutex_lock(&sys->lock);
    pic_fifo_put(&sys->ret_pics, pic);
    vlc_mutex_unlock(&sys->lock);

    vlc_sem_post(&sys->sem);
}

static void conv_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    filter_t * const p_filter = (filter_t *)port->userdata;
    filter_sys_t * const sys = p_filter->p_sys;

#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s: cmd=%d, flags=%#x, pic=%p, data=%p, len=%d/%d, pts=%lld/%lld", __func__,
            buf->cmd, buf->flags, buf->user_data, buf->data, buf->length, buf->alloc_size,
            (long long)buf->pts, (long long)sys->stash[(unsigned int)(buf->pts & 0xf)].pts);
#endif
    if (buf->cmd == 0) {
        picture_t * const pic = (picture_t *)buf->user_data;

        if (pic == NULL) {
            msg_Err(p_filter, "%s: Buffer has no attached picture", __func__);
        }
        else if (buf->data == NULL || buf->length == 0)
        {
#if TRACE_ALL
            msg_Dbg(p_filter, "%s: Buffer has no data", __func__);
#endif
        }
        else
        {
            buf_to_pic_copy_props(pic, buf);

            // Set pic data pointers from buf aux info now it has it
            if (sys->is_cma) {
                if (cma_pic_set_data(pic, sys->output->format, buf) != VLC_SUCCESS)
                    msg_Err(p_filter, "Failed to set data");
            }

//            draw_corners(pic->p[0].p_pixels, pic->p[0].i_pitch / 4, 0, 0, pic->p[0].i_visible_pitch / 4, pic->p[0].i_visible_lines);
#if DEBUG_SQUARES
            draw_square(pic->p[0].p_pixels, pic->p[0].i_pitch / 4,  0, 0, 32, 32, 0xffff0000);
            draw_square(pic->p[0].p_pixels, pic->p[0].i_pitch / 4, 32, 0, 32, 32, 0xff00ff00);
            draw_square(pic->p[0].p_pixels, pic->p[0].i_pitch / 4, 64, 0, 32, 32, 0xff0000ff);
#endif

            buf->user_data = NULL;  // Responsability for this pic no longer with buffer
            conv_out_q_pic(sys, pic);
        }
    }

    mmal_buffer_header_release(buf);
}


static void slice_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    filter_t * const p_filter = (filter_t *)port->userdata;
    filter_sys_t * const sys = p_filter->p_sys;

#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s: cmd=%d, flags=%#x, pic=%p, data=%p, len=%d/%d, pts=%lld", __func__,
            buf->cmd, buf->flags, buf->user_data, buf->data, buf->length, buf->alloc_size, (long long)buf->pts);
#endif

    if (buf->cmd != 0)
    {
        mmal_buffer_header_release(buf);
        return;
    }

    if (buf->data == NULL || buf->length == 0)
    {
#if TRACE_ALL
        msg_Dbg(p_filter, "%s: Buffer has no data", __func__);
#endif
    }
    else
    {
        // Got slice
        picture_t *pic = sys->slice.pics.head;
        const unsigned int scale_lines = sys->output->format->es->video.height;  // Expected lines of callback

        if (pic == NULL) {
            msg_Err(p_filter, "No output picture");
            goto fail;
        }

        // Copy lines
        // * single plane only - fix for I420
        {
            const unsigned int scale_n = __MIN(scale_lines - sys->slice.line, MMAL_SLICE_HEIGHT);
            const unsigned int pic_lines = pic->p[0].i_lines;
            const unsigned int copy_n = sys->slice.line + scale_n <= pic_lines ? scale_n :
                sys->slice.line >= pic_lines ? 0 :
                    pic_lines - sys->slice.line;

            const unsigned int src_stride = buf->type->video.pitch[0];
            const unsigned int dst_stride = pic->p[0].i_pitch;
            uint8_t *dst = pic->p[0].p_pixels + sys->slice.line * dst_stride;
            const uint8_t *src = buf->data + buf->type->video.offset[0];

            if (src_stride == dst_stride) {
                if (copy_n != 0)
                    memcpy(dst, src, src_stride * copy_n);
            }
            else {
                unsigned int i;
                for (i = 0; i != copy_n; ++i) {
                    memcpy(dst, src, __MIN(dst_stride, src_stride));
                    dst += dst_stride;
                    src += src_stride;
                }
            }
            sys->slice.line += scale_n;
        }

        if ((buf->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) != 0 || sys->slice.line >= scale_lines) {

            if ((buf->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) == 0 || sys->slice.line != scale_lines) {
                // Stuff doesn't add up...
                msg_Err(p_filter, "Line count (%d/%d) & EOF disagree (flags=%#x)", sys->slice.line, scale_lines, buf->flags);
                goto fail;
            }
            else {
                sys->slice.line = 0;

                vlc_mutex_lock(&sys->lock);
                pic_fifo_get(&sys->slice.pics);  // Remove head from Q
                vlc_mutex_unlock(&sys->lock);

                buf_to_pic_copy_props(pic, buf);
                conv_out_q_pic(sys, pic);
            }
        }
    }

    // Put back
    buf->user_data = NULL; // Zap here to make sure we can't reuse later
    mmal_buffer_header_reset(buf);

    if (mmal_port_send_buffer(sys->output, buf) != MMAL_SUCCESS) {
        mmal_buffer_header_release(buf);
    }
    return;

fail:
    sys->err_stream = MMAL_EIO;
    vlc_sem_post(&sys->sem);  // If we were waiting then break us out - the flush should fix sem values
}


static void conv_flush(filter_t * p_filter)
{
    filter_sys_t * const sys = p_filter->p_sys;
    unsigned int i;

#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif

    if (sys->resizer_type == FILTER_RESIZER_HVS)
    {
        for (i = 0; i != SUBS_MAX; ++i) {
            hw_mmal_subpic_flush(VLC_OBJECT(p_filter), sys->subs + i);
        }
    }

    if (sys->input != NULL && sys->input->is_enabled)
        mmal_port_disable(sys->input);

    if (sys->output != NULL && sys->output->is_enabled)
        mmal_port_disable(sys->output);

//    cma_buf_pool_deletez(&sys->cma_out_pool);

    // Free up anything we may have already lying around
    // Don't need lock as the above disables should have prevented anything
    // happening in the background

    for (i = 0; i != 16; ++i) {
        conv_frame_stash_t *const stash = sys->stash + i;
        unsigned int sub_no;

        stash->pts = MMAL_TIME_UNKNOWN;
        for (sub_no = 0; sub_no != SUBS_MAX; ++sub_no) {
            if (stash->sub_bufs[sub_no] != NULL) {
                mmal_buffer_header_release(stash->sub_bufs[sub_no]);
                stash->sub_bufs[sub_no] = NULL;
            }
        }
    }

    pic_fifo_release_all(&sys->slice.pics);
    pic_fifo_release_all(&sys->ret_pics);

    // Reset sem values - easiest & most reliable way is to just kill & re-init
    vlc_sem_destroy(&sys->sem);
    vlc_sem_init(&sys->sem, 0);
    sys->pic_n = 0;

    // Reset error status
    sys->err_stream = MMAL_SUCCESS;

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s", __func__);
#endif
}

static void conv_stash_fixup(filter_t * const p_filter, filter_sys_t * const sys, picture_t * const p_pic)
{
    conv_frame_stash_t * const stash = sys->stash + (p_pic->date & 0xf);
    unsigned int sub_no;
    VLC_UNUSED(p_filter);

    p_pic->date = stash->pts;
    for (sub_no = 0; sub_no != SUBS_MAX; ++sub_no) {
        if (stash->sub_bufs[sub_no] != NULL) {
            // **** Do stashed blend
            // **** Aaargh, bother... need to rescale subs too

            mmal_buffer_header_release(stash->sub_bufs[sub_no]);
            stash->sub_bufs[sub_no] = NULL;
        }
    }
}

// Output buffers may contain a pic ref on error or flush
// Free it
static MMAL_BOOL_T out_buffer_pre_release_cb(MMAL_BUFFER_HEADER_T *header, void *userdata)
{
    VLC_UNUSED(userdata);

    picture_t * const pic = header->user_data;
    header->user_data = NULL;

    if (pic != NULL)
        picture_Release(pic);

    return MMAL_FALSE;
}

static MMAL_STATUS_T conv_set_output(filter_t * const p_filter, filter_sys_t * const sys, picture_t * const pic)
{
    MMAL_STATUS_T status;

    sys->output->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    sys->output->format->type = MMAL_ES_TYPE_VIDEO;
    sys->output->format->encoding = vlc_to_mmal_video_fourcc(&p_filter->fmt_out.video);
    sys->output->format->encoding_variant = 0;
    hw_mmal_vlc_fmt_to_mmal_fmt(sys->output->format, &p_filter->fmt_out.video);

    if (pic != NULL)
    {
        // Override default format width/height if we have a pic we need to match
        if ((status = pic_to_format(sys->output->format, pic)) != MMAL_SUCCESS)
        {
            char cbuf[5];
            msg_Err(p_filter, "Bad format desc: %s, pic=%p, bits=%d", str_fourcc(cbuf, pic->format.i_chroma), pic, pic->format.i_bits_per_pixel);
            return status;
        }

        MMAL_VIDEO_FORMAT_T *fmt = &sys->output->format->es->video;
        msg_Dbg(p_filter, "%s: %dx%d [(0,0) %dx%d]", __func__, fmt->width, fmt->height, fmt->crop.width, fmt->crop.height);
    }

    if (sys->is_sliced) {
        // Override height for slice
        sys->output->format->es->video.height = MMAL_SLICE_HEIGHT;
    }

    mmal_log_dump_format(sys->output->format);

    status = mmal_port_format_commit(sys->output);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to commit format for output port %s (status=%"PRIx32" %s)",
                sys->output->name, status, mmal_status_to_string(status));
        return status;
    }

    sys->output->buffer_num = __MAX(sys->is_sliced ? 16 : 2, sys->output->buffer_num_recommended);
    sys->output->buffer_size = sys->output->buffer_size_recommended;

    if ((status = conv_enable_out(p_filter, sys)) != MMAL_SUCCESS)
        return status;

    return MMAL_SUCCESS;
}


static picture_t *conv_get_out_pics(filter_sys_t * const sys)
{
    picture_t * ret_pics;

    vlc_sem_wait(&sys->sem);

    // Return a single pending buffer
    vlc_mutex_lock(&sys->lock);
    ret_pics = pic_fifo_get(&sys->ret_pics);
    vlc_mutex_unlock(&sys->lock);

    return ret_pics;
}

static picture_t *conv_filter(filter_t *p_filter, picture_t *p_pic)
{
    filter_sys_t * const sys = p_filter->p_sys;
    picture_t * ret_pics = NULL;
    MMAL_STATUS_T err;
    const uint64_t frame_seq = ++sys->frame_seq;
    conv_frame_stash_t * const stash = sys->stash + (frame_seq & 0xf);
    MMAL_BUFFER_HEADER_T * out_buf = NULL;

#if TRACE_ALL
    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(p_filter, "<<< %s: %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d->%s,%dx%d [(%d,%d) %dx%d] sar:%d/%d", __func__,
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma), p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                p_filter->fmt_in.video.i_sar_num, p_filter->fmt_in.video.i_sar_den,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma), p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height,
                p_filter->fmt_out.video.i_sar_num, p_filter->fmt_out.video.i_sar_den);
    }
#endif

    if (sys->err_stream != MMAL_SUCCESS) {
        goto stream_fail;
    }

    // Check pic fmt corresponds to what we have set up
    if (hw_mmal_vlc_pic_to_mmal_fmt_update(sys->input->format, p_pic))
    {
        msg_Dbg(p_filter, "Reset input port format");

        // HVS can take new formats without disable, others need it
        if (sys->resizer_type != FILTER_RESIZER_HVS) {
            // Extract any pending pic
            if (sys->pic_n >= 2) {
                ret_pics = conv_get_out_pics(sys);
                // If pic_n == 1 then we return without trying to get stuff
                sys->pic_n = 1;
            }
            if (sys->input->is_enabled) {
                if ((err = mmal_port_disable(sys->input)) != MMAL_SUCCESS)
                    msg_Warn(p_filter, "Format update disable failed: %s", mmal_status_to_string(err));
            }
        }

//        mmal_log_dump_port(sys->input);
        if ((err = mmal_port_format_commit(sys->input)) != MMAL_SUCCESS)
            msg_Warn(p_filter, "Format update commit failed: %s", mmal_status_to_string(err));

        // (Re)enable if required will be done later
    }

    if (p_pic->context == NULL) {
        // Can't have stashed subpics if not one of our pics
        if (!sys->needs_copy_in)
            msg_Dbg(p_filter, "%s: No context", __func__);
    }
    else if (sys->resizer_type == FILTER_RESIZER_HVS)
    {
        unsigned int sub_no = 0;

        for (sub_no = 0; sub_no != SUBS_MAX; ++sub_no) {
            int rv;
            if ((rv = hw_mmal_subpic_update(VLC_OBJECT(p_filter),
                                            hw_mmal_pic_sub_buf_get(p_pic, sub_no),
                                            sys->subs + sub_no,
                                            &p_pic->format,
                                            &sys->output->format->es->video.crop,
                                            MMAL_DISPLAY_ROT0,
                                            frame_seq)) == 0)
                break;
            else if (rv < 0)
                goto fail;
        }
    }
    else
    {
        unsigned int sub_no = 0;
        for (sub_no = 0; sub_no != SUBS_MAX; ++sub_no) {
            if ((stash->sub_bufs[sub_no] = hw_mmal_pic_sub_buf_get(p_pic, sub_no)) != NULL) {
                mmal_buffer_header_acquire(stash->sub_bufs[sub_no]);
            }
        }
    }

    if (!sys->out_fmt_set) {
        sys->out_fmt_set = true;

        if (sys->is_sliced) {
            // If zc then we will do stride conversion when we copy to arm side
            // so no need to worry about actual pic dimensions here
            if ((err = conv_set_output(p_filter, sys, NULL)) != MMAL_SUCCESS)
                goto fail;

            sys->out_pool = mmal_port_pool_create(sys->output, sys->output->buffer_num, sys->output->buffer_size);
        }
        else {
            picture_t *pic = filter_NewPicture(p_filter);
            err = conv_set_output(p_filter, sys, pic);
            picture_Release(pic);
            if (err != MMAL_SUCCESS)
                goto fail;

            sys->out_pool = mmal_pool_create(sys->output->buffer_num, 0);
        }

        if (sys->out_pool == NULL) {
            msg_Err(p_filter, "Failed to create output pool");
            goto fail;
        }
    }

    // Reenable stuff if the last thing we did was flush
    if ((err = conv_enable_out(p_filter, sys)) != MMAL_SUCCESS ||
        (err = conv_enable_in(p_filter, sys)) != MMAL_SUCCESS)
        goto fail;

    // We attach pic to buf before stuffing the output port
    // We could attach the pic on output for cma, but it is a lot easier to keep
    // the code common.
    {
        picture_t * const out_pic = filter_NewPicture(p_filter);

        if (out_pic == NULL)
        {
            msg_Err(p_filter, "Failed to alloc required filter output pic");
            goto fail;
        }

        if (p_filter->fmt_out.video.i_sar_den == 0 || p_filter->fmt_out.video.i_sar_num == 0) {
            out_pic->format.i_sar_den = 1;
            out_pic->format.i_sar_num = 1;
        }
        else {
            out_pic->format.i_sar_den = p_filter->fmt_out.video.i_sar_den;
            out_pic->format.i_sar_num = p_filter->fmt_out.video.i_sar_num;
        }

        if (sys->is_sliced) {
            vlc_mutex_lock(&sys->lock);
            pic_fifo_put(&sys->slice.pics, out_pic);
            vlc_mutex_unlock(&sys->lock);

            // Poke any returned pic buffers into output
            // In general this should only happen immediately after enable
            while ((out_buf = mmal_queue_get(sys->out_pool->queue)) != NULL)
                mmal_port_send_buffer(sys->output, out_buf);
        }
        else
        {
            // 1 in - 1 out
            if ((out_buf = mmal_queue_wait(sys->out_pool->queue)) == NULL)
            {
                msg_Err(p_filter, "Failed to get output buffer");
                picture_Release(out_pic);
                goto fail;
            }
            mmal_buffer_header_reset(out_buf);

            // Attach out_pic to the buffer & ensure it is freed when the buffer is released
            // On a good send callback the pic will be extracted to avoid this
            out_buf->user_data = out_pic;
            mmal_buffer_header_pre_release_cb_set(out_buf, out_buffer_pre_release_cb, NULL);

#if 0
            {
                char dbuf0[5];
                msg_Dbg(p_filter, "out_pic %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d",
                        str_fourcc(dbuf0, out_pic->format.i_chroma),
                        out_pic->format.i_width, out_pic->format.i_height,
                        out_pic->format.i_x_offset, out_pic->format.i_y_offset,
                        out_pic->format.i_visible_width, out_pic->format.i_visible_height,
                        out_pic->format.i_sar_num, out_pic->format.i_sar_den);
            }
#endif

            if (sys->is_cma) {
                int rv;

                cma_buf_t * const cb = cma_buf_pool_alloc_buf(sys->cma_out_pool, sys->output->buffer_size);
                if (cb == NULL) {
                    char dbuf0[5];
                    msg_Err(p_filter, "Failed to alloc CMA buf: fmt=%s, size=%d",
                            str_fourcc(dbuf0, out_pic->format.i_chroma),
                            sys->output->buffer_size);
                    goto fail;
                }
                const unsigned int vc_h = cma_buf_vc_handle(cb);  // Cannot coerce without going via variable
                out_buf->data = (uint8_t *)vc_h;
                out_buf->alloc_size = sys->output->buffer_size;

                if ((rv = cma_buf_pic_attach(cb, out_pic)) != VLC_SUCCESS)
                {
                    char dbuf0[5];
                    msg_Err(p_filter, "Failed to attach CMA to pic: fmt=%s err=%d",
                            str_fourcc(dbuf0, out_pic->format.i_chroma),
                            rv);
                    cma_buf_unref(cb);
                    goto fail;
                }
            }
            else {
                out_buf->data = out_pic->p[0].p_pixels;
                out_buf->alloc_size = out_pic->p[0].i_pitch * out_pic->p[0].i_lines;
                //**** stride ????
            }

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
    }


    // Stuff into input
    // We assume the BH is already set up with values reflecting pic date etc.
    stash->pts = p_pic->date;
    {
        MMAL_BUFFER_HEADER_T *const pic_buf = sys->needs_copy_in ?
            hw_mmal_pic_buf_copied(p_pic, sys->in_pool, sys->input, sys->cma_in_pool) :
            hw_mmal_pic_buf_replicated(p_pic, sys->in_pool);

        // Whether or not we extracted the pic_buf we are done with the picture
        picture_Release(p_pic);
        p_pic = NULL;

        if (pic_buf == NULL) {
            msg_Err(p_filter, "Pic has no attached buffer");
            goto fail;
        }

        pic_buf->pts = frame_seq;

#if TRACE_ALL
            msg_Dbg(p_filter, "In buf send: pic=%p, data=%p, user=%p, flags=%#x, len=%d/%d/%d, pts=%lld",
                    p_pic, pic_buf->data, pic_buf->user_data, pic_buf->flags,
                    pic_buf->length, pic_buf->alloc_size, sys->input->buffer_size, (long long)pic_buf->pts);
#endif

        if ((err = mmal_port_send_buffer(sys->input, pic_buf)) != MMAL_SUCCESS)
        {
            msg_Err(p_filter, "Send buffer to input failed");
            mmal_buffer_header_release(pic_buf);
            goto fail;
        }
    }

    // We have a 1 pic latency for everything except the 1st pic which we
    // wait for.
    // This means we get a single static pic out
    if (sys->pic_n++ == 1) {
#if TRACE_ALL
        msg_Dbg(p_filter, ">>> %s: Pic1=%p", __func__, ret_pics);
#endif
        return ret_pics;
    }

    ret_pics = conv_get_out_pics(sys);

    if (sys->err_stream != MMAL_SUCCESS)
        goto stream_fail;

    conv_stash_fixup(p_filter, sys, ret_pics);

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s: pic=%p", __func__, ret_pics);
#endif

    return ret_pics;

stream_fail:
    msg_Err(p_filter, "MMAL error reported by callback");
fail:
#if TRACE_ALL
    msg_Err(p_filter, ">>> %s: FAIL", __func__);
#endif
    if (ret_pics != NULL)
        picture_Release(ret_pics);
    if (out_buf != NULL)
        mmal_buffer_header_release(out_buf);
    if (p_pic != NULL)
        picture_Release(p_pic);
    conv_flush(p_filter);
    return NULL;
}

static void CloseConverter(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    filter_sys_t * const sys = p_filter->p_sys;
    unsigned int i;

#if TRACE_ALL
    msg_Dbg(obj, "<<< %s", __func__);
#endif

    if (sys == NULL)
        return;

    // Disables input & output ports
    conv_flush(p_filter);

    cma_buf_pool_deletez(&sys->cma_in_pool);
    cma_buf_pool_deletez(&sys->cma_out_pool);

    if (sys->component && sys->component->control->is_enabled)
        mmal_port_disable(sys->component->control);

    if (sys->component && sys->component->is_enabled)
        mmal_component_disable(sys->component);

    if (sys->resizer_type == FILTER_RESIZER_HVS)
    {
        for (i = 0; i != SUBS_MAX; ++i) {
            hw_mmal_subpic_close(VLC_OBJECT(p_filter), sys->subs + i);
        }
    }

    if (sys->out_pool)
    {
        if (sys->is_sliced)
            mmal_port_pool_destroy(sys->output, sys->out_pool);
        else
            mmal_pool_destroy(sys->out_pool);
    }

    if (sys->in_pool != NULL)
        mmal_pool_destroy(sys->in_pool);

    if (sys->component)
        mmal_component_release(sys->component);

    cma_vcsm_exit(sys->vcsm_init_type);

    vlc_sem_destroy(&sys->sem);
    vlc_mutex_destroy(&sys->lock);

    p_filter->p_sys = NULL;
    free(sys);
}


static inline MMAL_FOURCC_T filter_enc_in(const video_format_t * const fmt)
{
    if (hw_mmal_chroma_is_mmal(fmt->i_chroma))
        return vlc_to_mmal_video_fourcc(fmt);

    if (fmt->i_chroma == VLC_CODEC_I420 ||
        fmt->i_chroma == VLC_CODEC_I420_10L)
        return MMAL_ENCODING_I420;

    return 0;
}

static inline MMAL_FOURCC_T filter_enc_out(const video_format_t * const fmt)
{
    const MMAL_FOURCC_T mmes = vlc_to_mmal_video_fourcc(fmt);
    // Can only copy out single plane stuff currently - this could be fixed!
    return hw_mmal_chroma_is_mmal(fmt->i_chroma) || mmes != MMAL_ENCODING_I420 ? mmes : 0;
}


static int OpenConverter(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    int ret = VLC_EGENERIC;
    filter_sys_t *sys;
    MMAL_STATUS_T status = 0;
    MMAL_FOURCC_T enc_out = filter_enc_out(&p_filter->fmt_out.video);
    const MMAL_FOURCC_T enc_in = filter_enc_in(&p_filter->fmt_in.video);
    bool use_resizer;
    bool use_isp;
    int gpu_mem;

    // At least in principle we should deal with any mmal format as input
    if (enc_in == 0 || enc_out == 0)
        return VLC_EGENERIC;

    // Can't transform
    if (p_filter->fmt_in.video.orientation != p_filter->fmt_out.video.orientation)
        return VLC_EGENERIC;

    use_resizer = var_InheritBool(p_filter, MMAL_RESIZE_NAME);
    use_isp = var_InheritBool(p_filter, MMAL_ISP_NAME);

retry:
    // ** Make more generic by checking supported encs
    //
    // Must use ISP - HVS can't do this, nor can resizer
    if (enc_in == MMAL_ENCODING_YUVUV64_10) {
        // If resizer selected then just give up
        if (use_resizer)
            return VLC_EGENERIC;
        // otherwise downgrade HVS to ISP
        use_isp = true;
    }
    // HVS can't do I420
    if (enc_out == MMAL_ENCODING_I420) {
        use_isp = true;
    }
    // Only HVS can deal with SAND30
    if (enc_in == MMAL_ENCODING_YUV10_COL) {
        if (use_isp || use_resizer)
            return VLC_EGENERIC;
    }


    if (use_resizer) {
        // use resizer overrides use_isp
        use_isp = false;
    }

    // Check we have a sliced version of the fourcc if we want the resizer
    if (use_resizer &&
        (enc_out = pic_to_slice_mmal_fourcc(enc_out)) == 0) {
        return VLC_EGENERIC;
    }

    gpu_mem = hw_mmal_get_gpu_mem();

    {
        char dbuf0[5], dbuf1[5], dbuf2[5], dbuf3[5];
        msg_Dbg(p_filter, "%s: (%s) %s/%s,%dx%d [(%d,%d) %d/%d] sar:%d/%d->%s/%s,%dx%d [(%d,%d) %dx%d] rgb:%#x:%#x:%#x sar:%d/%d (gpu=%d)", __func__,
                use_resizer ? "resize" : use_isp ? "isp" : "hvs",
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma), str_fourcc(dbuf2, enc_in),
                p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                p_filter->fmt_in.video.i_sar_num, p_filter->fmt_in.video.i_sar_den,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma), str_fourcc(dbuf3, enc_out),
                p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height,
                p_filter->fmt_out.video.i_rmask, p_filter->fmt_out.video.i_gmask, p_filter->fmt_out.video.i_bmask,
                p_filter->fmt_out.video.i_sar_num, p_filter->fmt_out.video.i_sar_den,
                gpu_mem);
    }

    sys = calloc(1, sizeof(filter_sys_t));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    p_filter->p_sys = sys;

    // Init stuff the we destroy unconditionaly in Close first
    vlc_mutex_init(&sys->lock);
    vlc_sem_init(&sys->sem, 0);
    sys->err_stream = MMAL_SUCCESS;
    pic_fifo_init(&sys->ret_pics);
    pic_fifo_init(&sys->slice.pics);

    sys->needs_copy_in = !hw_mmal_chroma_is_mmal(p_filter->fmt_in.video.i_chroma);
    sys->in_port_cb_fn = conv_input_port_cb;

    if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
        msg_Err(p_filter, "VCSM init failed");
        goto fail;
    }

    if (use_resizer) {
        sys->resizer_type = FILTER_RESIZER_RESIZER;
        sys->is_sliced = true;
        sys->component_name = MMAL_COMPONENT_DEFAULT_RESIZER;
        sys->out_port_cb_fn = slice_output_port_cb;
    }
    else if (use_isp) {
        sys->resizer_type = FILTER_RESIZER_ISP;
        sys->is_sliced = false;  // Copy directly into filter picture
        sys->component_name = MMAL_COMPONENT_ISP_RESIZER;
        sys->out_port_cb_fn = conv_output_port_cb;
    } else {
        sys->resizer_type = FILTER_RESIZER_HVS;
        sys->is_sliced = false;  // Copy directly into filter picture
        sys->component_name = MMAL_COMPONENT_HVS;
        sys->out_port_cb_fn = conv_output_port_cb;
    }
    sys->is_cma = is_cma_buf_pic_chroma(p_filter->fmt_out.video.i_chroma);

    status = mmal_component_create(sys->component_name, &sys->component);
    if (status != MMAL_SUCCESS) {
        if (!use_isp && !use_resizer) {
            msg_Warn(p_filter, "Failed to rcreate HVS resizer - retrying with ISP");
            CloseConverter(obj);
            use_isp = true;
            goto retry;
        }
        msg_Err(p_filter, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
        goto fail;
    }
    sys->output = sys->component->output[0];
    sys->input  = sys->component->input[0];

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    status = mmal_port_enable(sys->component->control, conv_control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable control port %s (status=%"PRIx32" %s)",
                sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if (sys->needs_copy_in &&
        (sys->cma_in_pool = cma_buf_pool_new(2, 2, true, "conv-copy-in")) == NULL)
    {
        msg_Err(p_filter, "Failed to allocate input CMA pool");
        goto fail;
    }

    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)p_filter;
    sys->input->format->type = MMAL_ES_TYPE_VIDEO;
    sys->input->format->encoding = enc_in;
    sys->input->format->encoding_variant = MMAL_ENCODING_I420;
    hw_mmal_vlc_fmt_to_mmal_fmt(sys->input->format, &p_filter->fmt_in.video);
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

    if ((status = conv_enable_in(p_filter, sys)) != MMAL_SUCCESS)
        goto fail;

    port_parameter_set_bool(sys->output, MMAL_PARAMETER_ZERO_COPY, sys->is_sliced || sys->is_cma);

    status = mmal_component_enable(sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(p_filter, "Failed to enable component %s (status=%"PRIx32" %s)",
                sys->component->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if ((sys->in_pool = mmal_pool_create(sys->input->buffer_num, 0)) == NULL)
    {
        msg_Err(p_filter, "Failed to create input pool");
        goto fail;
    }

    if (sys->resizer_type == FILTER_RESIZER_HVS)
    {
        unsigned int i;
        for (i = 0; i != SUBS_MAX; ++i) {
            if (hw_mmal_subpic_open(VLC_OBJECT(p_filter), sys->subs + i, sys->component->input[i + 1], -1, i + 1) != MMAL_SUCCESS)
            {
                msg_Err(p_filter, "Failed to open subpic %d", i);
                goto fail;
            }
        }
    }

    p_filter->pf_video_filter = conv_filter;
    p_filter->pf_flush = conv_flush;
    // video_drain NIF in filter structure

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s: ok", __func__);
#endif

    return VLC_SUCCESS;

fail:
    CloseConverter(obj);

    if (!use_resizer && status == MMAL_ENOMEM) {
        use_resizer = true;
        msg_Warn(p_filter, "Lack of memory to use HVS/ISP: trying resizer");
        goto retry;
    }

#if TRACE_ALL
    msg_Dbg(p_filter, ">>> %s: FAIL: %d", __func__, ret);
#endif
    return ret;
}

#if OPT_TO_FROM_ZC
//----------------------------------------------------------------------------
//
// Simple copy in to ZC

typedef struct to_zc_sys_s {
    vcsm_init_type_t vcsm_init_type;
    cma_buf_pool_t * cma_out_pool;
} to_zc_sys_t;


static size_t buf_alloc_size(const vlc_fourcc_t i_chroma, const unsigned int width, const unsigned int height)
{
    const unsigned int pels = width * height;

    switch (i_chroma)
    {
        case VLC_CODEC_MMAL_ZC_RGB32:
            return pels * 4;
        case VLC_CODEC_MMAL_ZC_I420:
            return pels * 3 / 2;
        default:
            break;
    }
    return 0;
}


static picture_t *
to_zc_filter(filter_t *p_filter, picture_t *in_pic)
{
    to_zc_sys_t * const sys = (to_zc_sys_t *)p_filter->p_sys;
#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif

    assert(p_filter->fmt_out.video.i_chroma == VLC_CODEC_MMAL_ZC_I420);

    picture_t * const out_pic = filter_NewPicture(p_filter);
    if (out_pic == NULL)
        goto fail0;

    MMAL_ES_SPECIFIC_FORMAT_T mm_vfmt = {.video={0}};
    MMAL_ES_FORMAT_T mm_esfmt = {
        .encoding = vlc_to_mmal_video_fourcc(&p_filter->fmt_out.video),
        .es = &mm_vfmt};

    hw_mmal_vlc_fmt_to_mmal_fmt(&mm_esfmt, &p_filter->fmt_out.video);

    const size_t buf_alloc = buf_alloc_size(p_filter->fmt_out.video.i_chroma,
                                            mm_vfmt.video.width, mm_vfmt.video.height);
    if (buf_alloc == 0)
        goto fail1;
    cma_buf_t *const cb = cma_buf_pool_alloc_buf(sys->cma_out_pool, buf_alloc);
    if (cb == NULL)
        goto fail1;

    if (cma_buf_pic_attach(cb, out_pic) != VLC_SUCCESS)
        goto fail2;
    cma_pic_set_data(out_pic, &mm_esfmt, NULL);

    hw_mmal_copy_pic_to_buf(cma_buf_addr(cb), NULL, &mm_esfmt, in_pic);

    // Copy pic properties
    out_pic->date              = in_pic->date;
    out_pic->b_force           = in_pic->b_force;
    out_pic->b_progressive     = in_pic->b_progressive;
    out_pic->b_top_field_first = in_pic->b_top_field_first;
    out_pic->i_nb_fields       = in_pic->i_nb_fields;

    picture_Release(in_pic);

    return out_pic;

fail2:
    cma_buf_unref(cb);
fail1:
    picture_Release(out_pic);
fail0:
    picture_Release(in_pic);
    return NULL;
}

static void to_zc_flush(filter_t * p_filter)
{
    VLC_UNUSED(p_filter);
}

static void CloseConverterToZc(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    to_zc_sys_t * const sys = (to_zc_sys_t *)p_filter->p_sys;

    if (sys == NULL)
        return;

    p_filter->p_sys = NULL;

    cma_buf_pool_deletez(&sys->cma_out_pool);
    cma_vcsm_exit(sys->vcsm_init_type);

    free(sys);
}

static bool to_zc_validate_fmt(const video_format_t * const f_in, const video_format_t * const f_out)
{
    if (!((f_in->i_chroma == VLC_CODEC_I420 || f_in->i_chroma == VLC_CODEC_I420_10L) &&
          f_out->i_chroma == VLC_CODEC_MMAL_ZC_I420))
    {
        return false;
    }
    if (f_in->i_height != f_out->i_height ||
        f_in->i_width  != f_out->i_width)
    {
        return false;
    }

    return true;
}

static int OpenConverterToZc(vlc_object_t * obj)
{
    int ret = VLC_EGENERIC;
    filter_t * const p_filter = (filter_t *)obj;

    if (!to_zc_validate_fmt(&p_filter->fmt_in.video, &p_filter->fmt_out.video))
        goto fail;

    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(p_filter, "%s: %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d->%s,%dx%d [(%d,%d) %dx%d] rgb:%#x:%#x:%#x sar:%d/%d", __func__,
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma),
                p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                p_filter->fmt_in.video.i_sar_num, p_filter->fmt_in.video.i_sar_den,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma),
                p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height,
                p_filter->fmt_out.video.i_rmask, p_filter->fmt_out.video.i_gmask, p_filter->fmt_out.video.i_bmask,
                p_filter->fmt_out.video.i_sar_num, p_filter->fmt_out.video.i_sar_den);
    }

    to_zc_sys_t * const sys = calloc(1, sizeof(*sys));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    p_filter->p_sys = (filter_sys_t *)sys;

    if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
        msg_Err(p_filter, "VCSM init failed");
        goto fail;
    }

    if ((sys->cma_out_pool = cma_buf_pool_new(5, 5, true, "conv-to-zc")) == NULL)
    {
        msg_Err(p_filter, "Failed to allocate input CMA pool");
        goto fail;
    }

    p_filter->pf_video_filter = to_zc_filter;
    p_filter->pf_flush = to_zc_flush;
    return VLC_SUCCESS;

fail:
    CloseConverterToZc(obj);
    return ret;
}

//----------------------------------------------------------------------------
//
// Simple "copy" from ZC

static void CloseConverterFromZc(vlc_object_t * obj)
{
    VLC_UNUSED(obj);
}

static int OpenConverterFromZc(vlc_object_t * obj)
{
    return VLC_EGENERIC;
}
#endif
//----------------------------------------------------------------------------

typedef struct blend_sys_s {
    vzc_pool_ctl_t * vzc;
    const picture_t * last_dst;  // Not a ref, just a hint that we have a new pic
    vcsm_init_type_t vcsm_init_type;
} blend_sys_t;

static void FilterBlendMmal(filter_t *p_filter,
                  picture_t *dst, const picture_t * src,
                  int x_offset, int y_offset, int alpha)
{
    blend_sys_t * const sys = (blend_sys_t *)p_filter->p_sys;
#if TRACE_ALL
    msg_Dbg(p_filter, "%s (%d,%d:%d) pic=%p, pts=%lld, force=%d", __func__, x_offset, y_offset, alpha, src, src->date, src->b_force);
#endif
    // If nothing to do then do nothing
    if (alpha == 0 ||
        src->format.i_visible_height == 0 ||
        src->format.i_visible_width == 0)
    {
        return;
    }

    if (dst->context == NULL)
        msg_Err(p_filter, "MMAL pic missing context");
    else
    {
        // cast away src const so we can ref it
        MMAL_BUFFER_HEADER_T *buf = hw_mmal_vzc_buf_from_pic(sys->vzc, (picture_t *)src,
                                                             &p_filter->fmt_in.video,
                                                             vis_mmal_rect(&p_filter->fmt_out.video),
                                                             x_offset, y_offset,
                                                             alpha,
                                                             dst != sys->last_dst || !hw_mmal_pic_has_sub_bufs(dst));
        if (buf == NULL) {
            msg_Err(p_filter, "Failed to allocate vzc buffer for subpic");
            return;
        }

        hw_mmal_pic_sub_buf_add(dst, buf);

        sys->last_dst = dst;
    }
}

static void FlushBlendMmal(filter_t * p_filter)
{
    blend_sys_t * const sys = (blend_sys_t *)p_filter->p_sys;
    sys->last_dst = NULL;
    hw_mmal_vzc_pool_flush(sys->vzc);
}

static void CloseBlendMmal(vlc_object_t *object)
{
    filter_t * const p_filter = (filter_t *)object;
    blend_sys_t * const sys = (blend_sys_t *)p_filter->p_sys;

    if (sys != NULL) {
        p_filter->p_sys = NULL;

        hw_mmal_vzc_pool_release(sys->vzc);
        cma_vcsm_exit(sys->vcsm_init_type);
        free(sys);
    }
}

static int OpenBlendMmal(vlc_object_t *object)
{
    filter_t * const p_filter = (filter_t *)object;
    const vlc_fourcc_t vfcc_dst = p_filter->fmt_out.video.i_chroma;

    if (!hw_mmal_chroma_is_mmal(vfcc_dst) ||
        !hw_mmal_vzc_subpic_fmt_valid(&p_filter->fmt_in.video))
    {
        return VLC_EGENERIC;
    }

    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(p_filter, "%s: (%s) %s,%dx%d [(%d,%d) %dx%d]->%s,%dx%d [(%d,%d) %dx%d]", __func__,
                "blend",
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma), p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma), p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height);
    }

    {
        blend_sys_t * const sys = calloc(1, sizeof (*sys));
        if (sys == NULL)
            return VLC_ENOMEM;

        p_filter->p_sys = (filter_sys_t *)sys;

        if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
            msg_Err(p_filter, "VCSM init failed");
            goto fail;
        }

        if ((sys->vzc = hw_mmal_vzc_pool_new()) == NULL)
            goto fail;
    }

    p_filter->pf_video_blend = FilterBlendMmal;
    p_filter->pf_flush = FlushBlendMmal;

    return VLC_SUCCESS;

fail:
    CloseBlendMmal(VLC_OBJECT(p_filter));
    return VLC_ENOMEM;
}

// ---------------------------------------------------------------------------

static void FilterBlendNeon(filter_t *p_filter,
                  picture_t *dst_pic, const picture_t * src_pic,
                  int x_offset, int y_offset, int alpha)
{
    const uint8_t * s_data;
    uint8_t * d_data;
    const video_format_t *const src_fmt = &p_filter->fmt_in.video;  // Format here has different visible params to pic
    const video_format_t *const dst_fmt = &p_filter->fmt_out.video;
    int width = src_fmt->i_visible_width;
    int height = src_fmt->i_visible_height;
    blend_neon_fn *const blend_fn = (blend_neon_fn * )p_filter->p_sys;

#if TRACE_ALL
    msg_Dbg(p_filter, "%s (%d,%d:%d) pic=%p (%dx%d/%dx%d), pts=%lld, force=%d, filter in %dx%d/%dx%d", __func__, x_offset, y_offset, alpha,
            src_pic, src_pic->format.i_width, src_pic->format.i_height, src_pic->format.i_visible_width, src_pic->format.i_visible_height,
            src_pic->date, src_pic->b_force,
            src_fmt->i_width, src_fmt->i_height, src_fmt->i_visible_width, src_fmt->i_visible_height);
#endif

    if (alpha == 0)
        return;

    x_offset += dst_fmt->i_x_offset;
    y_offset += dst_fmt->i_y_offset;

    // Deal with R/B overrun
    if (x_offset + width >= (int)(dst_fmt->i_x_offset + dst_fmt->i_visible_width))
        width = dst_fmt->i_x_offset + dst_fmt->i_visible_width - x_offset;
    if (y_offset + height >= (int)(dst_fmt->i_y_offset + dst_fmt->i_visible_height))
        height = dst_fmt->i_y_offset + dst_fmt->i_visible_height - y_offset;

    if (width <= 0 || height <= 0)
        return;

    // *** L/U overrun

    s_data = src_pic->p[0].p_pixels +
        src_pic->p[0].i_pixel_pitch * src_fmt->i_x_offset +
        src_pic->p[0].i_pitch * src_fmt->i_y_offset;
    d_data = dst_pic->p[0].p_pixels +
        dst_pic->p[0].i_pixel_pitch * x_offset +
        dst_pic->p[0].i_pitch * y_offset;


    do {
        blend_fn(d_data, s_data, alpha, width);
        s_data += src_pic->p[0].i_pitch;
        d_data += dst_pic->p[0].i_pitch;
    } while (--height > 0);
}

static void CloseBlendNeon(vlc_object_t *object)
{
    VLC_UNUSED(object);
}

static int OpenBlendNeon(vlc_object_t *object)
{
    filter_t * const p_filter = (filter_t *)object;
    const vlc_fourcc_t vfcc_dst = p_filter->fmt_out.video.i_chroma;
    MMAL_FOURCC_T mfcc_src = vlc_to_mmal_video_fourcc(&p_filter->fmt_in.video);
    MMAL_FOURCC_T mfcc_dst = vlc_to_mmal_video_fourcc(&p_filter->fmt_out.video);
    blend_neon_fn * blend_fn = (blend_neon_fn *)0;

    // Obviously can't use this if we have no neon
    if (!vlc_CPU_ARM_NEON())
        return VLC_EGENERIC;

    // Non-alpha RGB only for dest
    if (vfcc_dst != VLC_CODEC_RGB32)
        return VLC_EGENERIC;

    // Check we have appropriate blend fn (mmal doesn't have a non-alpha RGB32)
    switch (mfcc_src) {
    case MMAL_ENCODING_RGBA:
        if (mfcc_dst == MMAL_ENCODING_RGBA)
            blend_fn = blend_rgbx_rgba_neon;
        else if (mfcc_dst == MMAL_ENCODING_BGRA)
            blend_fn = blend_bgrx_rgba_neon;
        break;

    case MMAL_ENCODING_BGRA:
        if (mfcc_dst == MMAL_ENCODING_BGRA)
            blend_fn = blend_rgbx_rgba_neon;
        else if (mfcc_dst == MMAL_ENCODING_RGBA)
            blend_fn = blend_bgrx_rgba_neon;
        break;

    default:
        break;
    }

    if (blend_fn == (blend_neon_fn *)0)
    {
        return VLC_EGENERIC;
    }

    p_filter->p_sys = (void *)blend_fn;
    p_filter->pf_video_blend = FilterBlendNeon;

    {
        char dbuf0[5], dbuf1[5];
        char dbuf0a[5], dbuf1a[5];
        msg_Dbg(p_filter, "%s: (%s) %s/%s,%dx%d [(%d,%d) %dx%d]->%s/%s,%dx%d [(%d,%d) %dx%d]", __func__,
                "blend",
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma),
                str_fourcc(dbuf0a, mfcc_src),
                p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma),
                str_fourcc(dbuf1a, mfcc_dst),
                p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height);
    }

    return VLC_SUCCESS;
}

vlc_module_begin()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_shortname(N_("MMAL decoder"))
    set_description(N_("MMAL-based decoder plugin for Raspberry Pi"))
    set_capability("video decoder", 90)
    add_shortcut("mmal_decoder")
    add_bool(MMAL_OPAQUE_NAME, true, MMAL_OPAQUE_TEXT, MMAL_OPAQUE_LONGTEXT, false)
    add_bool(MMAL_DECODE_ENABLE_NAME, false, MMAL_DECODE_ENABLE_TEXT, MMAL_DECODE_ENABLE_LONGTEXT, true)
    set_callbacks(OpenDecoder, CloseDecoder)

    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("MMAL resizer"))
    set_description(N_("MMAL resizing conversion filter"))
    add_shortcut("mmal_converter")
    set_capability( "video converter", 900 )
    add_bool(MMAL_RESIZE_NAME, /* default */ false, MMAL_RESIZE_TEXT, MMAL_RESIZE_LONGTEXT, /* advanced option */ false)
    add_bool(MMAL_ISP_NAME, /* default */ false, MMAL_ISP_TEXT, MMAL_ISP_LONGTEXT, /* advanced option */ false)
    set_callbacks(OpenConverter, CloseConverter)

#if OPT_TO_FROM_ZC
    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("MMAL to ZC"))
    set_description(N_("MMAL conversion to ZC filter"))
    add_shortcut("mmal_to_zc")
    set_capability( "video converter", 901 )
    set_callbacks(OpenConverterToZc, CloseConverterToZc)

    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("MMAL from ZC"))
    set_description(N_("MMAL conversion from ZC filter"))
    add_shortcut("mmal_from_zc")
    set_capability( "video converter", 902 )
    set_callbacks(OpenConverterFromZc, CloseConverterFromZc)
#endif

    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_description(N_("Video pictures blending for MMAL"))
    add_shortcut("mmal_blend")
    set_capability("video blending", 120)
    set_callbacks(OpenBlendMmal, CloseBlendMmal)

    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_description(N_("Video pictures blending for neon"))
    add_shortcut("neon_blend")
    set_capability("video blending", 110)
    set_callbacks(OpenBlendNeon, CloseBlendNeon)

vlc_module_end()


