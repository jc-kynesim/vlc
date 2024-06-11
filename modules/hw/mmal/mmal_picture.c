/*****************************************************************************
 * mmal_picture.c: MMAL picture related shared functionality
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

// We would really like to use vlc_thread.h but the detach thread stuff can't be
// used here :-(
#include <pthread.h>

#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>

#include <vlc_common.h>
#include <vlc_cpu.h>
#include <vlc_picture.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
#include <bcm_host.h>
#pragma GCC diagnostic pop
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/vmcs_host/vcgencmd.h>
#include <interface/vcsm/user-vcsm.h>

#include "mmal_cma.h"
#include "mmal_picture.h"
#include "transform_ops.h"

#define TRACE_TRANSFORMS 0

#define UINT64_SIZE(s) (((s) + sizeof(uint64_t) - 1)/sizeof(uint64_t))

static inline char safe_char(const unsigned int c0)
{
    const unsigned int c = c0 & 0xff;
    return c > ' ' && c < 0x7f ? c : '.';
}

const char * str_fourcc(char * const buf, const unsigned int fcc)
{
    if (fcc == 0)
        return "----";
    buf[0] = safe_char(fcc >> 0);
    buf[1] = safe_char(fcc >> 8);
    buf[2] = safe_char(fcc >> 16);
    buf[3] = safe_char(fcc >> 24);
    buf[4] = 0;
    return buf;
}

// WB + Inv
static inline void flush_range(void * const start, const size_t len)
{
    uint64_t buf[UINT64_SIZE(sizeof(struct vcsm_user_clean_invalid2_s) + sizeof(struct vcsm_user_clean_invalid2_block_s))];
    struct vcsm_user_clean_invalid2_s * const b = (struct vcsm_user_clean_invalid2_s *)buf;

    *b = (struct vcsm_user_clean_invalid2_s){
        .op_count = 1
    };

    b->s[0] = (struct vcsm_user_clean_invalid2_block_s){
        .invalidate_mode = 3,   // wb + invalidate
        .block_count = 1,
        .start_address = start, // Rely on clean inv to fix up align & size boundries
        .block_size = len,
        .inter_block_stride = 0
    };

    vcsm_clean_invalid2(b);
}

MMAL_FOURCC_T vlc_to_mmal_color_space(const video_color_space_t vlc_cs)
{
    switch (vlc_cs)
    {
        case COLOR_SPACE_BT601:
            return MMAL_COLOR_SPACE_ITUR_BT601;
        case COLOR_SPACE_BT709:
            return MMAL_COLOR_SPACE_ITUR_BT709;
        default:
            break;
    }
    return MMAL_COLOR_SPACE_UNKNOWN;
}

MMAL_FOURCC_T vlc_to_mmal_video_fourcc(const video_frame_format_t * const vf_vlc)
{
    switch (vf_vlc->i_chroma) {
        case VLC_CODEC_MMAL_ZC_RGB32:
        case VLC_CODEC_RGB32:
        {
            // VLC RGB32 aka RV32 means we have to look at the mask values
            const uint32_t r = vf_vlc->i_rmask;
            const uint32_t g = vf_vlc->i_gmask;
            const uint32_t b = vf_vlc->i_bmask;
            if (r == 0xff0000 && g == 0xff00 && b == 0xff)
                return MMAL_ENCODING_BGRA;
            if (r == 0xff && g == 0xff00 && b == 0xff0000)
                return MMAL_ENCODING_RGBA;
            if (r == 0xff000000 && g == 0xff0000 && b == 0xff00)
                return MMAL_ENCODING_ABGR;
            if (r == 0xff00 && g == 0xff0000 && b == 0xff000000)
                return MMAL_ENCODING_ARGB;
            break;
        }
        case VLC_CODEC_RGB16:
        {
            // VLC RGB16 aka RV16 means we have to look at the mask values
            const uint32_t r = vf_vlc->i_rmask;
            const uint32_t g = vf_vlc->i_gmask;
            const uint32_t b = vf_vlc->i_bmask;
            if (r == 0xf800 && g == 0x7e0 && b == 0x1f)
                return MMAL_ENCODING_RGB16;
            break;
        }
        case VLC_CODEC_I420:
        case VLC_CODEC_MMAL_ZC_I420:
            return MMAL_ENCODING_I420;
        case VLC_CODEC_RGBA:
            return MMAL_ENCODING_RGBA;
        case VLC_CODEC_BGRA:
            return MMAL_ENCODING_BGRA;
        case VLC_CODEC_ARGB:
            return MMAL_ENCODING_ARGB;
        // VLC_CODEC_ABGR does not exist in VLC
        case VLC_CODEC_MMAL_OPAQUE:
            return MMAL_ENCODING_OPAQUE;
        case VLC_CODEC_MMAL_ZC_SAND8:
            return MMAL_ENCODING_YUVUV128;
        case VLC_CODEC_MMAL_ZC_SAND10:
            return MMAL_ENCODING_YUVUV64_10;
        case VLC_CODEC_MMAL_ZC_SAND30:
            return MMAL_ENCODING_YUV10_COL;
        default:
            break;
    }
    return 0;
}

static void vlc_fmt_to_video_format(MMAL_VIDEO_FORMAT_T *const vf_mmal, const video_frame_format_t * const vf_vlc)
{
    const unsigned int wmask = (vf_vlc->i_chroma == VLC_CODEC_MMAL_ZC_I420 ||
                                vf_vlc->i_chroma == VLC_CODEC_I420) ? 31 : 15;

    vf_mmal->width          = (vf_vlc->i_width + wmask) & ~wmask;
    vf_mmal->height         = (vf_vlc->i_height + 15) & ~15;
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
    vf_mmal->color_space    = vlc_to_mmal_color_space(vf_vlc->space);
}


void hw_mmal_vlc_fmt_to_mmal_fmt(MMAL_ES_FORMAT_T *const es_fmt, const video_frame_format_t * const vf_vlc)
{
    vlc_fmt_to_video_format(&es_fmt->es->video, vf_vlc);
}

bool hw_mmal_vlc_pic_to_mmal_fmt_update(MMAL_ES_FORMAT_T *const es_fmt, const picture_t * const pic)
{
    MMAL_VIDEO_FORMAT_T vf_new_ss;
    MMAL_VIDEO_FORMAT_T *const vf_old = &es_fmt->es->video;
    MMAL_VIDEO_FORMAT_T *const vf_new = &vf_new_ss;

    vlc_fmt_to_video_format(vf_new, &pic->format);

    // If we have a format that might have come from ffmpeg then rework for
    // a better guess as to layout. All sand stuff is "special" with regards to
    // width/height vs real layout so leave as is if that
    if ((pic->format.i_chroma == VLC_CODEC_MMAL_ZC_I420 ||
         pic->format.i_chroma == VLC_CODEC_MMAL_ZC_RGB32) &&
        pic->p[0].i_pixel_pitch != 0)
    {
        // Now overwrite width/height with a better guess as to actual layout info
        vf_new->height = pic->p[0].i_lines;
        vf_new->width = pic->p[0].i_pitch / pic->p[0].i_pixel_pitch;
    }

    if (
        vf_new->width          != vf_old->width          ||
        vf_new->height         != vf_old->height         ||
        vf_new->crop.x         != vf_old->crop.x         ||
        vf_new->crop.y         != vf_old->crop.y         ||
        vf_new->crop.width     != vf_old->crop.width     ||
        vf_new->crop.height    != vf_old->crop.height    ||
        vf_new->par.num        != vf_old->par.num        ||
        vf_new->par.den        != vf_old->par.den        ||
        // Frame rate ignored
        vf_new->color_space    != vf_old->color_space)
    {
#if 0
        char dbuf0[5], dbuf1[5];
        printf("%dx%d (%d,%d %dx%d) par:%d/%d %s -> %dx%d (%d,%d %dx%d) par:%d/%d %s\n",
               vf_old->width          ,
               vf_old->height         ,
               vf_old->crop.x         ,
               vf_old->crop.y         ,
               vf_old->crop.width     ,
               vf_old->crop.height    ,
               vf_old->par.num        ,
               vf_old->par.den        ,
               str_fourcc(dbuf0, vf_old->color_space)    ,
               vf_new->width          ,
               vf_new->height         ,
               vf_new->crop.x         ,
               vf_new->crop.y         ,
               vf_new->crop.width     ,
               vf_new->crop.height    ,
               vf_new->par.num        ,
               vf_new->par.den        ,
               str_fourcc(dbuf1, vf_new->color_space)    );
#endif
        *vf_old = *vf_new;
        return true;
    }
    return false;
}


hw_mmal_port_pool_ref_t * hw_mmal_port_pool_ref_create(MMAL_PORT_T * const port,
   const unsigned int headers, const uint32_t payload_size)
{
    hw_mmal_port_pool_ref_t * ppr = calloc(1, sizeof(hw_mmal_port_pool_ref_t));
    if (ppr == NULL)
        return NULL;

    if ((ppr->pool = mmal_port_pool_create(port, headers, payload_size)) == NULL)
        goto fail;

    ppr->port = port;
    atomic_store(&ppr->refs, 1);
    return ppr;

fail:
    free(ppr);
    return NULL;
}

static void do_detached(void *(*fn)(void *), void * v)
{
    pthread_t dothread;
    pthread_create(&dothread, NULL, fn, v);
    pthread_detach(dothread);
}

// Destroy a ppr - aranged s.t. it has the correct prototype for a pthread
static void * kill_ppr(void * v)
{
    hw_mmal_port_pool_ref_t * const ppr = v;
    if (ppr->port->is_enabled)
        mmal_port_disable(ppr->port);  // Avoid annoyed messages from MMAL when we kill the pool
    mmal_port_pool_destroy(ppr->port, ppr->pool);
    free(ppr);
    return NULL;
}

void hw_mmal_port_pool_ref_release(hw_mmal_port_pool_ref_t * const ppr, const bool in_cb)
{
    if (ppr == NULL)
        return;
    if (atomic_fetch_sub(&ppr->refs, 1) != 1)
        return;
    if (in_cb)
        do_detached(kill_ppr, ppr);
    else
        kill_ppr(ppr);
}

// Put buffer in port if possible - if not then release to pool
// Returns true if sent, false if recycled
bool hw_mmal_port_pool_ref_recycle(hw_mmal_port_pool_ref_t * const ppr, MMAL_BUFFER_HEADER_T * const buf)
{
    mmal_buffer_header_reset(buf);
    buf->user_data = NULL;

    if (mmal_port_send_buffer(ppr->port, buf) == MMAL_SUCCESS)
        return true;
    mmal_buffer_header_release(buf);
    return false;
}

MMAL_STATUS_T hw_mmal_port_pool_ref_fill(hw_mmal_port_pool_ref_t * const ppr)
{
    MMAL_BUFFER_HEADER_T * buf;
    MMAL_STATUS_T err = MMAL_SUCCESS;

    while ((buf = mmal_queue_get(ppr->pool->queue)) != NULL) {
        if ((err = mmal_port_send_buffer(ppr->port, buf)) != MMAL_SUCCESS)
        {
            mmal_queue_put_back(ppr->pool->queue, buf);
            break;
        }
    }
    return err;
}


MMAL_STATUS_T hw_mmal_opaque_output(vlc_object_t * const obj,
                                    hw_mmal_port_pool_ref_t ** pppr,
                                    MMAL_PORT_T * const port,
                                    const unsigned int extra_buffers, MMAL_PORT_BH_CB_T callback)
{
    MMAL_STATUS_T status;

    port->userdata = (struct MMAL_PORT_USERDATA_T *)obj;

    status = port_parameter_set_uint32(port, MMAL_PARAMETER_EXTRA_BUFFERS, extra_buffers);
    if (status != MMAL_SUCCESS) {
        msg_Err(obj, "Failed to set MMAL_PARAMETER_EXTRA_BUFFERS on output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        return status;
    }

    status = port_parameter_set_bool(port, MMAL_PARAMETER_ZERO_COPY, 1);
    if (status != MMAL_SUCCESS) {
       msg_Err(obj, "Failed to set zero copy on port %s (status=%"PRIx32" %s)",
                port->name, status, mmal_status_to_string(status));
       return status;
    }

    port->format->encoding = MMAL_ENCODING_OPAQUE;
    port->format->encoding_variant = 0;
    if ((status = mmal_port_format_commit(port)) != MMAL_SUCCESS)
    {
        msg_Err(obj, "Failed to commit format on port %s (status=%"PRIx32" %s)",
                 port->name, status, mmal_status_to_string(status));
        return status;
    }

    port->buffer_num = 30;
    port->buffer_size = port->buffer_size_recommended;

    if ((*pppr = hw_mmal_port_pool_ref_create(port, port->buffer_num, port->buffer_size)) == NULL) {
        msg_Err(obj, "Failed to create output pool");
        return status;
    }

    status = mmal_port_enable(port, callback);
    if (status != MMAL_SUCCESS) {
        hw_mmal_port_pool_ref_release(*pppr, false);
        *pppr = NULL;
        msg_Err(obj, "Failed to enable output port %s (status=%"PRIx32" %s)",
                port->name, status, mmal_status_to_string(status));
        return status;
    }

    return MMAL_SUCCESS;
}


void hw_mmal_pic_ctx_destroy(picture_context_t * pic_ctx_cmn)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    unsigned int i;

    for (i = 0; i != ctx->buf_count; ++i) {
        if (ctx->bufs[i] != NULL)
            mmal_buffer_header_release(ctx->bufs[i]);
    }

    cma_buf_end_flight(ctx->cb);
    cma_buf_unref(ctx->cb);

    free(ctx);
}

picture_context_t * hw_mmal_pic_ctx_copy(picture_context_t * pic_ctx_cmn)
{
    const pic_ctx_mmal_t * const src_ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    pic_ctx_mmal_t * const dst_ctx = calloc(1, sizeof(*dst_ctx));
    unsigned int i;

    if (dst_ctx == NULL)
        return NULL;

    // Copy
    dst_ctx->cmn = src_ctx->cmn;

    dst_ctx->cb = cma_buf_ref(src_ctx->cb);

    dst_ctx->buf_count = src_ctx->buf_count;
    for (i = 0; i != src_ctx->buf_count; ++i) {
        dst_ctx->bufs[i] = src_ctx->bufs[i];
        if (dst_ctx->bufs[i] != NULL)
            mmal_buffer_header_acquire(dst_ctx->bufs[i]);
    }

    return &dst_ctx->cmn;
}

static MMAL_BOOL_T
buf_pre_release_cb(MMAL_BUFFER_HEADER_T * buf, void *userdata)
{
    hw_mmal_port_pool_ref_t * const ppr = userdata;

    // Kill the callback - otherwise we will go in circles!
    mmal_buffer_header_pre_release_cb_set(buf, (MMAL_BH_PRE_RELEASE_CB_T)0, NULL);
    mmal_buffer_header_acquire(buf);  // Ref it again

    // As we have re-acquired the buffer we need a full release
    // (not continue) to zap the ref count back to zero
    // This is "safe" 'cos we have already reset the cb
    hw_mmal_port_pool_ref_recycle(ppr, buf);
    hw_mmal_port_pool_ref_release(ppr, true); // Assume in callback

    return MMAL_TRUE;
}

// Buffer belongs to context on successful return from this fn
// is still valid on failure
picture_context_t *
hw_mmal_gen_context(MMAL_BUFFER_HEADER_T * buf, hw_mmal_port_pool_ref_t * const ppr)
{
    pic_ctx_mmal_t * const ctx = calloc(1, sizeof(pic_ctx_mmal_t));

    if (ctx == NULL)
        return NULL;

    // If we have an associated ppr then ref & set appropriate callbacks
    if (ppr != NULL) {
        hw_mmal_port_pool_ref_acquire(ppr);
        mmal_buffer_header_pre_release_cb_set(buf, buf_pre_release_cb, ppr);
        buf->user_data = NULL;
    }

    ctx->cmn.copy = hw_mmal_pic_ctx_copy;
    ctx->cmn.destroy = hw_mmal_pic_ctx_destroy;

    ctx->buf_count = 1;
    ctx->bufs[0] = buf;

    return &ctx->cmn;
}

// n is els
// * Make NEON!
typedef void piccpy_fn(void * dest, const void * src, size_t n);

extern piccpy_fn mmal_piccpy_10_to_8_neon;

static void piccpy_10_to_8_c(void * dest, const void * src, size_t n)
{
    uint8_t * d = dest;
    const uint16_t * s = src;
    while (n-- != 0)
        *d++ = *s++ >> 2;
}

// Do a stride converting copy - if the strides are the same and line_len is
// close then do a single block copy - we don't expect to have to preserve
// pixels in the output frame
static void mem_copy_2d(uint8_t * d_ptr, const size_t d_stride,
                        const uint8_t * s_ptr, const size_t s_stride,
                        size_t lines, const size_t line_len)
{
    if (s_stride == d_stride && d_stride < line_len + 32)
    {
        memcpy(d_ptr, s_ptr, d_stride * lines);
    }
    else
    {
        while (lines-- != 0) {
            memcpy(d_ptr, s_ptr, line_len);
            d_ptr += d_stride;
            s_ptr += s_stride;
        }
    }
}

// line_len in D units
static void mem_copy_2d_10_to_8(uint8_t * d_ptr, const size_t d_stride,
                        const uint8_t * s_ptr, const size_t s_stride,
                        size_t lines, const size_t line_len)
{
    piccpy_fn * const docpy = vlc_CPU_ARM_NEON() ? mmal_piccpy_10_to_8_neon : piccpy_10_to_8_c;
    if (s_stride == d_stride * 2 && d_stride < line_len + 32)
    {
        docpy(d_ptr, s_ptr, d_stride * lines);
    }
    else
    {
        while (lines-- != 0) {
            docpy(d_ptr, s_ptr, line_len);
            d_ptr += d_stride;
            s_ptr += s_stride;
        }
    }
}


int hw_mmal_copy_pic_to_buf(void * const buf_data,
                            uint32_t * const pLength,
                            const MMAL_ES_FORMAT_T * const fmt,
                            const picture_t * const pic)
{
    const MMAL_VIDEO_FORMAT_T *const video = &fmt->es->video;
    uint8_t * const dest = buf_data;
    size_t length = 0;

    //**** Worry about x/y_offsets

    assert(fmt->encoding == MMAL_ENCODING_I420);

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420:
        {
            const size_t y_size = video->width * video->height;
            mem_copy_2d(dest, video->width,
                 pic->p[0].p_pixels, pic->p[0].i_pitch,
                 video->crop.height,
                 video->crop.width);

            mem_copy_2d(dest + y_size, video->width / 2,
                 pic->p[1].p_pixels, pic->p[1].i_pitch,
                 video->crop.height / 2,
                 video->crop.width / 2);

            mem_copy_2d(dest + y_size + y_size / 4, video->width / 2,
                 pic->p[2].p_pixels, pic->p[2].i_pitch,
                 video->crop.height / 2,
                 video->crop.width / 2);

            // And make sure it is actually in memory
            length = y_size + y_size / 2;
            break;
        }

        case VLC_CODEC_I420_10L:
        {
            const size_t y_size = video->width * video->height;
            mem_copy_2d_10_to_8(dest, video->width,
                 pic->p[0].p_pixels, pic->p[0].i_pitch,
                 video->crop.height,
                 video->crop.width);

            mem_copy_2d_10_to_8(dest + y_size, video->width / 2,
                 pic->p[1].p_pixels, pic->p[1].i_pitch,
                 video->crop.height / 2,
                 video->crop.width / 2);

            mem_copy_2d_10_to_8(dest + y_size + y_size / 4, video->width / 2,
                 pic->p[2].p_pixels, pic->p[2].i_pitch,
                 video->crop.height / 2,
                 video->crop.width / 2);

            // And make sure it is actually in memory
            length = y_size + y_size / 2;
            break;
        }

        default:
            if (pLength != NULL)
                *pLength = 0;
            return VLC_EBADVAR;
    }

    if (cma_vcsm_type() == VCSM_INIT_LEGACY) {  // ** CMA is currently always uncached
        flush_range(dest, length);
    }

    if (pLength != NULL)
        *pLength = (uint32_t)length;

    return VLC_SUCCESS;
}


static MMAL_BOOL_T rep_buf_free_cb(MMAL_BUFFER_HEADER_T *header, void *userdata)
{
    cma_buf_t * const cb = userdata;
    VLC_UNUSED(header);

    cma_buf_unref(cb);
    return MMAL_FALSE;
}

static int cma_buf_buf_attach(MMAL_BUFFER_HEADER_T * const buf, cma_buf_t * const cb)
{
    // Just a CMA buffer - fill in new buffer
    const uintptr_t vc_h = cma_buf_vc_handle(cb);
    if (vc_h == 0)
        return VLC_EGENERIC;

    mmal_buffer_header_reset(buf);
    buf->data       = (uint8_t *)vc_h;
    buf->alloc_size = cma_buf_size(cb);
    buf->length     = buf->alloc_size;
    // Ensure cb remains valid for the duration of this buffer
    mmal_buffer_header_pre_release_cb_set(buf, rep_buf_free_cb, cma_buf_ref(cb));
    return VLC_SUCCESS;
}

MMAL_BUFFER_HEADER_T * hw_mmal_pic_buf_copied(const picture_t *const pic,
                                              MMAL_POOL_T * const rep_pool,
                                              MMAL_PORT_T * const port,
                                              cma_buf_pool_t * const cbp)
{
    MMAL_BUFFER_HEADER_T *const buf = mmal_queue_wait(rep_pool->queue);
    if (buf == NULL)
        goto fail0;

    cma_buf_t * const cb = cma_buf_pool_alloc_buf(cbp, port->buffer_size);
    if (cb == NULL)
        goto fail1;

    if (cma_buf_buf_attach(buf, cb) != VLC_SUCCESS)
        goto fail2;

    pic_to_buf_copy_props(buf, pic);

    if (hw_mmal_copy_pic_to_buf(cma_buf_addr(cb), &buf->length, port->format, pic) != VLC_SUCCESS)
        goto fail2;
    buf->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END;

    cma_buf_unref(cb);
    return buf;

fail2:
    cma_buf_unref(cb);
fail1:
    mmal_buffer_header_release(buf);
fail0:
    return NULL;
}

MMAL_BUFFER_HEADER_T * hw_mmal_pic_buf_replicated(const picture_t *const pic, MMAL_POOL_T * const rep_pool)
{
    pic_ctx_mmal_t *const ctx = (pic_ctx_mmal_t *)pic->context;
    MMAL_BUFFER_HEADER_T *const rep_buf = mmal_queue_wait(rep_pool->queue);

    if (rep_buf == NULL)
        return NULL;

    if (ctx->bufs[0] != NULL)
    {
        // Existing buffer - replicate it
        if (mmal_buffer_header_replicate(rep_buf, ctx->bufs[0]) != MMAL_SUCCESS)
            goto fail;
    }
    else if (ctx->cb != NULL)
    {
        // Just a CMA buffer - fill in new buffer
        if (cma_buf_buf_attach(rep_buf, ctx->cb) != 0)
            goto fail;
    }
    else
        goto fail;

    pic_to_buf_copy_props(rep_buf, pic);
    return rep_buf;

fail:
    mmal_buffer_header_release(rep_buf);
    return NULL;
}




int hw_mmal_get_gpu_mem(void) {
    static int stashed_val = -2;
    VCHI_INSTANCE_T vchi_instance;
    VCHI_CONNECTION_T *vchi_connection = NULL;
    char rbuf[1024] = { 0 };

    if (stashed_val >= -1)
        return stashed_val;

    if (vchi_initialise(&vchi_instance) != 0)
        goto fail0;

    //create a vchi connection
    if (vchi_connect(NULL, 0, vchi_instance) != 0)
        goto fail0;

    vc_vchi_gencmd_init(vchi_instance, &vchi_connection, 1);

    //send the gencmd for the argument
    if (vc_gencmd_send("get_mem gpu") != 0)
        goto fail;

    if (vc_gencmd_read_response(rbuf, sizeof(rbuf) - 1) != 0)
        goto fail;

    if (strncmp(rbuf, "gpu=", 4) != 0)
        goto fail;

    char *p;
    unsigned long m = strtoul(rbuf + 4, &p, 10);

    if (p[0] != 'M' || p[1] != '\0')
        stashed_val = -1;
    else
        stashed_val = (int)m << 20;

    vc_gencmd_stop();

    //close the vchi connection
    vchi_disconnect(vchi_instance);

    return stashed_val;

fail:
    vc_gencmd_stop();
    vchi_disconnect(vchi_instance);
fail0:
    stashed_val = -1;
    return -1;
};

// ===========================================================================

typedef struct pool_ent_s
{
    struct pool_ent_s * next;
    struct pool_ent_s * prev;

    atomic_int ref_count;
    unsigned int seq;

    size_t size;

    int vcsm_hdl;
    int vc_hdl;
    void * buf;

    unsigned int width;
    unsigned int height;
    MMAL_FOURCC_T enc_type;

    picture_t * pic;
} pool_ent_t;


typedef struct ent_list_hdr_s
{
    pool_ent_t * ents;
    pool_ent_t * tail;
    unsigned int n;
} ent_list_hdr_t;

#define ENT_LIST_HDR_INIT (ent_list_hdr_t){ \
   .ents = NULL, \
   .tail = NULL, \
   .n = 0 \
}

struct vzc_pool_ctl_s
{
    atomic_int ref_count;

    ent_list_hdr_t ent_pool;
    ent_list_hdr_t ents_cur;
    ent_list_hdr_t ents_prev;

    unsigned int max_n;
    unsigned int seq;

    vlc_mutex_t lock;

    MMAL_POOL_T * buf_pool;

    vcsm_init_type_t vcsm_init_type;
};

typedef struct vzc_subbuf_ent_s
{
    pool_ent_t * ent;
    MMAL_RECT_T pic_rect;
    MMAL_RECT_T orig_dest_rect;
    MMAL_DISPLAYREGION_T dreg;
} vzc_subbuf_ent_t;


static pool_ent_t * ent_extract(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Ext %p\n", elh, elh->n, ent);

    if (ent == NULL)
        return NULL;

    if (ent->next == NULL)
        elh->tail = ent->prev;
    else
        ent->next->prev = ent->prev;

    if (ent->prev == NULL)
        elh->ents = ent->next;
    else
        ent->prev->next = ent->next;

    ent->prev = ent->next = NULL;

    --elh->n;

    return ent;  // For convienience
}

static inline pool_ent_t * ent_extract_tail(ent_list_hdr_t * const elh)
{
    return ent_extract(elh, elh->tail);
}

static void ent_add_head(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Add %p\n", elh, elh->n, ent);

    if ((ent->next = elh->ents) == NULL)
        elh->tail = ent;
    else
        ent->next->prev = ent;

    ent->prev = NULL;
    elh->ents = ent;
    ++elh->n;
}

static void ent_free(pool_ent_t * const ent)
{
//    printf("Free ent: %p\n", ent);
    if (ent != NULL) {
        // If we still have a ref to a pic - kill it now
        if (ent->pic != NULL)
            picture_Release(ent->pic);

        // Free contents
        vcsm_unlock_hdl(ent->vcsm_hdl);

        vcsm_free(ent->vcsm_hdl);

        free(ent);
    }
}

static void ent_free_list(ent_list_hdr_t * const elh)
{
    pool_ent_t * ent = elh->ents;

//    printf("Free list: %p [%d]\n", elh, elh->n);

    *elh = ENT_LIST_HDR_INIT;

    while (ent != NULL) {
        pool_ent_t * const t = ent;
        ent = t->next;
        ent_free(t);
    }
}

static void ent_list_move(ent_list_hdr_t * const dst, ent_list_hdr_t * const src)
{
//    printf("Move %p->%p\n", src, dst);

    *dst = *src;
    *src = ENT_LIST_HDR_INIT;
}

// Scans "backwards" as that should give us the fastest match if we are
// presented with pics in the same order each time
static pool_ent_t * ent_list_extract_pic_ent(ent_list_hdr_t * const elh, picture_t * const pic)
{
    pool_ent_t *ent = elh->tail;

//    printf("Find list: %p [%d]; pic:%p\n", elh, elh->n, pic);

    while (ent != NULL) {
//        printf("Check ent: %p, pic:%p\n", ent, ent->pic);

        if (ent->pic == pic)
            return ent_extract(elh, ent);
        ent = ent->prev;
    }
    return NULL;
}

#define POOL_ENT_ALLOC_BLOCK  0x10000

static pool_ent_t * pool_ent_alloc_new(size_t req_size)
{
    pool_ent_t * ent = calloc(1, sizeof(*ent));
    const size_t alloc_size = (req_size + POOL_ENT_ALLOC_BLOCK - 1) & ~(POOL_ENT_ALLOC_BLOCK - 1);

    if (ent == NULL)
        return NULL;

    ent->next = ent->prev = NULL;

    // Alloc from vcsm
    if ((ent->vcsm_hdl = vcsm_malloc_cache(alloc_size, VCSM_CACHE_TYPE_HOST, (char *)"vlc-subpic")) == -1)
        goto fail1;
    if ((ent->vc_hdl = vcsm_vc_hdl_from_hdl(ent->vcsm_hdl)) == 0)
        goto fail2;
    if ((ent->buf = vcsm_lock(ent->vcsm_hdl)) == NULL)
        goto fail2;

    ent->size = alloc_size;
    return ent;

fail2:
    vcsm_free(ent->vcsm_hdl);
fail1:
    free(ent);
    return NULL;
}

static inline pool_ent_t * pool_ent_ref(pool_ent_t * const ent)
{
//    int n = atomic_fetch_add(&ent->ref_count, 1) + 1;
//    printf("Ref: %p: %d\n", ent, n);
    atomic_fetch_add(&ent->ref_count, 1);
    return ent;
}

static void pool_recycle(vzc_pool_ctl_t * const pc, pool_ent_t * const ent)
{
    pool_ent_t * xs = NULL;
    int n;

    if (ent == NULL)
        return;

    n = atomic_fetch_sub(&ent->ref_count, 1) - 1;

//    printf("%s: Pool: %p: Ent: %p: %d\n", __func__, &pc->ent_pool, ent, n);

    if (n != 0)
        return;

    if (ent->pic != NULL) {
        picture_Release(ent->pic);
        ent->pic = NULL;
    }

    vlc_mutex_lock(&pc->lock);

    // If we have a full pool then extract the LRU and free it
    // Free done outside mutex
    if (pc->ent_pool.n >= pc->max_n)
        xs = ent_extract_tail(&pc->ent_pool);

    ent_add_head(&pc->ent_pool, ent);

    vlc_mutex_unlock(&pc->lock);

    ent_free(xs);
}

// * This could be made more efficient, but this is easy
static void pool_recycle_list(vzc_pool_ctl_t * const pc, ent_list_hdr_t * const elh)
{
    pool_ent_t * ent;
    while ((ent = ent_extract_tail(elh)) != NULL) {
        pool_recycle(pc, ent);
    }
}

static pool_ent_t * pool_best_fit(vzc_pool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * best = NULL;

    vlc_mutex_lock(&pc->lock);

    {
        pool_ent_t * ent = pc->ent_pool.ents;

        // Simple scan
        while (ent != NULL) {
            if (ent->size >= req_size && ent->size <= req_size * 2 + POOL_ENT_ALLOC_BLOCK &&
                    (best == NULL || best->size > ent->size))
                best = ent;
            ent = ent->next;
        }

        // extract best from chain if we've found it
        ent_extract(&pc->ent_pool, best);
    }

    vlc_mutex_unlock(&pc->lock);

    if (best == NULL)
        best = pool_ent_alloc_new(req_size);

    if ((best->seq = ++pc->seq) == 0)
        best->seq = ++pc->seq;  // Never allow to be zero

    atomic_store(&best->ref_count, 1);
    return best;
}


const vlc_fourcc_t hw_mmal_vzc_subpicture_chromas[] = { VLC_CODEC_RGBA, VLC_CODEC_BGRA, VLC_CODEC_ARGB, 0 };

void hw_mmal_vzc_buf_get_wh(MMAL_BUFFER_HEADER_T * const buf, int * const pW, int * const pH)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    *pW = ent->width;
    *pH = ent->height;
}

bool hw_mmal_vzc_buf_set_format(MMAL_BUFFER_HEADER_T * const buf, MMAL_ES_FORMAT_T * const es_fmt)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    MMAL_VIDEO_FORMAT_T * const v_fmt = &es_fmt->es->video;

    es_fmt->type = MMAL_ES_TYPE_VIDEO;
    es_fmt->encoding = ent->enc_type;
    es_fmt->encoding_variant = 0;

    v_fmt->width = ent->width;
    v_fmt->height = ent->height;
    v_fmt->crop.x = 0;
    v_fmt->crop.y = 0;
    v_fmt->crop.width = ent->width;
    v_fmt->crop.height = ent->height;

    return true;
}

void hw_mmal_vzc_buf_frame_size(MMAL_BUFFER_HEADER_T * const buf,
                                uint32_t * const pWidth, uint32_t * const pHeight)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    *pWidth = ent->width;
    *pHeight = ent->height;
}


MMAL_DISPLAYREGION_T * hw_mmal_vzc_buf_region(MMAL_BUFFER_HEADER_T * const buf)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    return &sb->dreg;
}

static inline int rescale_x(int x, int mul, int div)
{
    return div == 0 ? x * mul : (x * mul + div/2) / div;
}

static void rescale_rect(MMAL_RECT_T * const d, const MMAL_RECT_T * const s, const MMAL_RECT_T * mul_rect, const MMAL_RECT_T * div_rect)
{
    d->x      = rescale_x(s->x - div_rect->x, mul_rect->width,  div_rect->width)  + mul_rect->x;
    d->y      = rescale_x(s->y - div_rect->y, mul_rect->height, div_rect->height) + mul_rect->y;
    d->width  = rescale_x(s->width,           mul_rect->width,  div_rect->width);
    d->height = rescale_x(s->height,          mul_rect->height, div_rect->height);
#if TRACE_TRANSFORMS
    fprintf(stderr, "(%d,%d %dx%d) * (%d,%d %dx%d) / (%d,%d %dx%d) -> (%d,%d %dx%d)\n",
            s->x, s->y, s->width, s->height,
            mul_rect->x, mul_rect->y, mul_rect->width, mul_rect->height,
            div_rect->x, div_rect->y, div_rect->width, div_rect->height,
            d->x, d->y, d->width, d->height);
#endif
}

static MMAL_RECT_T
rect_untransform(MMAL_RECT_T s, const MMAL_RECT_T c, const MMAL_DISPLAYTRANSFORM_T t)
{
#if TRACE_TRANSFORMS
    fprintf(stderr, "t=%d, s=%d,%d:%dx%d, c=%d,%d:%dx%d -> ", (int)t,
           s.x,s.y,s.width,s.height,
           c.x,c.y,c.width,c.height);
#endif
    if (is_transform_hflip(t))
        s = rect_hflip(s, c);
    if (is_transform_vflip(t) != 0)
        s = rect_vflip(s, c);
    if (is_transform_transpose(t) != 0)
        s = rect_transpose(s);
#if TRACE_TRANSFORMS
    fprintf(stderr, "s=%d,%d:%dx%d\n",
           s.x,s.y,s.width,s.height);
#endif
    return s;
}

void hw_mmal_vzc_buf_scale_dest_rect(MMAL_BUFFER_HEADER_T * const buf, const MMAL_RECT_T * const scale_rect, const MMAL_DISPLAYTRANSFORM_T scale_transform)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    if (scale_rect == NULL) {
        sb->dreg.dest_rect = sb->orig_dest_rect;
        sb->dreg.transform = MMAL_DISPLAY_ROT0;
    }
    else
    {
        // The scale rect has been transposed if we have a transposing
        // transform - untranspose so we are the same way up as the source
        const MMAL_RECT_T c = (scale_transform & 4) == 0 ? *scale_rect : rect_transpose(*scale_rect);
        rescale_rect(&sb->dreg.dest_rect, &sb->orig_dest_rect,
                     &c, &sb->pic_rect);
        sb->dreg.dest_rect = rect_untransform(sb->dreg.dest_rect, c, scale_transform);
        sb->dreg.transform = scale_transform;
    }
}

unsigned int hw_mmal_vzc_buf_seq(MMAL_BUFFER_HEADER_T * const buf)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    return sb->ent->seq;
}


// The intent with the ents_cur & ents_last stuff is to remember the buffers
// we used on the last frame and reuse them on the current one if they are the
// same.  Unfortunately detection of "is_first" is only a heuristic (there are
// no rules governing the order in which things are blended) so we must deal
// (fairly) gracefully with it never (or always) being set.

// dst_fmt gives the number space in which the destination pixels are specified

MMAL_BUFFER_HEADER_T * hw_mmal_vzc_buf_from_pic(vzc_pool_ctl_t * const pc,
                                                picture_t * const pic,
                                                const video_format_t * src_fmt,
                                                const MMAL_RECT_T dst_pic_rect,
                                                const int x_offset, const int y_offset,
                                                const unsigned int alpha,
                                                const bool is_first)
{
    MMAL_BUFFER_HEADER_T * const buf = mmal_queue_get(pc->buf_pool->queue);
    vzc_subbuf_ent_t * sb;

    if (buf == NULL)
        return NULL;

    if ((sb = calloc(1, sizeof(*sb))) == NULL)
        goto fail1;

    // If first or we've had a lot of stuff move everything to the last list
    // (we could deal more gracefully with the "too many" case but it shouldn't
    // really happen)
    if (is_first || pc->ents_cur.n >= CTX_BUFS_MAX) {
        pool_recycle_list(pc, &pc->ents_prev);
        ent_list_move(&pc->ents_prev, &pc->ents_cur);
    }

    sb->dreg.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
    sb->dreg.hdr.size = sizeof(sb->dreg);
    buf->user_data = sb;

    {
        // ?? Round start offset as well as length
        const video_format_t *const fmt = &pic->format;

        const unsigned int bpp = (fmt->i_bits_per_pixel + 7) >> 3;
        const unsigned int xl = (fmt->i_x_offset & ~15);
        const unsigned int xr = (fmt->i_x_offset + fmt->i_visible_width + 15) & ~15;
        const size_t dst_stride = (xr - xl) * bpp;
        const size_t dst_lines = ((fmt->i_visible_height + 15) & ~15);
        const size_t dst_size = dst_stride * dst_lines;

        pool_ent_t * ent = ent_list_extract_pic_ent(&pc->ents_prev, pic);
        bool needs_copy = false;

        // If we didn't find ent in last then look in cur in case is_first
        // isn't working
        if (ent == NULL)
            ent = ent_list_extract_pic_ent(&pc->ents_cur, pic);

//        printf("ent_found: %p\n", ent);

        if (ent == NULL)
        {
            // Need a new ent
            needs_copy = true;

            if ((ent = pool_best_fit(pc, dst_size)) == NULL)
                goto fail2;
            if ((ent->enc_type = vlc_to_mmal_video_fourcc(&pic->format)) == 0)
                goto fail2;

            ent->pic = picture_Hold(pic);
        }

        ent_add_head(&pc->ents_cur, ent);

        sb->ent = pool_ent_ref(ent);
        hw_mmal_vzc_pool_ref(pc);

        // Copy data
        buf->next = NULL;
        buf->cmd = 0;
        buf->data = (uint8_t *)(ent->vc_hdl);
        buf->alloc_size = buf->length = dst_size;
        buf->offset = 0;
        buf->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END;
        buf->pts = buf->dts = pic->date != VLC_TICK_INVALID ? pic->date : MMAL_TIME_UNKNOWN;
        buf->type->video = (MMAL_BUFFER_HEADER_VIDEO_SPECIFIC_T){
            .planes = 1,
            .pitch = { dst_stride }
        };

        // Remember offsets
        sb->dreg.set = MMAL_DISPLAY_SET_SRC_RECT |
            MMAL_DISPLAY_SET_DEST_RECT |
            MMAL_DISPLAY_SET_FULLSCREEN |
            MMAL_DISPLAY_SET_TRANSFORM |
            MMAL_DISPLAY_SET_ALPHA;

        sb->dreg.fullscreen = 0;

        // Will be set later - zero now to avoid any confusion
        sb->dreg.transform = MMAL_DISPLAY_ROT0;
        sb->dreg.dest_rect = (MMAL_RECT_T){0, 0, 0, 0};

        sb->dreg.alpha = (uint32_t)(alpha & 0xff) | MMAL_DISPLAY_ALPHA_FLAGS_MIX;

//        printf("+++ bpp:%d, vis:%dx%d wxh:%dx%d, d:%dx%d\n", bpp, fmt->i_visible_width, fmt->i_visible_height, fmt->i_width, fmt->i_height, dst_stride, dst_lines);

        sb->dreg.src_rect = (MMAL_RECT_T){
            .x      = (fmt->i_x_offset - xl) + src_fmt->i_x_offset,
            .y      = src_fmt->i_y_offset,
            .width  = src_fmt->i_visible_width,
            .height = src_fmt->i_visible_height
        };

        sb->pic_rect = dst_pic_rect;

        sb->orig_dest_rect = (MMAL_RECT_T){
            .x      = x_offset,
            .y      = y_offset,
            .width  = src_fmt->i_visible_width,
            .height = src_fmt->i_visible_height
        };

        if (needs_copy)
        {
            ent->width = dst_stride / bpp;
            ent->height = dst_lines;

            // 2D copy
            {
                uint8_t *d = ent->buf;
                const uint8_t *s = pic->p[0].p_pixels + xl * bpp + fmt->i_y_offset * pic->p[0].i_pitch;

                mem_copy_2d(d, dst_stride, s, pic->p[0].i_pitch, fmt->i_visible_height, dst_stride);

                // And make sure it is actually in memory
                if (pc->vcsm_init_type != VCSM_INIT_CMA) {  // ** CMA is currently always uncached
                    flush_range(ent->buf, dst_stride * fmt->i_visible_height);
                }
            }
        }
    }

    return buf;

fail2:
    free(sb);
fail1:
    mmal_buffer_header_release(buf);
    return NULL;
}

void hw_mmal_vzc_pool_flush(vzc_pool_ctl_t * const pc)
{
    pool_recycle_list(pc, &pc->ents_prev);
    pool_recycle_list(pc, &pc->ents_cur);
}

static void hw_mmal_vzc_pool_delete(vzc_pool_ctl_t * const pc)
{

//    printf("<<< %s\n", __func__);

    hw_mmal_vzc_pool_flush(pc);

    ent_free_list(&pc->ent_pool);

    if (pc->buf_pool != NULL)
        mmal_pool_destroy(pc->buf_pool);

    vlc_mutex_destroy(&pc->lock);

    cma_vcsm_exit(pc->vcsm_init_type);

//    memset(pc, 0xba, sizeof(*pc)); // Zap for (hopefully) faster crash
    free (pc);

    //    printf(">>> %s\n", __func__);
}

void hw_mmal_vzc_pool_release(vzc_pool_ctl_t * const pc)
{
    int n;

    if (pc == NULL)
        return;

    n = atomic_fetch_sub(&pc->ref_count, 1) - 1;

    if (n != 0)
        return;

    hw_mmal_vzc_pool_delete(pc);
}

void hw_mmal_vzc_pool_ref(vzc_pool_ctl_t * const pc)
{
    atomic_fetch_add(&pc->ref_count, 1);
}

static MMAL_BOOL_T vcz_pool_release_cb(MMAL_POOL_T * buf_pool, MMAL_BUFFER_HEADER_T *buf, void *userdata)
{
    vzc_pool_ctl_t * const pc = userdata;
    vzc_subbuf_ent_t * const sb = buf->user_data;

    VLC_UNUSED(buf_pool);

//    printf("<<< %s\n", __func__);

    if (sb != NULL) {
        buf->user_data = NULL;
        pool_recycle(pc, sb->ent);
        hw_mmal_vzc_pool_release(pc);
        free(sb);
    }

//    printf(">>> %s\n", __func__);

    return MMAL_TRUE;
}

vzc_pool_ctl_t * hw_mmal_vzc_pool_new()
{
    vzc_pool_ctl_t * const pc = calloc(1, sizeof(*pc));

    if (pc == NULL)
        return NULL;

    if ((pc->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE)
    {
        free(pc);
        return NULL;
    }

    pc->max_n = 8;
    vlc_mutex_init(&pc->lock);  // Must init before potential destruction

    if ((pc->buf_pool = mmal_pool_create(64, 0)) == NULL)
    {
        hw_mmal_vzc_pool_delete(pc);
        return NULL;
    }

    atomic_store(&pc->ref_count, 1);

    mmal_pool_callback_set(pc->buf_pool, vcz_pool_release_cb, pc);

    return pc;
}

//----------------------------------------------------------------------------


static const uint8_t shift_00[] = {0,0,0,0};
static const uint8_t shift_01[] = {0,1,1,1};

int cma_pic_set_data(picture_t * const pic,
                            const MMAL_ES_FORMAT_T * const mm_esfmt,
                            const MMAL_BUFFER_HEADER_T * const buf)
{
    const MMAL_VIDEO_FORMAT_T * const mm_fmt = &mm_esfmt->es->video;
    const MMAL_BUFFER_HEADER_VIDEO_SPECIFIC_T *const buf_vid = (buf == NULL) ? NULL : &buf->type->video;
    cma_buf_t *const cb = cma_buf_pic_get(pic);
    unsigned int planes = 1;

    uint8_t * const data = cma_buf_addr(cb);
    if (data == NULL) {
        return VLC_ENOMEM;
    }

    const uint8_t * ws = shift_00;
    const uint8_t * hs = shift_00;
    int pb = 1;

    switch (mm_esfmt->encoding)
    {
        case MMAL_ENCODING_ARGB:
        case MMAL_ENCODING_ABGR:
        case MMAL_ENCODING_RGBA:
        case MMAL_ENCODING_BGRA:
        case MMAL_ENCODING_RGB32:
        case MMAL_ENCODING_BGR32:
            pb = 4;
            break;
        case MMAL_ENCODING_RGB16:
            pb = 2;
            break;

        case MMAL_ENCODING_I420:
            ws = shift_01;
            hs = shift_01;
            planes = 3;
            break;

        case MMAL_ENCODING_YUVUV128:
            hs = shift_01;
            planes = 2;
            break;

        default:
//            msg_Err(p_filter, "%s: Unexpected format", __func__);
            return VLC_EGENERIC;
    }

    // Fix up SAR if unset
    if (pic->format.i_sar_den == 0 || pic->format.i_sar_num == 0) {
        pic->format.i_sar_den = mm_fmt->par.den;
        pic->format.i_sar_num = mm_fmt->par.num;
    }

    pic->i_planes = planes;
    unsigned int offset = 0;
    for (unsigned int i = 0; i != planes; ++i) {
        pic->p[i] = (plane_t){
            .p_pixels = data + (buf_vid != NULL ? buf_vid->offset[i] : offset),
            .i_lines = mm_fmt->height >> hs[i],
            .i_pitch = buf_vid != NULL ? buf_vid->pitch[i] : mm_fmt->width * pb,
            .i_pixel_pitch = pb,
            .i_visible_lines = mm_fmt->crop.height >> hs[i],
            .i_visible_pitch = mm_fmt->crop.width >> ws[i]
        };
        offset += pic->p[i].i_pitch * pic->p[i].i_lines;
    }
    return VLC_SUCCESS;
}

int cma_buf_pic_attach(cma_buf_t * const cb, picture_t * const pic)
{
    if (!is_cma_buf_pic_chroma(pic->format.i_chroma))
        return VLC_EGENERIC;
    if (pic->context != NULL)
        return VLC_EBADVAR;

    pic_ctx_mmal_t * const ctx = calloc(1, sizeof(pic_ctx_mmal_t));

    if (ctx == NULL)
        return VLC_ENOMEM;

    ctx->cmn.copy = hw_mmal_pic_ctx_copy;
    ctx->cmn.destroy = hw_mmal_pic_ctx_destroy;
    ctx->buf_count = 1; // cb takes the place of the 1st buf
    ctx->cb = cb;

    cma_buf_in_flight(cb);

    pic->context = &ctx->cmn;
    return VLC_SUCCESS;
}


//----------------------------------------------------------------------------

/* Returns the type of the Pi being used
*/
bool rpi_is_model_pi4(void)
{
    return bcm_host_is_model_pi4();
}

// Board types that support PI3 hybrid HEVC accel
bool rpi_use_pi3_hevc(void)
{
    const int t = bcm_host_get_model_type();
    return
        t == BCM_HOST_BOARD_TYPE_PI3MODELB ||
        t == BCM_HOST_BOARD_TYPE_CM3 ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELBPLUS ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELBPLUS ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELAPLUS ||
        t == BCM_HOST_BOARD_TYPE_CM3PLUS;
}

// Board types that support qpu adv deinterlace
bool rpi_use_qpu_deinterlace(void)
{
    const int t = bcm_host_get_model_type();
    return
        t == BCM_HOST_BOARD_TYPE_MODELA ||
        t == BCM_HOST_BOARD_TYPE_MODELB ||
        t == BCM_HOST_BOARD_TYPE_MODELAPLUS ||
        t == BCM_HOST_BOARD_TYPE_MODELBPLUS ||
        t == BCM_HOST_BOARD_TYPE_PI2MODELB ||
        t == BCM_HOST_BOARD_TYPE_CM ||
        t == BCM_HOST_BOARD_TYPE_CM2 ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELB ||
        t == BCM_HOST_BOARD_TYPE_PI0 ||
        t == BCM_HOST_BOARD_TYPE_CM3 ||
        t == BCM_HOST_BOARD_TYPE_PI0W ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELBPLUS ||
        t == BCM_HOST_BOARD_TYPE_PI3MODELAPLUS ||
        t == BCM_HOST_BOARD_TYPE_CM3PLUS;
}


// Preferred mode - none->cma on Pi4 otherwise legacy
static volatile vcsm_init_type_t last_vcsm_type = VCSM_INIT_NONE;

vcsm_init_type_t cma_vcsm_type(void)
{
    return last_vcsm_type;
}

vcsm_init_type_t cma_vcsm_init(void)
{
    vcsm_init_type_t rv = VCSM_INIT_NONE;
    // We don't bother locking - taking a copy here should be good enough
    vcsm_init_type_t try_type = last_vcsm_type;

    if (try_type == VCSM_INIT_NONE) {
        if (bcm_host_is_fkms_active() ||
            bcm_host_is_kms_active())
            try_type = VCSM_INIT_CMA;
        else
            try_type = VCSM_INIT_LEGACY;
    }

    if (try_type == VCSM_INIT_CMA) {
        if (vcsm_init_ex(1, -1) == 0)
            rv = VCSM_INIT_CMA;
        else if (vcsm_init_ex(0, -1) == 0)
            rv = VCSM_INIT_LEGACY;
    }
    else
    {
        if (vcsm_init_ex(0, -1) == 0)
            rv = VCSM_INIT_LEGACY;
        else if (vcsm_init_ex(1, -1) == 0)
            rv = VCSM_INIT_CMA;
    }

    // Just in case this affects vcsm init do after that
    if (rv != VCSM_INIT_NONE)
        bcm_host_init();

    last_vcsm_type = rv;
    return rv;
}

void cma_vcsm_exit(const vcsm_init_type_t init_mode)
{
    if (init_mode != VCSM_INIT_NONE)
    {
        vcsm_exit();
        bcm_host_deinit();  // Does nothing but add in case it ever does
    }
}

const char * cma_vcsm_init_str(const vcsm_init_type_t init_mode)
{
    switch (init_mode)
    {
        case VCSM_INIT_CMA:
            return "CMA";
        case VCSM_INIT_LEGACY:
            return "Legacy";
        case VCSM_INIT_NONE:
            return "none";
        default:
            break;
    }
    return "???";
}


