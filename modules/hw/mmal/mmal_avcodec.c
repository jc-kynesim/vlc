/*****************************************************************************
 * video.c: video decoder using the libavcodec library
 *****************************************************************************
 * Copyright (C) 1999-2001 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@videolan.org>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "config.h"

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_avcodec.h>
#include <vlc_cpu.h>
#include <vlc_atomic.h>

#include <assert.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext_drm.h>
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 55, 16, 101 ) )
#include <libavutil/mastering_display_metadata.h>
#endif

//#include "avcodec.h"
//#include "va.h"

#include <vlc_plugin.h>
#include <libavutil/rpi_sand_fns.h>
#include <libavcodec/rpi_zc.h>
#include "../../codec/cc.h"
#include "../../codec/avcodec/avcommon.h"  // ??? Beware over inclusion
#include "mmal_cma.h"
#include "mmal_cma_drmprime.h"
#include "mmal_picture.h"

#include <libdrm/drm_fourcc.h>

#define TRACE_ALL 0

#define BUFFERS_IN_FLIGHT       5       // Default max value for in flight buffers
#define BUFFERS_IN_FLIGHT_UHD   3       // Fewer if very big

#define MMAL_AVCODEC_BUFFERS "mmal-avcodec-buffers"
#define MMAL_AVCODEC_BUFFERS_TEXT N_("In flight buffer count before blocking.")
#define MMAL_AVCODEC_BUFFERS_LONGTEXT N_("In flight buffer count before blocking. " \
"Beware that incautious changing of this can lead to lockup. " \
"Zero will disable the module.")


// Fwd declarations required due to wanting to avoid reworking the original
// code too much
static void MmalAvcodecCloseDecoder( vlc_object_t *obj );


/*****************************************************************************
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    AVCodecContext *p_context;
    const AVCodec  *p_codec;

    /* Video decoder specific part */
    date_t  pts;

    /* Closed captions for decoders */
    cc_data_t cc;

    /* for frame skipping algo */
    bool b_hurry_up;
    bool b_show_corrupted;
    bool b_from_preroll;
    enum AVDiscard i_skip_frame;

    /* how many decoded frames are late */
    int     i_late_frames;
    mtime_t i_late_frames_start;
    mtime_t i_last_late_delay;

    /* for direct rendering */
    bool        b_direct_rendering;
    atomic_bool b_dr_failure;

    /* Hack to force display of still pictures */
    bool b_first_frame;


    /* */
    bool palette_sent;

    /* VA API */
//    vlc_va_t *p_va;
    enum PixelFormat pix_fmt;
    int profile;
    int level;

    vlc_sem_t sem_mt;

    // Rpi vars
    cma_buf_pool_t * cma_pool;
    bool pool_alloc_1;
    vcsm_init_type_t vcsm_init_type;
    int cma_in_flight_max;
    bool use_drm;
    // Debug
    decoder_t * p_dec;
};


static vlc_fourcc_t
ZcFindVlcChroma(const int i_ffmpeg_chroma, const int i_ffmpeg_sw_chroma)
{
    int c = i_ffmpeg_chroma;

    if (c == AV_PIX_FMT_DRM_PRIME)
        c = i_ffmpeg_sw_chroma;

    switch (c)
    {
        // This is all we claim to deal with
        // In theory RGB should be doable within our current framework
        case AV_PIX_FMT_YUV420P:
            return VLC_CODEC_MMAL_ZC_I420;
        case AV_PIX_FMT_SAND128:
        case AV_PIX_FMT_RPI4_8:
            return VLC_CODEC_MMAL_ZC_SAND8;
        case AV_PIX_FMT_SAND64_10:
            return VLC_CODEC_MMAL_ZC_SAND10;
        case AV_PIX_FMT_RPI4_10:
            return VLC_CODEC_MMAL_ZC_SAND30;
        default:
            break;
    }
    return 0;
}

// Pix Fmt conv for MMal
// video_fromat from ffmpeg pic_fmt
static int
ZcGetVlcChroma( video_format_t *fmt, int i_ffmpeg_chroma, const int i_ffmpeg_sw_chroma)
{
    fmt->i_rmask = 0;
    fmt->i_gmask = 0;
    fmt->i_bmask = 0;
    fmt->i_chroma = ZcFindVlcChroma(i_ffmpeg_chroma, i_ffmpeg_sw_chroma);

    return fmt->i_chroma == 0 ? -1 : 0;
}


// Format chooser is way simpler than vlc
static enum PixelFormat
ZcGetFormat(AVCodecContext *p_context, const enum PixelFormat *pi_fmt)
{
    enum PixelFormat swfmt = avcodec_default_get_format(p_context, pi_fmt);
    decoder_t * const p_dec = av_rpi_zc_in_use(p_context) ? NULL : p_context->opaque;
    const bool use_drm = (p_dec != NULL) && p_dec->p_sys->use_drm;

    for (size_t i = 0; pi_fmt[i] != AV_PIX_FMT_NONE; i++)
    {
        if (use_drm  && pi_fmt[i] == AV_PIX_FMT_DRM_PRIME)
            return pi_fmt[i];

        if (!use_drm && pi_fmt[i] != AV_PIX_FMT_DRM_PRIME &&
            ZcFindVlcChroma(pi_fmt[i], p_context->sw_pix_fmt) != 0)
            return pi_fmt[i];
    }
    return swfmt;
}


static void cma_avbuf_pool_free(void * v)
{
    cma_buf_unref(v);
}

static unsigned int zc_buf_vcsm_handle(void * v)
{
    return cma_buf_vcsm_handle(v);
}

static unsigned int zc_buf_vc_handle(void * v)
{
    return cma_buf_vc_handle(v);
}

static void * zc_buf_map_arm(void * v)
{
    return cma_buf_addr(v);
}

static unsigned int zc_buf_map_vc(void * v)
{
    return cma_buf_vc_addr(v);
}



static const av_rpi_zc_buf_fn_tab_t zc_buf_fn_tab = {
    .free = cma_avbuf_pool_free,

    .vcsm_handle = zc_buf_vcsm_handle,
    .vc_handle = zc_buf_vc_handle,
    .map_arm = zc_buf_map_arm,
    .map_vc = zc_buf_map_vc
};


static AVBufferRef *
zc_alloc_buf(void * v, size_t size, const AVRpiZcFrameGeometry * geo)
{
    decoder_t * const dec = v;
    decoder_sys_t * const sys = dec->p_sys;

    VLC_UNUSED(geo);

    assert(sys != NULL);

    const unsigned int dec_pool_req = av_rpi_zc_get_decoder_pool_size(sys->p_context->opaque);
    if (dec_pool_req != 0)
    {
        cma_buf_pool_resize(sys->cma_pool, dec_pool_req + sys->cma_in_flight_max, sys->cma_in_flight_max);

        if (!sys->pool_alloc_1)
        {
            sys->pool_alloc_1 = true;
            msg_Dbg(dec, "Pool size: (%d+%d) * %zd", dec_pool_req, sys->cma_in_flight_max, size);
            if (cma_buf_pool_fill(sys->cma_pool, size) != 0)
                msg_Warn(dec, "Failed to preallocate decoder pool (%d+%d) * %zd", dec_pool_req, sys->cma_in_flight_max, size);
        }
    }

    void * const cmabuf = cma_buf_pool_alloc_buf(sys->cma_pool, size);

    if (cmabuf == NULL)
    {
        msg_Err(dec, "CMA buf pool alloc buf failed");
        return NULL;
    }

    AVBufferRef *const avbuf = av_rpi_zc_buf(cma_buf_size(cmabuf), 0, cmabuf, &zc_buf_fn_tab);

    if (avbuf == NULL)
    {
        msg_Err(dec, "av_rpi_zc_buf failed");
        cma_buf_unref(cmabuf);
        return NULL;
    }

    return avbuf;
}

static void
zc_free_pool(void * v)
{
    decoder_t * const dec = v;
    cma_buf_pool_delete(dec->p_sys->cma_pool);
}


static const uint8_t shift_01[] = {0,1,1,1};
static const uint8_t pb_1[] = {1,1,1,1};
static const uint8_t pb_12[] = {1,2,2,2};
static const uint8_t pb_24[] = {2,4,4,4};
static const uint8_t pb_4[] = {4,4,4,4};

static inline int pitch_from_mod(const uint64_t mod)
{
    return fourcc_mod_broadcom_mod(mod) != DRM_FORMAT_MOD_BROADCOM_SAND128 ? 0 :
        fourcc_mod_broadcom_param(mod);
}

static int set_pic_from_frame(picture_t * const pic, const AVFrame * const frame)
{
    const uint8_t * hs = shift_01;
    const uint8_t * ws = shift_01;
    const uint8_t * pb = pb_1;

    switch (pic->format.i_chroma)
    {
        case VLC_CODEC_MMAL_ZC_RGB32:
            pic->i_planes = 1;
            pb = pb_4;
            break;
        case VLC_CODEC_MMAL_ZC_I420:
            pic->i_planes = 3;
            break;
        case VLC_CODEC_MMAL_ZC_SAND8:
            pic->i_planes = 2;
            pb = pb_12;
            break;
        case VLC_CODEC_MMAL_ZC_SAND10:
        case VLC_CODEC_MMAL_ZC_SAND30:  // Lies: SAND30 is "special"
            pic->i_planes = 2;
            pb = pb_24;
            break;
        default:
            return VLC_EGENERIC;
    }

    const cma_buf_t * const cb = cma_buf_pic_get(pic);
    uint8_t * const data = cma_buf_addr(cb);
    if (data == NULL) {
        return VLC_ENOMEM;
    }

    if (frame->format == AV_PIX_FMT_DRM_PRIME)
    {
        const AVDRMFrameDescriptor * const desc = (AVDRMFrameDescriptor*)frame->data[0];
        const AVDRMLayerDescriptor * layer = desc->layers + 0;
        const AVDRMPlaneDescriptor * plane = layer->planes + 0;
        const uint64_t mod = desc->objects[0].format_modifier;
        const int set_pitch = pitch_from_mod(mod);
        int nb_plane = 0;

        if (desc->nb_objects != 1)
            return VLC_EGENERIC;

        for (int i = 0; i != pic->i_planes; ++i)
        {
            if (nb_plane >= layer->nb_planes)
            {
                ++layer;
                plane = layer->planes + 0;
                nb_plane = 0;
            }

            pic->p[i] = (plane_t){
                .p_pixels = data + plane->offset,
                .i_lines = frame->height >> hs[i],
                .i_pitch = set_pitch != 0 ? set_pitch : plane->pitch,
                .i_pixel_pitch = pb[i],
                .i_visible_lines = av_frame_cropped_height(frame) >> hs[i],
                .i_visible_pitch = av_frame_cropped_width(frame) >> ws[i]
            };

            ++plane;
            ++nb_plane;
        }

        // Calculate lines from gap between planes
        // This will give us an accurate "height" for later use by MMAL
        for (int i = 0; i + 1 < pic->i_planes; ++i)
            pic->p[i].i_lines = (pic->p[i + 1].p_pixels - pic->p[i].p_pixels) / pic->p[i].i_pitch;
    }
    else
    {
        uint8_t * frame_end = frame->data[0] + cma_buf_size(cb);
        for (int i = 0; i != pic->i_planes; ++i) {
            // Calculate lines from gap between planes
            // This will give us an accurate "height" for later use by MMAL
            const int lines = ((i + 1 == pic->i_planes ? frame_end : frame->data[i + 1]) -
                               frame->data[i]) / frame->linesize[i];
            pic->p[i] = (plane_t){
                .p_pixels = data + (frame->data[i] - frame->data[0]),
                .i_lines = lines,
                .i_pitch = av_rpi_is_sand_frame(frame) ? av_rpi_sand_frame_stride2(frame) : frame->linesize[i],
                .i_pixel_pitch = pb[i],
                .i_visible_lines = av_frame_cropped_height(frame) >> hs[i],
                .i_visible_pitch = av_frame_cropped_width(frame) >> ws[i]
            };
        }
    }
    return 0;
}


//============================================================================
//
// Nicked from avcodec/fourcc.c
//
// * Really we should probably use that directly

/*
 * Video Codecs
 */

struct vlc_avcodec_fourcc
{
    vlc_fourcc_t i_fourcc;
    unsigned i_codec;
};


static const struct vlc_avcodec_fourcc video_codecs[] =
{
    { VLC_CODEC_MP1V, AV_CODEC_ID_MPEG1VIDEO },
    { VLC_CODEC_MP2V, AV_CODEC_ID_MPEG2VIDEO }, /* prefer MPEG2 over MPEG1 */
    { VLC_CODEC_MPGV, AV_CODEC_ID_MPEG2VIDEO }, /* prefer MPEG2 over MPEG1 */
    /* AV_CODEC_ID_MPEG2VIDEO_XVMC */
    { VLC_CODEC_H261, AV_CODEC_ID_H261 },
    { VLC_CODEC_H263, AV_CODEC_ID_H263 },
    { VLC_CODEC_RV10, AV_CODEC_ID_RV10 },
    { VLC_CODEC_RV13, AV_CODEC_ID_RV10 },
    { VLC_CODEC_RV20, AV_CODEC_ID_RV20 },
    { VLC_CODEC_MJPG, AV_CODEC_ID_MJPEG },
    { VLC_CODEC_MJPGB, AV_CODEC_ID_MJPEGB },
    { VLC_CODEC_LJPG, AV_CODEC_ID_LJPEG },
    { VLC_CODEC_SP5X, AV_CODEC_ID_SP5X },
    { VLC_CODEC_JPEGLS, AV_CODEC_ID_JPEGLS },
    { VLC_CODEC_MP4V, AV_CODEC_ID_MPEG4 },
    /* AV_CODEC_ID_RAWVIDEO */
    { VLC_CODEC_DIV1, AV_CODEC_ID_MSMPEG4V1 },
    { VLC_CODEC_DIV2, AV_CODEC_ID_MSMPEG4V2 },
    { VLC_CODEC_DIV3, AV_CODEC_ID_MSMPEG4V3 },
    { VLC_CODEC_WMV1, AV_CODEC_ID_WMV1 },
    { VLC_CODEC_WMV2, AV_CODEC_ID_WMV2 },
    { VLC_CODEC_H263P, AV_CODEC_ID_H263P },
    { VLC_CODEC_H263I, AV_CODEC_ID_H263I },
    { VLC_CODEC_FLV1, AV_CODEC_ID_FLV1 },
    { VLC_CODEC_SVQ1, AV_CODEC_ID_SVQ1 },
    { VLC_CODEC_SVQ3, AV_CODEC_ID_SVQ3 },
    { VLC_CODEC_DV, AV_CODEC_ID_DVVIDEO },
    { VLC_CODEC_HUFFYUV, AV_CODEC_ID_HUFFYUV },
    { VLC_CODEC_CYUV, AV_CODEC_ID_CYUV },
    { VLC_CODEC_H264, AV_CODEC_ID_H264 },
    { VLC_CODEC_INDEO3, AV_CODEC_ID_INDEO3 },
    { VLC_CODEC_VP3, AV_CODEC_ID_VP3 },
    { VLC_CODEC_THEORA, AV_CODEC_ID_THEORA },
#if ( !defined( WORDS_BIGENDIAN ) )
    /* Asus Video (Another thing that doesn't work on PPC) */
    { VLC_CODEC_ASV1, AV_CODEC_ID_ASV1 },
    { VLC_CODEC_ASV2, AV_CODEC_ID_ASV2 },
#endif
    { VLC_CODEC_FFV1, AV_CODEC_ID_FFV1 },
    { VLC_CODEC_4XM, AV_CODEC_ID_4XM },
    { VLC_CODEC_VCR1, AV_CODEC_ID_VCR1 },
    { VLC_CODEC_CLJR, AV_CODEC_ID_CLJR },
    { VLC_CODEC_MDEC, AV_CODEC_ID_MDEC },
    { VLC_CODEC_ROQ, AV_CODEC_ID_ROQ },
    { VLC_CODEC_INTERPLAY, AV_CODEC_ID_INTERPLAY_VIDEO },
    { VLC_CODEC_XAN_WC3, AV_CODEC_ID_XAN_WC3 },
    { VLC_CODEC_XAN_WC4, AV_CODEC_ID_XAN_WC4 },
    { VLC_CODEC_RPZA, AV_CODEC_ID_RPZA },
    { VLC_CODEC_CINEPAK, AV_CODEC_ID_CINEPAK },
    { VLC_CODEC_WS_VQA, AV_CODEC_ID_WS_VQA },
    { VLC_CODEC_MSRLE, AV_CODEC_ID_MSRLE },
    { VLC_CODEC_MSVIDEO1, AV_CODEC_ID_MSVIDEO1 },
    { VLC_CODEC_IDCIN, AV_CODEC_ID_IDCIN },
    { VLC_CODEC_8BPS, AV_CODEC_ID_8BPS },
    { VLC_CODEC_SMC, AV_CODEC_ID_SMC },
    { VLC_CODEC_FLIC, AV_CODEC_ID_FLIC },
    { VLC_CODEC_TRUEMOTION1, AV_CODEC_ID_TRUEMOTION1 },
    { VLC_CODEC_VMDVIDEO, AV_CODEC_ID_VMDVIDEO },
    { VLC_CODEC_LCL_MSZH, AV_CODEC_ID_MSZH },
    { VLC_CODEC_LCL_ZLIB, AV_CODEC_ID_ZLIB },
    { VLC_CODEC_QTRLE, AV_CODEC_ID_QTRLE },
    { VLC_CODEC_TSCC, AV_CODEC_ID_TSCC },
    { VLC_CODEC_ULTI, AV_CODEC_ID_ULTI },
    { VLC_CODEC_QDRAW, AV_CODEC_ID_QDRAW },
    { VLC_CODEC_VIXL, AV_CODEC_ID_VIXL },
    { VLC_CODEC_QPEG, AV_CODEC_ID_QPEG },
    { VLC_CODEC_PNG, AV_CODEC_ID_PNG },
    { VLC_CODEC_PPM, AV_CODEC_ID_PPM },
    /* AV_CODEC_ID_PBM */
    { VLC_CODEC_PGM, AV_CODEC_ID_PGM },
    { VLC_CODEC_PGMYUV, AV_CODEC_ID_PGMYUV },
    { VLC_CODEC_PAM, AV_CODEC_ID_PAM },
    { VLC_CODEC_FFVHUFF, AV_CODEC_ID_FFVHUFF },
    { VLC_CODEC_RV30, AV_CODEC_ID_RV30 },
    { VLC_CODEC_RV40, AV_CODEC_ID_RV40 },
    { VLC_CODEC_VC1,  AV_CODEC_ID_VC1 },
    { VLC_CODEC_WMVA, AV_CODEC_ID_VC1 },
    { VLC_CODEC_WMV3, AV_CODEC_ID_WMV3 },
    { VLC_CODEC_WMVP, AV_CODEC_ID_WMV3 },
    { VLC_CODEC_LOCO, AV_CODEC_ID_LOCO },
    { VLC_CODEC_WNV1, AV_CODEC_ID_WNV1 },
    { VLC_CODEC_AASC, AV_CODEC_ID_AASC },
    { VLC_CODEC_INDEO2, AV_CODEC_ID_INDEO2 },
    { VLC_CODEC_FRAPS, AV_CODEC_ID_FRAPS },
    { VLC_CODEC_TRUEMOTION2, AV_CODEC_ID_TRUEMOTION2 },
    { VLC_CODEC_BMP, AV_CODEC_ID_BMP },
    { VLC_CODEC_CSCD, AV_CODEC_ID_CSCD },
    { VLC_CODEC_MMVIDEO, AV_CODEC_ID_MMVIDEO },
    { VLC_CODEC_ZMBV, AV_CODEC_ID_ZMBV },
    { VLC_CODEC_AVS, AV_CODEC_ID_AVS },
    { VLC_CODEC_SMACKVIDEO, AV_CODEC_ID_SMACKVIDEO },
    { VLC_CODEC_NUV, AV_CODEC_ID_NUV },
    { VLC_CODEC_KMVC, AV_CODEC_ID_KMVC },
    { VLC_CODEC_FLASHSV, AV_CODEC_ID_FLASHSV },
    { VLC_CODEC_CAVS, AV_CODEC_ID_CAVS },
    { VLC_CODEC_JPEG2000, AV_CODEC_ID_JPEG2000 },
    { VLC_CODEC_VMNC, AV_CODEC_ID_VMNC },
    { VLC_CODEC_VP5, AV_CODEC_ID_VP5 },
    { VLC_CODEC_VP6, AV_CODEC_ID_VP6 },
    { VLC_CODEC_VP6F, AV_CODEC_ID_VP6F },
    { VLC_CODEC_TARGA, AV_CODEC_ID_TARGA },
    { VLC_CODEC_DSICINVIDEO, AV_CODEC_ID_DSICINVIDEO },
    { VLC_CODEC_TIERTEXSEQVIDEO, AV_CODEC_ID_TIERTEXSEQVIDEO },
    { VLC_CODEC_TIFF, AV_CODEC_ID_TIFF },
    { VLC_CODEC_GIF, AV_CODEC_ID_GIF },
    { VLC_CODEC_DXA, AV_CODEC_ID_DXA },
    { VLC_CODEC_DNXHD, AV_CODEC_ID_DNXHD },
    { VLC_CODEC_THP, AV_CODEC_ID_THP },
    { VLC_CODEC_SGI, AV_CODEC_ID_SGI },
    { VLC_CODEC_C93, AV_CODEC_ID_C93 },
    { VLC_CODEC_BETHSOFTVID, AV_CODEC_ID_BETHSOFTVID },
    /* AV_CODEC_ID_PTX */
    { VLC_CODEC_TXD, AV_CODEC_ID_TXD },
    { VLC_CODEC_VP6A, AV_CODEC_ID_VP6A },
    { VLC_CODEC_AMV, AV_CODEC_ID_AMV },
    { VLC_CODEC_VB, AV_CODEC_ID_VB },
    { VLC_CODEC_PCX, AV_CODEC_ID_PCX },
    /* AV_CODEC_ID_SUNRAST */
    { VLC_CODEC_INDEO4, AV_CODEC_ID_INDEO4 },
    { VLC_CODEC_INDEO5, AV_CODEC_ID_INDEO5 },
    { VLC_CODEC_MIMIC, AV_CODEC_ID_MIMIC },
    { VLC_CODEC_RL2, AV_CODEC_ID_RL2 },
    { VLC_CODEC_ESCAPE124, AV_CODEC_ID_ESCAPE124 },
    { VLC_CODEC_DIRAC, AV_CODEC_ID_DIRAC },
    { VLC_CODEC_BFI, AV_CODEC_ID_BFI },
    { VLC_CODEC_CMV, AV_CODEC_ID_CMV },
    { VLC_CODEC_MOTIONPIXELS, AV_CODEC_ID_MOTIONPIXELS },
    { VLC_CODEC_TGV, AV_CODEC_ID_TGV },
    { VLC_CODEC_TGQ, AV_CODEC_ID_TGQ },
    { VLC_CODEC_TQI, AV_CODEC_ID_TQI },
    { VLC_CODEC_AURA, AV_CODEC_ID_AURA },
    /* AV_CODEC_ID_AURA2 */
    /* AV_CODEC_ID_V210X */
    { VLC_CODEC_TMV, AV_CODEC_ID_TMV },
    { VLC_CODEC_V210, AV_CODEC_ID_V210 },
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT( 54, 50, 100 ) && LIBAVCODEC_VERSION_MICRO >= 100
    { VLC_CODEC_VUYA, AV_CODEC_ID_AYUV },
#endif
    /* AV_CODEC_ID_DPX */
    { VLC_CODEC_MAD, AV_CODEC_ID_MAD },
    { VLC_CODEC_FRWU, AV_CODEC_ID_FRWU },
    { VLC_CODEC_FLASHSV2, AV_CODEC_ID_FLASHSV2 },
    /* AV_CODEC_ID_CDGRAPHICS */
    /* AV_CODEC_ID_R210 */
    { VLC_CODEC_ANM, AV_CODEC_ID_ANM },
    { VLC_CODEC_BINKVIDEO, AV_CODEC_ID_BINKVIDEO },
    /* AV_CODEC_ID_IFF_ILBM */
    /* AV_CODEC_ID_IFF_BYTERUN1 */
    { VLC_CODEC_KGV1, AV_CODEC_ID_KGV1 },
    { VLC_CODEC_YOP, AV_CODEC_ID_YOP },
    { VLC_CODEC_VP8, AV_CODEC_ID_VP8 },
    /* AV_CODEC_ID_PICTOR */
    /* AV_CODEC_ID_ANSI */
    /* AV_CODEC_ID_A64_MULTI */
    /* AV_CODEC_ID_A64_MULTI5 */
    /* AV_CODEC_ID_R10K */
    { VLC_CODEC_MXPEG, AV_CODEC_ID_MXPEG },
    { VLC_CODEC_LAGARITH, AV_CODEC_ID_LAGARITH },
    { VLC_CODEC_PRORES, AV_CODEC_ID_PRORES },
    { VLC_CODEC_JV, AV_CODEC_ID_JV },
    { VLC_CODEC_DFA, AV_CODEC_ID_DFA },
    { VLC_CODEC_WMVP, AV_CODEC_ID_WMV3IMAGE },
    { VLC_CODEC_WMVP2, AV_CODEC_ID_VC1IMAGE },
    { VLC_CODEC_UTVIDEO, AV_CODEC_ID_UTVIDEO },
    { VLC_CODEC_BMVVIDEO, AV_CODEC_ID_BMV_VIDEO },
    { VLC_CODEC_VBLE, AV_CODEC_ID_VBLE },
    { VLC_CODEC_DXTORY, AV_CODEC_ID_DXTORY },
    /* AV_CODEC_ID_V410 */
    /* AV_CODEC_ID_XWD */
    { VLC_CODEC_CDXL, AV_CODEC_ID_CDXL },
    /* AV_CODEC_ID_XBM */
    /* AV_CODEC_ID_ZEROCODEC */
    { VLC_CODEC_MSS1, AV_CODEC_ID_MSS1 },
    { VLC_CODEC_MSA1, AV_CODEC_ID_MSA1 },
    { VLC_CODEC_TSC2, AV_CODEC_ID_TSCC2 },
    { VLC_CODEC_MTS2, AV_CODEC_ID_MTS2 },
    { VLC_CODEC_CLLC, AV_CODEC_ID_CLLC },
    { VLC_CODEC_MSS2, AV_CODEC_ID_MSS2 },
    { VLC_CODEC_VP9, AV_CODEC_ID_VP9 },
#if LIBAVCODEC_VERSION_CHECK( 57, 26, 0, 83, 101 )
    { VLC_CODEC_AV1, AV_CODEC_ID_AV1 },
#endif
    { VLC_CODEC_ICOD, AV_CODEC_ID_AIC },
    /* AV_CODEC_ID_ESCAPE130 */
    { VLC_CODEC_G2M4, AV_CODEC_ID_G2M },
    { VLC_CODEC_G2M2, AV_CODEC_ID_G2M },
    { VLC_CODEC_G2M3, AV_CODEC_ID_G2M },
    /* AV_CODEC_ID_WEBP */
    { VLC_CODEC_HNM4_VIDEO, AV_CODEC_ID_HNM4_VIDEO },
    { VLC_CODEC_HEVC, AV_CODEC_ID_HEVC },

    { VLC_CODEC_FIC , AV_CODEC_ID_FIC },
    /* AV_CODEC_ID_ALIAS_PIX */
    /* AV_CODEC_ID_BRENDER_PIX */
    /* AV_CODEC_ID_PAF_VIDEO */
    /* AV_CODEC_ID_EXR */

    { VLC_CODEC_VP7 , AV_CODEC_ID_VP7 },
    /* AV_CODEC_ID_SANM */
    /* AV_CODEC_ID_SGIRLE */
    /* AV_CODEC_ID_MVC1 */
    /* AV_CODEC_ID_MVC2 */
    { VLC_CODEC_HQX, AV_CODEC_ID_HQX },

    { VLC_CODEC_TDSC, AV_CODEC_ID_TDSC },

    { VLC_CODEC_HQ_HQA, AV_CODEC_ID_HQ_HQA },

    { VLC_CODEC_HAP, AV_CODEC_ID_HAP },
    /* AV_CODEC_ID_DDS */

    { VLC_CODEC_DXV, AV_CODEC_ID_DXV },

    /* ffmpeg only: AV_CODEC_ID_BRENDER_PIX */
    /* ffmpeg only: AV_CODEC_ID_Y41P */
    /* ffmpeg only: AV_CODEC_ID_EXR */
    /* ffmpeg only: AV_CODEC_ID_AVRP */
    /* ffmpeg only: AV_CODEC_ID_012V */
    /* ffmpeg only: AV_CODEC_ID_AVUI */
    /* ffmpeg only: AV_CODEC_ID_TARGA_Y216 */
    /* ffmpeg only: AV_CODEC_ID_V308 */
    /* ffmpeg only: AV_CODEC_ID_V408 */
    /* ffmpeg only: AV_CODEC_ID_YUV4 */
    /* ffmpeg only: AV_CODEC_ID_SANM */
    /* ffmpeg only: AV_CODEC_ID_PAF_VIDEO */
    /* ffmpeg only: AV_CODEC_ID_AVRN */
    /* ffmpeg only: AV_CODEC_ID_CPIA */
    /* ffmpeg only: AV_CODEC_ID_XFACE */
    /* ffmpeg only: AV_CODEC_ID_SGIRLE */
    /* ffmpeg only: AV_CODEC_ID_MVC1 */
    /* ffmpeg only: AV_CODEC_ID_MVC2 */
    /* ffmpeg only: AV_CODEC_ID_SNOW */
    /* ffmpeg only: AV_CODEC_ID_SMVJPEG */

#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 24, 102 )
    { VLC_CODEC_CINEFORM, AV_CODEC_ID_CFHD },
#endif

#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 70, 100 )
    { VLC_CODEC_PIXLET, AV_CODEC_ID_PIXLET },
#endif

#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 71, 101 )
    { VLC_CODEC_SPEEDHQ, AV_CODEC_ID_SPEEDHQ },
#endif

#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 79, 100 )
    { VLC_CODEC_FMVC, AV_CODEC_ID_FMVC },
#endif
};

// *** Really we should probably use GetFfmpegCodec with a pre-kludge for the bits we care about
static bool
ZcGetFfmpegCodec( enum es_format_category_e cat, vlc_fourcc_t i_fourcc,
                     unsigned *pi_ffmpeg_codec, const char **ppsz_name )
{
    const struct vlc_avcodec_fourcc *base;
    size_t count;

    base = video_codecs;
    count = ARRAY_SIZE(video_codecs);
    i_fourcc = vlc_fourcc_GetCodec( cat, i_fourcc );

    for( size_t i = 0; i < count; i++ )
    {
        if( base[i].i_fourcc == i_fourcc )
        {
            if( pi_ffmpeg_codec != NULL )
                *pi_ffmpeg_codec = base[i].i_codec;
            if( ppsz_name )
                *ppsz_name = vlc_fourcc_GetDescription( cat, i_fourcc );
            return true;
        }
    }
    return false;
}



//============================================================================
// Derived from codec/avcodec/avcodec.c

static AVCodecContext *
ZcFfmpeg_AllocContext( decoder_t *p_dec,
                                     const AVCodec **restrict codecp )
{
    unsigned i_codec_id;
    const char *psz_namecodec;
    const AVCodec *p_codec = NULL;

    /* *** determine codec type *** */
    if( !ZcGetFfmpegCodec( p_dec->fmt_in.i_cat, p_dec->fmt_in.i_codec,
                         &i_codec_id, &psz_namecodec ) )
         return NULL;

    msg_Dbg( p_dec, "using %s %s", AVPROVIDER(LIBAVCODEC), LIBAVCODEC_IDENT );

    /* Initialization must be done before avcodec_find_decoder() */
    vlc_init_avcodec(VLC_OBJECT(p_dec));

    /* *** ask ffmpeg for a decoder *** */
    char *psz_decoder = var_InheritString( p_dec, "avcodec-codec" );
    if( psz_decoder != NULL )
    {
        p_codec = avcodec_find_decoder_by_name( psz_decoder );
        if( !p_codec )
            msg_Err( p_dec, "Decoder `%s' not found", psz_decoder );
        else if( p_codec->id != i_codec_id )
        {
            msg_Err( p_dec, "Decoder `%s' can't handle %4.4s",
                    psz_decoder, (char*)&p_dec->fmt_in.i_codec );
            p_codec = NULL;
        }
        free( psz_decoder );
    }
    if( !p_codec )
//        p_codec = avcodec_find_decoder( i_codec_id );
    {
        if( p_dec->fmt_in.i_codec != VLC_CODEC_HEVC )
            p_codec = avcodec_find_decoder(i_codec_id);
        else
        {
            psz_namecodec = rpi_use_pi3_hevc() ? "hevc_rpi" : "hevc" ;
            msg_Info(p_dec, "Looking for HEVC decoder '%s'", psz_namecodec);
            p_codec = avcodec_find_decoder_by_name(psz_namecodec);
        }
    }

    if( !p_codec )
    {
        msg_Dbg( p_dec, "codec not found (%s)", psz_namecodec );
        return NULL;
    }

    *codecp = p_codec;

    /* *** get a p_context *** */
    AVCodecContext *avctx = avcodec_alloc_context3(p_codec);
    if( unlikely(avctx == NULL) )
        return NULL;

    avctx->debug = var_InheritInteger( p_dec, "avcodec-debug" );
    avctx->opaque = p_dec;
    return avctx;
}

/*****************************************************************************
 * ffmpeg_OpenCodec:
 *****************************************************************************/

static int
ZcFfmpeg_OpenCodec( decoder_t *p_dec, AVCodecContext *ctx,
                      const AVCodec *codec )
{
    char *psz_opts = var_InheritString( p_dec, "avcodec-options" );
    AVDictionary *options = NULL;
    int ret;

    if (psz_opts) {
        vlc_av_get_options(psz_opts, &options);
        free(psz_opts);
    }

    if (!p_dec->p_sys->use_drm &&
        av_rpi_zc_init2(ctx, p_dec, zc_alloc_buf, zc_free_pool) != 0)
    {
        msg_Err(p_dec, "Failed to init AV ZC");
        return VLC_EGENERIC;
    }

    vlc_avcodec_lock();
    ret = avcodec_open2( ctx, codec, options ? &options : NULL );
    vlc_avcodec_unlock();

    AVDictionaryEntry *t = NULL;
    while ((t = av_dict_get(options, "", t, AV_DICT_IGNORE_SUFFIX))) {
        msg_Err( p_dec, "Unknown option \"%s\"", t->key );
    }
    av_dict_free(&options);

    if( ret < 0 )
    {
        msg_Err( p_dec, "cannot start codec (%s)", codec->name );
        return VLC_EGENERIC;
    }

    msg_Dbg( p_dec, "codec (%s) started", codec->name );
    return VLC_SUCCESS;
}

//============================================================================
// Derived from 3.0.7.1 codec/avcodec/video.c

static inline void wait_mt(decoder_sys_t *sys)
{
#if 1
    // As we only ever update the output in our main thread this lock is
    // redundant
    VLC_UNUSED(sys);
#else
    vlc_sem_wait(&sys->sem_mt);
#endif
}

static inline void post_mt(decoder_sys_t *sys)
{
#if 1
    // As we only ever update the output in our main thread this lock is
    // redundant
    VLC_UNUSED(sys);
#else
    vlc_sem_post(&sys->sem_mt);
#endif
}

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void ffmpeg_InitCodec      ( decoder_t * );
static int  DecodeVideo( decoder_t *, block_t * );
static void Flush( decoder_t * );

static uint32_t ffmpeg_CodecTag( vlc_fourcc_t fcc )
{
    uint8_t *p = (uint8_t*)&fcc;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/*****************************************************************************
 * Local Functions
 *****************************************************************************/

/**
 * Sets the decoder output format.
 */
static int lavc_GetVideoFormat(decoder_t *dec, video_format_t *restrict fmt,
                               AVCodecContext *ctx, enum AVPixelFormat pix_fmt,
                               enum AVPixelFormat sw_pix_fmt)
{
    int width = ctx->coded_width;
    int height = ctx->coded_height;

    video_format_Init(fmt, 0);

#if 1
    if ((fmt->i_chroma = ZcFindVlcChroma(pix_fmt, sw_pix_fmt)) == 0) {
        msg_Info(dec, "Find chroma fail");
        return -1;
    }
#else
    if (pix_fmt == sw_pix_fmt)
    {   /* software decoding */
        int aligns[AV_NUM_DATA_POINTERS];

        if (GetVlcChroma(fmt, pix_fmt))
            return -1;

        /* The libavcodec palette can only be fetched when the first output
         * frame is decoded. Assume that the current chroma is RGB32 while we
         * are waiting for a valid palette. Indeed, fmt_out.video.p_palette
         * doesn't trigger a new vout request, but a new chroma yes. */
        if (pix_fmt == AV_PIX_FMT_PAL8 && !dec->fmt_out.video.p_palette)
            fmt->i_chroma = VLC_CODEC_RGB32;

        avcodec_align_dimensions2(ctx, &width, &height, aligns);
    }
    else /* hardware decoding */
        fmt->i_chroma = vlc_va_GetChroma(pix_fmt, sw_pix_fmt);
#endif

    if( width == 0 || height == 0 || width > 8192 || height > 8192 ||
        width < ctx->width || height < ctx->height )
    {
        msg_Err(dec, "Invalid frame size %dx%d vsz %dx%d",
                     width, height, ctx->width, ctx->height );
        return -1; /* invalid display size */
    }

    fmt->i_width = width;
    fmt->i_height = height;
    fmt->i_visible_width = ctx->width;
    fmt->i_visible_height = ctx->height;

    /* If an aspect-ratio was specified in the input format then force it */
    if (dec->fmt_in.video.i_sar_num > 0 && dec->fmt_in.video.i_sar_den > 0)
    {
        fmt->i_sar_num = dec->fmt_in.video.i_sar_num;
        fmt->i_sar_den = dec->fmt_in.video.i_sar_den;
    }
    else
    {
        fmt->i_sar_num = ctx->sample_aspect_ratio.num;
        fmt->i_sar_den = ctx->sample_aspect_ratio.den;

        if (fmt->i_sar_num == 0 || fmt->i_sar_den == 0)
            fmt->i_sar_num = fmt->i_sar_den = 1;
    }

    if (dec->fmt_in.video.i_frame_rate > 0
     && dec->fmt_in.video.i_frame_rate_base > 0)
    {
        fmt->i_frame_rate = dec->fmt_in.video.i_frame_rate;
        fmt->i_frame_rate_base = dec->fmt_in.video.i_frame_rate_base;
    }
    else if (ctx->framerate.num > 0 && ctx->framerate.den > 0)
    {
        fmt->i_frame_rate = ctx->framerate.num;
        fmt->i_frame_rate_base = ctx->framerate.den;
# if LIBAVCODEC_VERSION_MICRO <  100
        // for some reason libav don't thinkg framerate presents actually same thing as in ffmpeg
        fmt->i_frame_rate_base *= __MAX(ctx->ticks_per_frame, 1);
# endif
    }
    else if (ctx->time_base.num > 0 && ctx->time_base.den > 0)
    {
        fmt->i_frame_rate = ctx->time_base.den;
        fmt->i_frame_rate_base = ctx->time_base.num
                                 * __MAX(ctx->ticks_per_frame, 1);
    }

    /* FIXME we should only set the known values and let the core decide
     * later of fallbacks, but we can't do that with a boolean */
    switch ( ctx->color_range )
    {
    case AVCOL_RANGE_JPEG:
        fmt->b_color_range_full = true;
        break;
    case AVCOL_RANGE_UNSPECIFIED:
        fmt->b_color_range_full = !vlc_fourcc_IsYUV( fmt->i_chroma );
        break;
    case AVCOL_RANGE_MPEG:
    default:
        fmt->b_color_range_full = false;
        break;
    }

    switch( ctx->colorspace )
    {
        case AVCOL_SPC_BT709:
            fmt->space = COLOR_SPACE_BT709;
            break;
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            fmt->space = COLOR_SPACE_BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            fmt->space = COLOR_SPACE_BT2020;
            break;
        default:
            break;
    }

    switch( ctx->color_trc )
    {
        case AVCOL_TRC_LINEAR:
            fmt->transfer = TRANSFER_FUNC_LINEAR;
            break;
        case AVCOL_TRC_GAMMA22:
            fmt->transfer = TRANSFER_FUNC_SRGB;
            break;
        case AVCOL_TRC_BT709:
            fmt->transfer = TRANSFER_FUNC_BT709;
            break;
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:
            fmt->transfer = TRANSFER_FUNC_BT2020;
            break;
#if LIBAVUTIL_VERSION_CHECK( 55, 14, 0, 31, 100)
        case AVCOL_TRC_ARIB_STD_B67:
            fmt->transfer = TRANSFER_FUNC_ARIB_B67;
            break;
#endif
#if LIBAVUTIL_VERSION_CHECK( 55, 17, 0, 37, 100)
        case AVCOL_TRC_SMPTE2084:
            fmt->transfer = TRANSFER_FUNC_SMPTE_ST2084;
            break;
        case AVCOL_TRC_SMPTE240M:
            fmt->transfer = TRANSFER_FUNC_SMPTE_240;
            break;
        case AVCOL_TRC_GAMMA28:
            fmt->transfer = TRANSFER_FUNC_BT470_BG;
            break;
#endif
        default:
            break;
    }

    switch( ctx->color_primaries )
    {
        case AVCOL_PRI_BT709:
            fmt->primaries = COLOR_PRIMARIES_BT709;
            break;
        case AVCOL_PRI_BT470BG:
            fmt->primaries = COLOR_PRIMARIES_BT601_625;
            break;
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M:
            fmt->primaries = COLOR_PRIMARIES_BT601_525;
            break;
        case AVCOL_PRI_BT2020:
            fmt->primaries = COLOR_PRIMARIES_BT2020;
            break;
        default:
            break;
    }

    switch( ctx->chroma_sample_location )
    {
        case AVCHROMA_LOC_LEFT:
            fmt->chroma_location = CHROMA_LOCATION_LEFT;
            break;
        case AVCHROMA_LOC_CENTER:
            fmt->chroma_location = CHROMA_LOCATION_CENTER;
            break;
        case AVCHROMA_LOC_TOPLEFT:
            fmt->chroma_location = CHROMA_LOCATION_TOP_LEFT;
            break;
        default:
            break;
    }

    return 0;
}

static int lavc_UpdateVideoFormat(decoder_t *dec, AVCodecContext *ctx,
                                  enum AVPixelFormat fmt,
                                  enum AVPixelFormat swfmt)
{
    video_format_t fmt_out;
    int val;
#if TRACE_ALL
    msg_Dbg(dec, "<<< %s", __func__);
#endif
    val = lavc_GetVideoFormat(dec, &fmt_out, ctx, fmt, swfmt);
    if (val)
    {
        msg_Dbg(dec, "Failed to get format");
        return val;
    }

    /* always have date in fields/ticks units */
    if(dec->p_sys->pts.i_divider_num)
        date_Change(&dec->p_sys->pts, fmt_out.i_frame_rate *
                                      __MAX(ctx->ticks_per_frame, 1),
                                      fmt_out.i_frame_rate_base);
    else
        date_Init(&dec->p_sys->pts, fmt_out.i_frame_rate *
                                    __MAX(ctx->ticks_per_frame, 1),
                                    fmt_out.i_frame_rate_base);

    fmt_out.p_palette = dec-> fmt_out.video.p_palette;
    dec->fmt_out.video.p_palette = NULL;

    es_format_Change(&dec->fmt_out, VIDEO_ES, fmt_out.i_chroma);
    dec->fmt_out.video = fmt_out;
    dec->fmt_out.video.orientation = dec->fmt_in.video.orientation;
    dec->fmt_out.video.projection_mode = dec->fmt_in.video.projection_mode;
    dec->fmt_out.video.multiview_mode = dec->fmt_in.video.multiview_mode;
    dec->fmt_out.video.pose = dec->fmt_in.video.pose;
    if ( dec->fmt_in.video.mastering.max_luminance )
        dec->fmt_out.video.mastering = dec->fmt_in.video.mastering;
    dec->fmt_out.video.lighting = dec->fmt_in.video.lighting;

    val = decoder_UpdateVideoFormat(dec);
#if TRACE_ALL
    msg_Dbg(dec, ">>> %s: rv=%d", __func__, val);
#endif
    return val;
}

static int OpenVideoCodec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
    const AVCodec *codec = p_sys->p_codec;
    int ret;

    if( ctx->extradata_size <= 0 )
    {
        if( codec->id == AV_CODEC_ID_VC1 ||
            codec->id == AV_CODEC_ID_THEORA )
        {
            msg_Warn( p_dec, "waiting for extra data for codec %s",
                      codec->name );
            return 1;
        }
    }

    ctx->width  = p_dec->fmt_in.video.i_visible_width;
    ctx->height = p_dec->fmt_in.video.i_visible_height;

    ctx->coded_width = p_dec->fmt_in.video.i_width;
    ctx->coded_height = p_dec->fmt_in.video.i_height;

    ctx->bits_per_coded_sample = p_dec->fmt_in.video.i_bits_per_pixel;
    p_sys->pix_fmt = AV_PIX_FMT_NONE;
    p_sys->profile = -1;
    p_sys->level = -1;
    cc_Init( &p_sys->cc );

    set_video_color_settings( &p_dec->fmt_in.video, ctx );
    if( p_dec->fmt_in.video.i_frame_rate_base &&
        p_dec->fmt_in.video.i_frame_rate &&
        (double) p_dec->fmt_in.video.i_frame_rate /
                 p_dec->fmt_in.video.i_frame_rate_base < 6 )
    {
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    post_mt( p_sys );
    ret = ZcFfmpeg_OpenCodec( p_dec, ctx, codec );
    wait_mt( p_sys );
    if( ret < 0 )
        return ret;

    switch( ctx->active_thread_type )
    {
        case FF_THREAD_FRAME:
            msg_Dbg( p_dec, "using frame thread mode with %d threads",
                     ctx->thread_count );
            break;
        case FF_THREAD_SLICE:
            msg_Dbg( p_dec, "using slice thread mode with %d threads",
                     ctx->thread_count );
            break;
        case 0:
            if( ctx->thread_count > 1 )
                msg_Warn( p_dec, "failed to enable threaded decoding" );
            break;
        default:
            msg_Warn( p_dec, "using unknown thread mode with %d threads",
                      ctx->thread_count );
            break;
    }
    return 0;
}

/*****************************************************************************
 * InitVideo: initialize the video decoder
 *****************************************************************************
 * the ffmpeg codec will be opened, some memory allocated. The vout is not yet
 * opened (done after the first decoded frame).
 *****************************************************************************/

static bool has_legacy_rpivid()
{
    static int cached_state = 0;

    if (cached_state == 0)
    {
        struct stat buf;
        cached_state = stat("/dev/rpivid-intcmem", &buf) != 0 ? -1 : 1;
    }
    return cached_state > 0;
}


static int MmalAvcodecOpenDecoder( vlc_object_t *obj )
{
    decoder_t *p_dec = (decoder_t *)obj;
    const AVCodec *p_codec;
    bool use_zc;
    bool use_drm;
    AVBufferRef * hw_dev_ctx = NULL;

    int extra_buffers = var_InheritInteger(p_dec, MMAL_AVCODEC_BUFFERS);

    if (extra_buffers < 0)
    {
        extra_buffers = p_dec->fmt_in.video.i_height * p_dec->fmt_in.video.i_width >= 1920 * 1088 ?
            BUFFERS_IN_FLIGHT_UHD : BUFFERS_IN_FLIGHT;
    }

    if (extra_buffers <= 0)
    {
        msg_Dbg(p_dec, "%s: extra_buffers=%d - cannot use module", __func__, extra_buffers);
        return VLC_EGENERIC;
    }

    use_zc = has_legacy_rpivid();
    use_drm = !use_zc;  // ** At least for the moment
    const vcsm_init_type_t vcsm_type = use_zc ? cma_vcsm_init() : VCSM_INIT_NONE;
    const int vcsm_size = vcsm_type == VCSM_INIT_NONE ? 0 :
        vcsm_type == VCSM_INIT_LEGACY ? hw_mmal_get_gpu_mem() : 512 << 20;

#if 1
    {
        char buf1[5], buf2[5], buf2a[5];
        char buf3[5], buf4[5];
        uint32_t in_fcc = 0;
        msg_Dbg(p_dec, "%s: <<< (%s/%s)[%s] %dx%d -> (%s/%s) %dx%d [%s/%d] xb:%d", __func__,
                str_fourcc(buf1, p_dec->fmt_in.i_codec),
                str_fourcc(buf2, p_dec->fmt_in.video.i_chroma),
                str_fourcc(buf2a, in_fcc),
                p_dec->fmt_in.video.i_width, p_dec->fmt_in.video.i_height,
                str_fourcc(buf3, p_dec->fmt_out.i_codec),
                str_fourcc(buf4, p_dec->fmt_out.video.i_chroma),
                p_dec->fmt_out.video.i_width, p_dec->fmt_out.video.i_height,
                cma_vcsm_init_str(vcsm_type), vcsm_size, extra_buffers);
    }
#endif

    if (vcsm_type == VCSM_INIT_NONE && !use_drm)
        return VLC_EGENERIC;

    if (use_drm)
    {
        int dev_type = av_hwdevice_find_type_by_name("drm");

        if (dev_type == AV_HWDEVICE_TYPE_NONE ||
            av_hwdevice_ctx_create(&hw_dev_ctx, dev_type, NULL, NULL, 0))
        {
            msg_Warn(p_dec, "Failed to create drm hw device context");
            use_drm = false;
        }
    }


#if 1
    if (use_zc &&
        ((p_dec->fmt_in.i_codec != VLC_CODEC_HEVC &&
          (vcsm_type == VCSM_INIT_CMA || vcsm_size < (96 << 20))) ||
         (p_dec->fmt_in.i_codec == VLC_CODEC_HEVC &&
          vcsm_size < (128 << 20))))
    {
        cma_vcsm_exit(vcsm_type);
        return VLC_EGENERIC;
    }
#endif

    AVCodecContext *p_context = ZcFfmpeg_AllocContext( p_dec, &p_codec );
    if( p_context == NULL )
    {
        av_buffer_unref(&hw_dev_ctx);
        cma_vcsm_exit(vcsm_type);
        return VLC_EGENERIC;
    }
    p_context->hw_device_ctx = hw_dev_ctx;
    hw_dev_ctx = NULL;

    int i_val;

    /* Allocate the memory needed to store the decoder's structure */
    decoder_sys_t *p_sys = calloc( 1, sizeof(*p_sys) );
    if( unlikely(p_sys == NULL) )
    {
        avcodec_free_context( &p_context );
        cma_vcsm_exit(vcsm_type);
        return VLC_ENOMEM;
    }

    p_dec->p_sys = p_sys;
    p_sys->p_context = p_context;
    p_sys->p_codec = p_codec;
    p_sys->p_dec = p_dec;
//    p_sys->p_va = NULL;
    p_sys->cma_in_flight_max = extra_buffers;
    p_sys->vcsm_init_type = vcsm_type;
    p_sys->use_drm = use_drm;
    vlc_sem_init( &p_sys->sem_mt, 0 );

    /* ***** Fill p_context with init values ***** */
    p_context->codec_tag = ffmpeg_CodecTag( p_dec->fmt_in.i_original_fourcc ?
                                p_dec->fmt_in.i_original_fourcc : p_dec->fmt_in.i_codec );

    /*  ***** Get configuration of ffmpeg plugin ***** */
    p_context->workaround_bugs =
        var_InheritInteger( p_dec, "avcodec-workaround-bugs" );
    p_context->err_recognition =
        var_InheritInteger( p_dec, "avcodec-error-resilience" );

    if( var_CreateGetBool( p_dec, "grayscale" ) )
        p_context->flags |= AV_CODEC_FLAG_GRAY;

    /* ***** Output always the frames ***** */
    p_context->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

    i_val = var_CreateGetInteger( p_dec, "avcodec-skiploopfilter" );
    if( i_val >= 4 ) p_context->skip_loop_filter = AVDISCARD_ALL;
    else if( i_val == 3 ) p_context->skip_loop_filter = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_context->skip_loop_filter = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_context->skip_loop_filter = AVDISCARD_NONREF;
    else p_context->skip_loop_filter = AVDISCARD_DEFAULT;

    if( var_CreateGetBool( p_dec, "avcodec-fast" ) )
        p_context->flags2 |= AV_CODEC_FLAG2_FAST;

    /* ***** libavcodec frame skipping ***** */
    p_sys->b_hurry_up = var_CreateGetBool( p_dec, "avcodec-hurry-up" );
    p_sys->b_show_corrupted = var_CreateGetBool( p_dec, "avcodec-corrupted" );

    i_val = var_CreateGetInteger( p_dec, "avcodec-skip-frame" );
    if( i_val >= 4 ) p_sys->i_skip_frame = AVDISCARD_ALL;
    else if( i_val == 3 ) p_sys->i_skip_frame = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_sys->i_skip_frame = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_sys->i_skip_frame = AVDISCARD_NONREF;
    else if( i_val == -1 ) p_sys->i_skip_frame = AVDISCARD_NONE;
    else p_sys->i_skip_frame = AVDISCARD_DEFAULT;
    p_context->skip_frame = p_sys->i_skip_frame;

    i_val = var_CreateGetInteger( p_dec, "avcodec-skip-idct" );
    if( i_val >= 4 ) p_context->skip_idct = AVDISCARD_ALL;
    else if( i_val == 3 ) p_context->skip_idct = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_context->skip_idct = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_context->skip_idct = AVDISCARD_NONREF;
    else if( i_val == -1 ) p_context->skip_idct = AVDISCARD_NONE;
    else p_context->skip_idct = AVDISCARD_DEFAULT;

    /* ***** libavcodec direct rendering ***** */
    p_sys->b_direct_rendering = false;
    atomic_init(&p_sys->b_dr_failure, false);
    if( var_CreateGetBool( p_dec, "avcodec-dr" ) &&
       (p_codec->capabilities & AV_CODEC_CAP_DR1) &&
        /* No idea why ... but this fixes flickering on some TSCC streams */
        p_sys->p_codec->id != AV_CODEC_ID_TSCC &&
        p_sys->p_codec->id != AV_CODEC_ID_CSCD &&
        p_sys->p_codec->id != AV_CODEC_ID_CINEPAK )
    {
        /* Some codecs set pix_fmt only after the 1st frame has been decoded,
         * so we need to do another check in ffmpeg_GetFrameBuf() */
        p_sys->b_direct_rendering = true;
    }

    p_context->get_format = ZcGetFormat;
#if 0
    p_context->get_format = ffmpeg_GetFormat;
    /* Always use our get_buffer wrapper so we can calculate the
     * PTS correctly */
    p_context->get_buffer2 = lavc_GetFrame;
    p_context->opaque = p_dec;
#endif

    int i_thread_count = var_InheritInteger( p_dec, "avcodec-threads" );
    if( i_thread_count <= 0 )
#if 1
    {
        // Pick 5 threads for everything on Pi except for HEVC where the h/w
        // really limits the useful size to 3
        i_thread_count = p_codec->id == AV_CODEC_ID_HEVC ? 3 : 5;
    }
#else
    {
        i_thread_count = vlc_GetCPUCount();
        if( i_thread_count > 1 )
            i_thread_count++;

        //FIXME: take in count the decoding time
#if VLC_WINSTORE_APP
        i_thread_count = __MIN( i_thread_count, 6 );
#else
        i_thread_count = __MIN( i_thread_count, p_codec->id == AV_CODEC_ID_HEVC ? 10 : 6 );
#endif
    }
    i_thread_count = __MIN( i_thread_count, p_codec->id == AV_CODEC_ID_HEVC ? 32 : 16 );
#endif
    msg_Dbg( p_dec, "allowing %d thread(s) for decoding", i_thread_count );
    p_context->thread_count = i_thread_count;
    p_context->thread_safe_callbacks = true;

    switch( p_codec->id )
    {
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_H263:
            p_context->thread_type = 0;
            break;
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
            p_context->thread_type &= ~FF_THREAD_SLICE;
            /* fall through */
# if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 1, 0))
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_VC1:
        case AV_CODEC_ID_WMV3:
            p_context->thread_type &= ~FF_THREAD_FRAME;
# endif
        default:
            break;
    }

    if( p_context->thread_type & FF_THREAD_FRAME )
        p_dec->i_extra_picture_buffers = 2 * p_context->thread_count;

    /* ***** misc init ***** */
    date_Init(&p_sys->pts, 1, 30001);
    date_Set(&p_sys->pts, VLC_TS_INVALID);
    p_sys->b_first_frame = true;
    p_sys->i_late_frames = 0;
    p_sys->b_from_preroll = false;

    /* Set output properties */
    if (ZcGetVlcChroma(&p_dec->fmt_out.video, p_context->pix_fmt, p_context->sw_pix_fmt) != VLC_SUCCESS)
    {
        /* we are doomed. but not really, because most codecs set their pix_fmt later on */
//        p_dec->fmt_out.i_codec = VLC_CODEC_I420;
        p_dec->fmt_out.i_codec = VLC_CODEC_MMAL_ZC_I420;
    }
    p_dec->fmt_out.i_codec = p_dec->fmt_out.video.i_chroma;

    p_dec->fmt_out.video.orientation = p_dec->fmt_in.video.orientation;

    if( p_dec->fmt_in.video.p_palette ) {
        p_sys->palette_sent = false;
        p_dec->fmt_out.video.p_palette = malloc( sizeof(video_palette_t) );
        if( p_dec->fmt_out.video.p_palette )
            *p_dec->fmt_out.video.p_palette = *p_dec->fmt_in.video.p_palette;
    } else
        p_sys->palette_sent = true;

    if (use_drm)
        p_sys->cma_pool = cma_drmprime_pool_new(p_sys->cma_in_flight_max, p_sys->cma_in_flight_max, false, "drm_avcodec");
    else if (use_zc)
        p_sys->cma_pool = cma_buf_pool_new(p_sys->cma_in_flight_max, p_sys->cma_in_flight_max, false, "mmal_avcodec");

    if (p_sys->cma_pool == NULL)
    {
        msg_Err(p_dec, "CMA pool alloc failure");
        goto fail;
    }

    /* ***** init this codec with special data ***** */
    ffmpeg_InitCodec( p_dec );

    /* ***** Open the codec ***** */
    if( OpenVideoCodec( p_dec ) < 0 )
    {
        vlc_sem_destroy( &p_sys->sem_mt );
        free( p_sys );
        avcodec_free_context( &p_context );
        return VLC_EGENERIC;
    }

    p_dec->pf_decode = DecodeVideo;
    p_dec->pf_flush  = Flush;

    /* XXX: Writing input format makes little sense. */
    if( p_context->profile != FF_PROFILE_UNKNOWN )
        p_dec->fmt_in.i_profile = p_context->profile;
    if( p_context->level != FF_LEVEL_UNKNOWN )
        p_dec->fmt_in.i_level = p_context->level;

#if 1
    // Most of the time we have nothing useful by way of a format here
    // wait till we've decoded something
#else
    // Update output format
    if (lavc_UpdateVideoFormat(p_dec, p_context, p_context->pix_fmt,
                               p_context->pix_fmt) != 0)
    {
        msg_Err(p_dec, "Unable to update format: pix_fmt=%d", p_context->pix_fmt);
//        goto fail;
    }
#endif

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s: OK", __func__);
#endif
    return VLC_SUCCESS;

fail:
    MmalAvcodecCloseDecoder(VLC_OBJECT(p_dec));

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s: FAIL", __func__);
#endif

    return VLC_EGENERIC;
}

/*****************************************************************************
 * Flush:
 *****************************************************************************/
static void Flush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s", __func__);
#endif

    date_Set(&p_sys->pts, VLC_TS_INVALID); /* To make sure we recover properly */
    p_sys->i_late_frames = 0;
    cc_Flush( &p_sys->cc );

    /* Abort pictures in order to unblock all avcodec workers threads waiting
     * for a picture. This will avoid a deadlock between avcodec_flush_buffers
     * and workers threads */
// It would probably be good to use AbortPicture but that often deadlocks on close
// and given that we wait for pics in the main thread it should be unneeded (whereas
// cma is alloced in the depths of ffmpeg on its own threads)
//    decoder_AbortPictures( p_dec, true );
    cma_buf_pool_cancel(p_sys->cma_pool);

    post_mt( p_sys );
    /* do not flush buffers if codec hasn't been opened (theora/vorbis/VC1) */
    if( avcodec_is_open( p_context ) )
        avcodec_flush_buffers( p_context );
    wait_mt( p_sys );

    /* Reset cancel state to false */
    cma_buf_pool_uncancel(p_sys->cma_pool);
//    decoder_AbortPictures( p_dec, false );

#if TRACE_ALL
    msg_Dbg(p_dec, ">>> %s", __func__);
#endif

}

static bool check_block_validity( decoder_sys_t *p_sys, block_t *block )
{
    if( !block)
        return true;

    if( block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        date_Set( &p_sys->pts, VLC_TS_INVALID ); /* To make sure we recover properly */
        cc_Flush( &p_sys->cc );

        p_sys->i_late_frames = 0;
        if( block->i_flags & BLOCK_FLAG_CORRUPTED )
        {
            block_Release( block );
            return false;
        }
    }
    return true;
}

static bool check_block_being_late( decoder_sys_t *p_sys, block_t *block, mtime_t current_time)
{
    if( !block )
        return false;
    if( block->i_flags & BLOCK_FLAG_PREROLL )
    {
        /* Do not care about late frames when prerolling
         * TODO avoid decoding of non reference frame
         * (ie all B except for H264 where it depends only on nal_ref_idc) */
        p_sys->i_late_frames = 0;
        p_sys->b_from_preroll = true;
        p_sys->i_last_late_delay = INT64_MAX;
    }

    if( p_sys->i_late_frames <= 0 )
        return false;

    if( current_time - p_sys->i_late_frames_start > (5*CLOCK_FREQ))
    {
        date_Set( &p_sys->pts, VLC_TS_INVALID ); /* To make sure we recover properly */
        block_Release( block );
        p_sys->i_late_frames--;
        return true;
    }
    return false;
}

static bool check_frame_should_be_dropped( decoder_sys_t *p_sys, AVCodecContext *p_context, bool *b_need_output_picture )
{
    if( p_sys->i_late_frames <= 4)
        return false;

    *b_need_output_picture = false;
    if( p_sys->i_late_frames < 12 )
    {
        p_context->skip_frame =
                (p_sys->i_skip_frame <= AVDISCARD_NONREF) ?
                AVDISCARD_NONREF : p_sys->i_skip_frame;
    }
    else
    {
        /* picture too late, won't decode
         * but break picture until a new I, and for mpeg4 ...*/
        p_sys->i_late_frames--; /* needed else it will never be decrease */
        return true;
    }
    return false;
}

static mtime_t interpolate_next_pts( decoder_t *p_dec, AVFrame *frame )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

    if( date_Get( &p_sys->pts ) == VLC_TS_INVALID ||
        p_sys->pts.i_divider_num == 0 )
        return VLC_TS_INVALID;

    int i_tick = p_context->ticks_per_frame;
    if( i_tick <= 0 )
        i_tick = 1;

    /* interpolate the next PTS */
    return date_Increment( &p_sys->pts, i_tick + frame->repeat_pict );
}

static void update_late_frame_count( decoder_t *p_dec, block_t *p_block,
                                     mtime_t current_time, mtime_t i_pts,
                                     mtime_t i_next_pts )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
   /* Update frame late count (except when doing preroll) */
   mtime_t i_display_date = VLC_TS_INVALID;
   if( !p_block || !(p_block->i_flags & BLOCK_FLAG_PREROLL) )
       i_display_date = decoder_GetDisplayDate( p_dec, i_pts );

   mtime_t i_threshold = i_next_pts != VLC_TS_INVALID ? (i_next_pts - i_pts) / 2 : 20000;

   if( i_display_date > VLC_TS_INVALID && i_display_date + i_threshold <= current_time )
   {
       /* Out of preroll, consider only late frames on rising delay */
       if( p_sys->b_from_preroll )
       {
           if( p_sys->i_last_late_delay > current_time - i_display_date )
           {
               p_sys->i_last_late_delay = current_time - i_display_date;
               return;
           }
           p_sys->b_from_preroll = false;
       }

       p_sys->i_late_frames++;
       if( p_sys->i_late_frames == 1 )
           p_sys->i_late_frames_start = current_time;

   }
   else
   {
       p_sys->i_late_frames = 0;
   }
}


static int DecodeSidedata( decoder_t *p_dec, const AVFrame *frame, picture_t *p_pic )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    bool format_changed = false;

#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 55, 16, 101 ) )
#define FROM_AVRAT(default_factor, avrat) \
(uint64_t)(default_factor) * (avrat).num / (avrat).den
    const AVFrameSideData *metadata =
            av_frame_get_side_data( frame,
                                    AV_FRAME_DATA_MASTERING_DISPLAY_METADATA );
    if ( metadata )
    {
        const AVMasteringDisplayMetadata *hdr_meta =
                (const AVMasteringDisplayMetadata *) metadata->data;
        if ( hdr_meta->has_luminance )
        {
#define ST2086_LUMA_FACTOR 10000
            p_pic->format.mastering.max_luminance =
                    FROM_AVRAT(ST2086_LUMA_FACTOR, hdr_meta->max_luminance);
            p_pic->format.mastering.min_luminance =
                    FROM_AVRAT(ST2086_LUMA_FACTOR, hdr_meta->min_luminance);
        }
        if ( hdr_meta->has_primaries )
        {
#define ST2086_RED   2
#define ST2086_GREEN 0
#define ST2086_BLUE  1
#define LAV_RED    0
#define LAV_GREEN  1
#define LAV_BLUE   2
#define ST2086_PRIM_FACTOR 50000
            p_pic->format.mastering.primaries[ST2086_RED*2   + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_RED][0]);
            p_pic->format.mastering.primaries[ST2086_RED*2   + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_RED][1]);
            p_pic->format.mastering.primaries[ST2086_GREEN*2 + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_GREEN][0]);
            p_pic->format.mastering.primaries[ST2086_GREEN*2 + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_GREEN][1]);
            p_pic->format.mastering.primaries[ST2086_BLUE*2  + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_BLUE][0]);
            p_pic->format.mastering.primaries[ST2086_BLUE*2  + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_BLUE][1]);
            p_pic->format.mastering.white_point[0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->white_point[0]);
            p_pic->format.mastering.white_point[1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->white_point[1]);
        }

        if ( memcmp( &p_dec->fmt_out.video.mastering,
                     &p_pic->format.mastering,
                     sizeof(p_pic->format.mastering) ) )
        {
            p_dec->fmt_out.video.mastering = p_pic->format.mastering;
            format_changed = true;
        }
#undef FROM_AVRAT
    }
#endif
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 55, 60, 100 ) )
    const AVFrameSideData *metadata_lt =
            av_frame_get_side_data( frame,
                                    AV_FRAME_DATA_CONTENT_LIGHT_LEVEL );
    if ( metadata_lt )
    {
        const AVContentLightMetadata *light_meta =
                (const AVContentLightMetadata *) metadata_lt->data;
        p_pic->format.lighting.MaxCLL = light_meta->MaxCLL;
        p_pic->format.lighting.MaxFALL = light_meta->MaxFALL;
        if ( memcmp( &p_dec->fmt_out.video.lighting,
                     &p_pic->format.lighting,
                     sizeof(p_pic->format.lighting) ) )
        {
            p_dec->fmt_out.video.lighting  = p_pic->format.lighting;
            format_changed = true;
        }
    }
#endif

    if (format_changed && decoder_UpdateVideoFormat( p_dec ))
        return -1;

    const AVFrameSideData *p_avcc = av_frame_get_side_data( frame, AV_FRAME_DATA_A53_CC );
    if( p_avcc )
    {
        cc_Extract( &p_sys->cc, CC_PAYLOAD_RAW, true, p_avcc->data, p_avcc->size );
        if( p_sys->cc.b_reorder || p_sys->cc.i_data )
        {
            block_t *p_cc = block_Alloc( p_sys->cc.i_data );
            if( p_cc )
            {
                memcpy( p_cc->p_buffer, p_sys->cc.p_data, p_sys->cc.i_data );
                if( p_sys->cc.b_reorder )
                    p_cc->i_dts = p_cc->i_pts = p_pic->date;
                else
                    p_cc->i_pts = p_cc->i_dts;
                decoder_cc_desc_t desc;
                desc.i_608_channels = p_sys->cc.i_608channels;
                desc.i_708_channels = p_sys->cc.i_708channels;
                desc.i_reorder_depth = 4;
                decoder_QueueCc( p_dec, p_cc, &desc );
            }
            cc_Flush( &p_sys->cc );
        }
    }
    return 0;
}

/*****************************************************************************
 * DecodeBlock: Called to decode one or more frames
 *****************************************************************************/

static picture_t *DecodeBlock( decoder_t *p_dec, block_t **pp_block, bool *error )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;
    /* Boolean if we assume that we should get valid pic as result */
    bool b_need_output_picture = true;

    /* Boolean for END_OF_SEQUENCE */
    bool eos_spotted = false;

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s: (buf_size=%d)", __func__, pp_block == NULL || *pp_block == NULL ? 0 : (*pp_block)->i_buffer);
#endif

    block_t *p_block;
    mtime_t current_time;
    picture_t *p_pic = NULL;
    AVFrame *frame = NULL;
    cma_buf_t * cb = NULL;

    // By default we are OK
    *error = false;

    if( !p_context->extradata_size && p_dec->fmt_in.i_extra )
    {
        ffmpeg_InitCodec( p_dec );
        if( !avcodec_is_open( p_context ) )
            OpenVideoCodec( p_dec );
    }

    p_block = pp_block ? *pp_block : NULL;
    if(!p_block && !(p_sys->p_codec->capabilities & AV_CODEC_CAP_DELAY) )
        return NULL;

    if( !avcodec_is_open( p_context ) )
    {
        if( p_block )
            block_Release( p_block );
        return NULL;
    }

    if( !check_block_validity( p_sys, p_block ) )
        return NULL;

    current_time = mdate();
    if( p_dec->b_frame_drop_allowed &&  check_block_being_late( p_sys, p_block, current_time) )
    {
        msg_Err( p_dec, "more than 5 seconds of late video -> "
                 "dropping frame (computer too slow ?)" );
        return NULL;
    }


    /* A good idea could be to decode all I pictures and see for the other */

    /* Defaults that if we aren't in prerolling, we want output picture
       same for if we are flushing (p_block==NULL) */
    if( !p_block || !(p_block->i_flags & BLOCK_FLAG_PREROLL) )
        b_need_output_picture = true;
    else
        b_need_output_picture = false;

    /* Change skip_frame config only if hurry_up is enabled */
    if( p_sys->b_hurry_up )
    {
        p_context->skip_frame = p_sys->i_skip_frame;

        /* Check also if we should/can drop the block and move to next block
            as trying to catchup the speed*/
        if( p_dec->b_frame_drop_allowed &&
            check_frame_should_be_dropped( p_sys, p_context, &b_need_output_picture ) )
        {
            if( p_block )
                block_Release( p_block );
            msg_Warn( p_dec, "More than 11 late frames, dropping frame" );
            return NULL;
        }
    }
    if( !b_need_output_picture )
    {
        p_context->skip_frame = __MAX( p_context->skip_frame,
                                              AVDISCARD_NONREF );
    }

    /*
     * Do the actual decoding now */

    /* Don't forget that libavcodec requires a little more bytes
     * that the real frame size */
    if( p_block && p_block->i_buffer > 0 )
    {
        eos_spotted = ( p_block->i_flags & BLOCK_FLAG_END_OF_SEQUENCE ) != 0;

        p_block = block_Realloc( p_block, 0,
                            p_block->i_buffer + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !p_block )
            return NULL;
        p_block->i_buffer -= FF_INPUT_BUFFER_PADDING_SIZE;
        *pp_block = p_block;
        memset( p_block->p_buffer + p_block->i_buffer, 0,
                FF_INPUT_BUFFER_PADDING_SIZE );
    }

    while( !p_block || p_block->i_buffer > 0 || eos_spotted )
    {
        int i_used;
        AVPacket pkt;

        post_mt( p_sys );

        av_init_packet( &pkt );
        if( p_block && p_block->i_buffer > 0 )
        {
            pkt.data = p_block->p_buffer;
            pkt.size = p_block->i_buffer;
            pkt.pts = p_block->i_pts > VLC_TS_INVALID ? p_block->i_pts : AV_NOPTS_VALUE;
            pkt.dts = p_block->i_dts > VLC_TS_INVALID ? p_block->i_dts : AV_NOPTS_VALUE;
        }
        else
        {
            /* Return delayed frames if codec has CODEC_CAP_DELAY */
            pkt.data = NULL;
            pkt.size = 0;
        }

        if( !p_sys->palette_sent )
        {
            uint8_t *pal = av_packet_new_side_data(&pkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
            if (pal) {
                memcpy(pal, p_dec->fmt_in.video.p_palette->palette, AVPALETTE_SIZE);
                p_sys->palette_sent = true;
            }
        }

        /* Make sure we don't reuse the same timestamps twice */
        if( p_block )
        {
            p_block->i_pts =
            p_block->i_dts = VLC_TS_INVALID;
        }

        int ret = avcodec_send_packet(p_context, &pkt);
        if( ret != 0 && ret != AVERROR(EAGAIN) )
        {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
            {
                msg_Err(p_dec, "avcodec_send_packet critical error");
                *error = true;
            }
            av_packet_unref( &pkt );
            break;
        }
        i_used = ret != AVERROR(EAGAIN) ? pkt.size : 0;
        av_packet_unref( &pkt );

        frame = av_frame_alloc();
        if (unlikely(frame == NULL))
        {
            *error = true;
            break;
        }

        ret = avcodec_receive_frame(p_context, frame);
        if( ret != 0 && ret != AVERROR(EAGAIN) )
        {
            msg_Dbg(p_dec, "No receive");
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
            {
                msg_Err(p_dec, "avcodec_receive_frame critical error");
                *error = true;
            }
            av_frame_free(&frame);
            /* After draining, we need to reset decoder with a flush */
            if( ret == AVERROR_EOF )
                avcodec_flush_buffers( p_sys->p_context );
            break;
        }
        bool not_received_frame = ret;

        wait_mt( p_sys );

        if( eos_spotted )
            p_sys->b_first_frame = true;

        if( p_block )
        {
            if( p_block->i_buffer <= 0 )
                eos_spotted = false;

            /* Consumed bytes */
            p_block->p_buffer += i_used;
            p_block->i_buffer -= i_used;
        }

        /* Nothing to display */
        if( not_received_frame )
        {
//            msg_Dbg(p_dec, "No rx: used=%d", i_used);
            av_frame_free(&frame);
            if( i_used == 0 ) break;
            continue;
        }

        /* Compute the PTS */
#ifdef FF_API_PKT_PTS
        mtime_t i_pts = frame->pts;
#else
        mtime_t i_pts = frame->pkt_pts;
#endif
        if (i_pts == AV_NOPTS_VALUE )
            i_pts = frame->pkt_dts;

        if( i_pts == AV_NOPTS_VALUE )
            i_pts = date_Get( &p_sys->pts );

        /* Interpolate the next PTS */
        if( i_pts > VLC_TS_INVALID )
            date_Set( &p_sys->pts, i_pts );

        const mtime_t i_next_pts = interpolate_next_pts(p_dec, frame);

        update_late_frame_count( p_dec, p_block, current_time, i_pts, i_next_pts);

        if( !b_need_output_picture ||
//            ( !p_sys->p_va && !frame->linesize[0] ) ||
           (frame->format != AV_PIX_FMT_DRM_PRIME && !frame->linesize[0] ) ||
           ( p_dec->b_frame_drop_allowed && (frame->flags & AV_FRAME_FLAG_CORRUPT) &&
             !p_sys->b_show_corrupted ) )
        {
            av_frame_free(&frame);
//            msg_Dbg(p_dec, "Bad frame");
            continue;
        }

        if( p_context->pix_fmt == AV_PIX_FMT_PAL8
         && !p_dec->fmt_out.video.p_palette )
        {
            /* See AV_PIX_FMT_PAL8 comment in avc_GetVideoFormat(): update the
             * fmt_out palette and change the fmt_out chroma to request a new
             * vout */
            assert( p_dec->fmt_out.video.i_chroma != VLC_CODEC_RGBP );

            video_palette_t *p_palette;
            p_palette = p_dec->fmt_out.video.p_palette
                      = malloc( sizeof(video_palette_t) );
            if( !p_palette )
            {
                *error = true;
                av_frame_free(&frame);
                break;
            }
            static_assert( sizeof(p_palette->palette) == AVPALETTE_SIZE,
                           "Palette size mismatch between vlc and libavutil" );
            assert( frame->data[1] != NULL );
            memcpy( p_palette->palette, frame->data[1], AVPALETTE_SIZE );
            p_palette->i_entries = AVPALETTE_COUNT;
            p_dec->fmt_out.video.i_chroma = VLC_CODEC_RGBP;
            if( decoder_UpdateVideoFormat( p_dec ) )
            {
                av_frame_free(&frame);
                continue;
            }
        }

#if 1
        if (lavc_UpdateVideoFormat(p_dec, p_context, p_context->pix_fmt,
                                   p_context->sw_pix_fmt) != 0)
        {
            msg_Err(p_dec, "Failed to update format");
            goto fail;
        }

        if ((p_pic = decoder_NewPicture(p_dec)) == NULL)
        {
            msg_Err(p_dec, "Failed to allocate pic");
            goto fail;
        }

        if (p_sys->use_drm)
        {
            cb = cma_drmprime_pool_alloc_buf(p_sys->cma_pool, frame);
            if (cb == NULL)
            {
                msg_Err(p_dec, "Failed to alloc CMA buf from DRM_PRIME");
                goto fail;
            }
        }
        else
        {
            cb = cma_buf_ref(av_rpi_zc_buf_v(frame->buf[0]));
            if (cb == NULL)
            {
                msg_Err(p_dec, "Frame has no attached CMA buffer");
                goto fail;
            }
        }

        if (cma_buf_pic_attach(cb, p_pic) != 0)
        {
            cma_buf_unref(cb);  // Undo the in_flight
            char dbuf0[5];
            msg_Err(p_dec, "Failed to attach bufs to pic: fmt=%s", str_fourcc(dbuf0, p_pic->format.i_chroma));
            goto fail;
        }
        cb = NULL; // Now attached to pic

        // ****** Set planes etc.
        set_pic_from_frame(p_pic, frame);
#else
        picture_t *p_pic = frame->opaque;
        if( p_pic == NULL )
        {   /* When direct rendering is not used, get_format() and get_buffer()
             * might not be called. The output video format must be set here
             * then picture buffer can be allocated. */
            if (p_sys->p_va == NULL
             && lavc_UpdateVideoFormat(p_dec, p_context, p_context->pix_fmt,
                                       p_context->pix_fmt) == 0)
                p_pic = decoder_NewPicture(p_dec);

            if( !p_pic )
            {
                av_frame_free(&frame);
                break;
            }

            /* Fill picture_t from AVFrame */
            if( lavc_CopyPicture( p_dec, p_pic, frame ) != VLC_SUCCESS )
            {
                av_frame_free(&frame);
                picture_Release( p_pic );
                break;
            }
        }
        else
        {
            /* Some codecs can return the same frame multiple times. By the
             * time that the same frame is returned a second time, it will be
             * too late to clone the underlying picture. So clone proactively.
             * A single picture CANNOT be queued multiple times.
             */
            p_pic = picture_Clone( p_pic );
            if( unlikely(p_pic == NULL) )
            {
                av_frame_free(&frame);
                break;
            }
        }
#endif

        if( !p_dec->fmt_in.video.i_sar_num || !p_dec->fmt_in.video.i_sar_den )
        {
            /* Fetch again the aspect ratio in case it changed */
            p_dec->fmt_out.video.i_sar_num
                = p_context->sample_aspect_ratio.num;
            p_dec->fmt_out.video.i_sar_den
                = p_context->sample_aspect_ratio.den;

            if( !p_dec->fmt_out.video.i_sar_num || !p_dec->fmt_out.video.i_sar_den )
            {
                p_dec->fmt_out.video.i_sar_num = 1;
                p_dec->fmt_out.video.i_sar_den = 1;
            }
        }

        p_pic->date = i_pts;
        /* Hack to force display of still pictures */
        p_pic->b_force = p_sys->b_first_frame;
        p_pic->i_nb_fields = 2 + frame->repeat_pict;
        p_pic->b_progressive = !frame->interlaced_frame;
        p_pic->b_top_field_first = frame->top_field_first;

        if (DecodeSidedata(p_dec, frame, p_pic))
            i_pts = VLC_TS_INVALID;

        av_frame_free(&frame);

        /* Send decoded frame to vout */
        if (i_pts > VLC_TS_INVALID)
        {
            p_sys->b_first_frame = false;
#if TRACE_ALL
            msg_Dbg(p_dec, ">>> %s: Got pic", __func__);
#endif
            return p_pic;
        }
        else
            picture_Release( p_pic );
    }

    if( p_block )
        block_Release( p_block );

#if TRACE_ALL
     msg_Dbg(p_dec, ">>> %s: NULL", __func__);
#endif
    return NULL;

fail:
#if TRACE_ALL
     msg_Dbg(p_dec, ">>> %s: FAIL", __func__);
#endif
    av_frame_free(&frame);
    if (p_pic != NULL)
        picture_Release(p_pic);
    if (p_block != NULL)
        block_Release(p_block);
    *error = true;
    return NULL;
}

static int DecodeVideo( decoder_t *p_dec, block_t *p_block )
{
    block_t **pp_block = p_block ? &p_block : NULL;
    picture_t *p_pic;
    bool error = false;
    while( ( p_pic = DecodeBlock( p_dec, pp_block, &error ) ) != NULL )
        decoder_QueueVideo( p_dec, p_pic );
    return VLCDEC_SUCCESS;
// Easiest to just ignore all errors - returning a real error seems to
// kill output forever
//    return error ? VLCDEC_ECRITICAL : VLCDEC_SUCCESS;
}

/*****************************************************************************
 * EndVideo: decoder destruction
 *****************************************************************************
 * This function is called when the thread ends after a successful
 * initialization.
 *****************************************************************************/
static void MmalAvcodecCloseDecoder( vlc_object_t *obj )
{
    decoder_t *p_dec = (decoder_t *)obj;
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
//    void *hwaccel_context;

    msg_Dbg(obj, "<<< %s", __func__);

    post_mt( p_sys );

    cma_buf_pool_cancel(p_sys->cma_pool);  // Abort any pending frame allocs

    /* do not flush buffers if codec hasn't been opened (theora/vorbis/VC1) */
    if( avcodec_is_open( ctx ) )
        avcodec_flush_buffers( ctx );

    if (!p_sys->use_drm)
        av_rpi_zc_uninit2(ctx);

    wait_mt( p_sys );

    cc_Flush( &p_sys->cc );

//    hwaccel_context = ctx->hwaccel_context;
    avcodec_free_context( &ctx );

//    if( p_sys->p_va )
//        vlc_va_Delete( p_sys->p_va, &hwaccel_context );

    cma_vcsm_exit(p_sys->vcsm_init_type);

    vlc_sem_destroy( &p_sys->sem_mt );
    free( p_sys );
}

/*****************************************************************************
 * ffmpeg_InitCodec: setup codec extra initialization data for ffmpeg
 *****************************************************************************/
static void ffmpeg_InitCodec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    size_t i_size = p_dec->fmt_in.i_extra;

    if( !i_size ) return;

    if( p_sys->p_codec->id == AV_CODEC_ID_SVQ3 )
    {
        uint8_t *p;

        p_sys->p_context->extradata_size = i_size + 12;
        p = p_sys->p_context->extradata =
            av_malloc( p_sys->p_context->extradata_size +
                       FF_INPUT_BUFFER_PADDING_SIZE );
        if( !p )
            return;

        memcpy( &p[0],  "SVQ3", 4 );
        memset( &p[4], 0, 8 );
        memcpy( &p[12], p_dec->fmt_in.p_extra, i_size );

        /* Now remove all atoms before the SMI one */
        if( p_sys->p_context->extradata_size > 0x5a &&
            strncmp( (char*)&p[0x56], "SMI ", 4 ) )
        {
            uint8_t *psz = &p[0x52];

            while( psz < &p[p_sys->p_context->extradata_size - 8] )
            {
                uint_fast32_t atom_size = GetDWBE( psz );
                if( atom_size <= 1 )
                {
                    /* FIXME handle 1 as long size */
                    break;
                }
                if( !strncmp( (char*)&psz[4], "SMI ", 4 ) )
                {
                    memmove( &p[0x52], psz,
                             &p[p_sys->p_context->extradata_size] - psz );
                    break;
                }

                psz += atom_size;
            }
        }
    }
    else
    {
        p_sys->p_context->extradata_size = i_size;
        p_sys->p_context->extradata =
            av_malloc( i_size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( p_sys->p_context->extradata )
        {
            memcpy( p_sys->p_context->extradata,
                    p_dec->fmt_in.p_extra, i_size );
            memset( p_sys->p_context->extradata + i_size,
                    0, FF_INPUT_BUFFER_PADDING_SIZE );
        }
    }
}


vlc_module_begin()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_shortname(N_("MMAL avcodec"))
    set_description(N_("MMAL buffered avcodec "))
    set_capability("video decoder", 80)
    add_shortcut("mmal_avcodec")
    add_integer(MMAL_AVCODEC_BUFFERS, -1, MMAL_AVCODEC_BUFFERS_TEXT,
                    MMAL_AVCODEC_BUFFERS_LONGTEXT, true)
    set_callbacks(MmalAvcodecOpenDecoder, MmalAvcodecCloseDecoder)
vlc_module_end()

