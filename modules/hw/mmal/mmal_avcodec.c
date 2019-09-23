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
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_plugin.h>
#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_avcodec.h>
#include <vlc_cpu.h>
#include <vlc_atomic.h>
#include <assert.h>

#include "../../codec/avcodec/avcommon.h"

#include <interface/mmal/mmal.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rpi_sand_fns.h>
#include <libavcodec/rpi_zc.h>
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 55, 16, 101 ) )
#include <libavutil/mastering_display_metadata.h>
#endif

#include "mmal_cma.h"
#include "mmal_picture.h"

#define TRACE_ALL 0

#define AVPROVIDER(lib) ((lib##_VERSION_MICRO < 100) ? "libav" : "ffmpeg")

#ifdef HAVE_LIBAVCODEC_AVCODEC_H
#include <libavcodec/avcodec.h>

/* LIBAVCODEC_VERSION_CHECK checks for the right version of libav and FFmpeg
 * a is the major version
 * b and c the minor and micro versions of libav
 * d and e the minor and micro versions of FFmpeg */
#define LIBAVCODEC_VERSION_CHECK( a, b, c, d, e ) \
    ( (LIBAVCODEC_VERSION_MICRO <  100 && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT( a, b, c ) ) || \
      (LIBAVCODEC_VERSION_MICRO >= 100 && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT( a, d, e ) ) )

#ifndef AV_CODEC_FLAG_OUTPUT_CORRUPT
# define AV_CODEC_FLAG_OUTPUT_CORRUPT CODEC_FLAG_OUTPUT_CORRUPT
#endif
#ifndef AV_CODEC_FLAG_GRAY
# define AV_CODEC_FLAG_GRAY CODEC_FLAG_GRAY
#endif
#ifndef AV_CODEC_FLAG_DR1
# define AV_CODEC_FLAG_DR1 CODEC_FLAG_DR1
#endif
#ifndef AV_CODEC_FLAG_DELAY
# define AV_CODEC_FLAG_DELAY CODEC_FLAG_DELAY
#endif
#ifndef AV_CODEC_FLAG2_FAST
# define AV_CODEC_FLAG2_FAST CODEC_FLAG2_FAST
#endif
#ifndef FF_INPUT_BUFFER_PADDING_SIZE
# define FF_INPUT_BUFFER_PADDING_SIZE AV_INPUT_BUFFER_PADDING_SIZE
#endif
#ifndef AV_CODEC_FLAG_INTERLACED_DCT
# define AV_CODEC_FLAG_INTERLACED_DCT CODEC_FLAG_INTERLACED_DCT
#endif
#ifndef AV_CODEC_FLAG_INTERLACED_ME
# define AV_CODEC_FLAG_INTERLACED_ME CODEC_FLAG_INTERLACED_ME
#endif
#ifndef AV_CODEC_FLAG_GLOBAL_HEADER
# define AV_CODEC_FLAG_GLOBAL_HEADER CODEC_FLAG_GLOBAL_HEADER
#endif
#ifndef AV_CODEC_FLAG_LOW_DELAY
# define AV_CODEC_FLAG_LOW_DELAY CODEC_FLAG_LOW_DELAY
#endif
#ifndef AV_CODEC_CAP_SMALL_LAST_FRAME
# define AV_CODEC_CAP_SMALL_LAST_FRAME CODEC_CAP_SMALL_LAST_FRAME
#endif
#ifndef AV_INPUT_BUFFER_MIN_SIZE
# define AV_INPUT_BUFFER_MIN_SIZE FF_MIN_BUFFER_SIZE
#endif
#ifndef  FF_MAX_B_FRAMES
# define  FF_MAX_B_FRAMES 16 // FIXME: remove this
#endif

#endif /* HAVE_LIBAVCODEC_AVCODEC_H */

#ifdef HAVE_LIBAVUTIL_AVUTIL_H
# include <libavutil/avutil.h>

/* LIBAVUTIL_VERSION_CHECK checks for the right version of libav and FFmpeg
 * a is the major version
 * b and c the minor and micro versions of libav
 * d and e the minor and micro versions of FFmpeg */
#define LIBAVUTIL_VERSION_CHECK( a, b, c, d, e ) \
    ( (LIBAVUTIL_VERSION_MICRO <  100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( a, b, c ) ) || \
      (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( a, d, e ) ) )

#if !LIBAVUTIL_VERSION_CHECK( 52, 11, 0, 32, 100 )
#   define AV_PIX_FMT_FLAG_HWACCEL  PIX_FMT_HWACCEL
#endif

#endif /* HAVE_LIBAVUTIL_AVUTIL_H */

#if LIBAVUTIL_VERSION_MAJOR >= 55
# define FF_API_AUDIOCONVERT 1
#endif

/* libavutil/pixfmt.h */
#ifndef PixelFormat
# define PixelFormat AVPixelFormat
#endif

#ifdef HAVE_LIBAVFORMAT_AVFORMAT_H
# include <libavformat/avformat.h>

#define LIBAVFORMAT_VERSION_CHECK( a, b, c, d, e ) \
    ( (LIBAVFORMAT_VERSION_MICRO <  100 && LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT( a, b, c ) ) || \
      (LIBAVFORMAT_VERSION_MICRO >= 100 && LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT( a, d, e ) ) )

#endif

//static int cma_get_buffer2(struct AVCodecContext *s, AVFrame *frame, int flags);


/*****************************************************************************
 * Codec fourcc -> libavcodec Codec_id mapping
 * Sorted by AVCodecID enumeration order
 *****************************************************************************/
struct vlc_avcodec_fourcc
{
    vlc_fourcc_t i_fourcc;
    unsigned i_codec;
};

/*
 * Video Codecs
 */
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
    /* ffmpeg only: AV_CODEC_ID_AYUV */
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

/*
 *  Audio Codecs
 */
static const struct vlc_avcodec_fourcc audio_codecs[] =
{
    /* PCM */
    { VLC_CODEC_S16L, AV_CODEC_ID_PCM_S16LE },
    { VLC_CODEC_S16B, AV_CODEC_ID_PCM_S16BE },
    { VLC_CODEC_U16L, AV_CODEC_ID_PCM_U16LE },
    { VLC_CODEC_U16B, AV_CODEC_ID_PCM_U16BE },
    { VLC_CODEC_S8, AV_CODEC_ID_PCM_S8 },
    { VLC_CODEC_U8, AV_CODEC_ID_PCM_U8 },
    { VLC_CODEC_MULAW, AV_CODEC_ID_PCM_MULAW },
    { VLC_CODEC_ALAW, AV_CODEC_ID_PCM_ALAW },
    { VLC_CODEC_S32L, AV_CODEC_ID_PCM_S32LE },
    { VLC_CODEC_S32B, AV_CODEC_ID_PCM_S32BE },
    { VLC_CODEC_U32L, AV_CODEC_ID_PCM_U32LE },
    { VLC_CODEC_U32B, AV_CODEC_ID_PCM_U32BE },
    { VLC_CODEC_S24L, AV_CODEC_ID_PCM_S24LE },
    { VLC_CODEC_S24B, AV_CODEC_ID_PCM_S24BE },
    { VLC_CODEC_U24L, AV_CODEC_ID_PCM_U24LE },
    { VLC_CODEC_U24B, AV_CODEC_ID_PCM_U24BE },
    { VLC_CODEC_S24DAUD, AV_CODEC_ID_PCM_S24DAUD },
    /* AV_CODEC_ID_PCM_ZORK */
    { VLC_CODEC_S16L_PLANAR, AV_CODEC_ID_PCM_S16LE_PLANAR },
    /* AV_CODEC_ID_PCM_DVD */
    { VLC_CODEC_F32B, AV_CODEC_ID_PCM_F32BE },
    { VLC_CODEC_F32L, AV_CODEC_ID_PCM_F32LE },
    { VLC_CODEC_F64B, AV_CODEC_ID_PCM_F64BE },
    { VLC_CODEC_F64L, AV_CODEC_ID_PCM_F64LE },
    { VLC_CODEC_BD_LPCM, AV_CODEC_ID_PCM_BLURAY },
    /* AV_CODEC_ID_PCM_LXF */
    /* AV_CODEC_ID_S302M */
    /* AV_CODEC_ID_PCM_S8_PLANAR */
    /* AV_CODEC_ID_PCM_S24LE_PLANAR */
    /* AV_CODEC_ID_PCM_S32LE_PLANAR */
    /* ffmpeg only: AV_CODEC_ID_PCM_S16BE_PLANAR */

    /* ADPCM */
    { VLC_CODEC_ADPCM_IMA_QT, AV_CODEC_ID_ADPCM_IMA_QT },
    { VLC_CODEC_ADPCM_IMA_WAV, AV_CODEC_ID_ADPCM_IMA_WAV },
    /* AV_CODEC_ID_ADPCM_IMA_DK3 */
    /* AV_CODEC_ID_ADPCM_IMA_DK4 */
    { VLC_CODEC_ADPCM_IMA_WS, AV_CODEC_ID_ADPCM_IMA_WS },
    /* AV_CODEC_ID_ADPCM_IMA_SMJPEG */
    { VLC_CODEC_ADPCM_MS, AV_CODEC_ID_ADPCM_MS },
    { VLC_CODEC_ADPCM_4XM, AV_CODEC_ID_ADPCM_4XM },
    { VLC_CODEC_ADPCM_XA, AV_CODEC_ID_ADPCM_XA },
    { VLC_CODEC_ADPCM_ADX, AV_CODEC_ID_ADPCM_ADX },
    { VLC_CODEC_ADPCM_EA, AV_CODEC_ID_ADPCM_EA },
    { VLC_CODEC_ADPCM_G726, AV_CODEC_ID_ADPCM_G726 },
    { VLC_CODEC_ADPCM_CREATIVE, AV_CODEC_ID_ADPCM_CT },
    { VLC_CODEC_ADPCM_SWF, AV_CODEC_ID_ADPCM_SWF },
    { VLC_CODEC_ADPCM_YAMAHA, AV_CODEC_ID_ADPCM_YAMAHA },
    { VLC_CODEC_ADPCM_SBPRO_4, AV_CODEC_ID_ADPCM_SBPRO_4 },
    { VLC_CODEC_ADPCM_SBPRO_3, AV_CODEC_ID_ADPCM_SBPRO_3 },
    { VLC_CODEC_ADPCM_SBPRO_2, AV_CODEC_ID_ADPCM_SBPRO_2 },
    { VLC_CODEC_ADPCM_THP, AV_CODEC_ID_ADPCM_THP },
    { VLC_CODEC_ADPCM_IMA_AMV, AV_CODEC_ID_ADPCM_IMA_AMV },
    { VLC_CODEC_ADPCM_EA_R1, AV_CODEC_ID_ADPCM_EA_R1 },
    /* AV_CODEC_ID_ADPCM_EA_R3 */
    /* AV_CODEC_ID_ADPCM_EA_R2 */
    { VLC_CODEC_ADPCM_IMA_EA_SEAD, AV_CODEC_ID_ADPCM_IMA_EA_SEAD },
    /* AV_CODEC_ID_ADPCM_IMA_EA_EACS */
    /* AV_CODEC_ID_ADPCM_EA_XAS */
    /* AV_CODEC_ID_ADPCM_EA_MAXIS_XA */
    /* AV_CODEC_ID_ADPCM_IMA_ISS */
    { VLC_CODEC_ADPCM_G722, AV_CODEC_ID_ADPCM_G722 },
    { VLC_CODEC_ADPCM_IMA_APC, AV_CODEC_ID_ADPCM_IMA_APC },
    /* ffmpeg only: AV_CODEC_ID_VIMA */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_AFC */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_IMA_OKI */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_DTK */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_IMA_RAD */
    /* ffmpeg only: AV_CODEC_ID_ADPCM_G726LE */

    /* AMR */
    { VLC_CODEC_AMR_NB, AV_CODEC_ID_AMR_NB },
    { VLC_CODEC_AMR_WB, AV_CODEC_ID_AMR_WB },

    /* RealAudio */
    { VLC_CODEC_RA_144, AV_CODEC_ID_RA_144 },
    { VLC_CODEC_RA_288, AV_CODEC_ID_RA_288 },

    /* DPCM */
    { VLC_CODEC_ROQ_DPCM, AV_CODEC_ID_ROQ_DPCM },
    { VLC_CODEC_INTERPLAY_DPCM, AV_CODEC_ID_INTERPLAY_DPCM },
    /* AV_CODEC_ID_XAN_DPCM */
    /* AV_CODEC_ID_SOL_DPCM */

    /* audio codecs */
    { VLC_CODEC_MPGA, AV_CODEC_ID_MP2 },
    { VLC_CODEC_MP2, AV_CODEC_ID_MP2 },
    { VLC_CODEC_MP3, AV_CODEC_ID_MP3 },
    { VLC_CODEC_MP4A, AV_CODEC_ID_AAC },
    { VLC_CODEC_A52, AV_CODEC_ID_AC3 },
    { VLC_CODEC_DTS, AV_CODEC_ID_DTS },
    { VLC_CODEC_VORBIS, AV_CODEC_ID_VORBIS },
    { VLC_CODEC_DVAUDIO, AV_CODEC_ID_DVAUDIO },
    { VLC_CODEC_WMA1, AV_CODEC_ID_WMAV1 },
    { VLC_CODEC_WMA2, AV_CODEC_ID_WMAV2 },
    { VLC_CODEC_MACE3, AV_CODEC_ID_MACE3 },
    { VLC_CODEC_MACE6, AV_CODEC_ID_MACE6 },
    { VLC_CODEC_VMDAUDIO, AV_CODEC_ID_VMDAUDIO },
    { VLC_CODEC_FLAC, AV_CODEC_ID_FLAC },
    /* AV_CODEC_ID_MP3ADU */
    /* AV_CODEC_ID_MP3ON4 */
    { VLC_CODEC_SHORTEN, AV_CODEC_ID_SHORTEN },
    { VLC_CODEC_ALAC, AV_CODEC_ID_ALAC },
    /* AV_CODEC_ID_WESTWOOD_SND1 */
    { VLC_CODEC_GSM, AV_CODEC_ID_GSM },
    { VLC_CODEC_QDM2, AV_CODEC_ID_QDM2 },
#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 71, 100 )
    { VLC_CODEC_QDMC, AV_CODEC_ID_QDMC },
#endif
    { VLC_CODEC_COOK, AV_CODEC_ID_COOK },
    { VLC_CODEC_TRUESPEECH, AV_CODEC_ID_TRUESPEECH },
    { VLC_CODEC_TTA, AV_CODEC_ID_TTA },
    { VLC_CODEC_SMACKAUDIO, AV_CODEC_ID_SMACKAUDIO },
    { VLC_CODEC_QCELP, AV_CODEC_ID_QCELP },
    { VLC_CODEC_WAVPACK, AV_CODEC_ID_WAVPACK },
    { VLC_CODEC_DSICINAUDIO, AV_CODEC_ID_DSICINAUDIO },
    { VLC_CODEC_IMC, AV_CODEC_ID_IMC },
    { VLC_CODEC_MUSEPACK7, AV_CODEC_ID_MUSEPACK7 },
    { VLC_CODEC_MLP, AV_CODEC_ID_MLP },
    { VLC_CODEC_GSM_MS, AV_CODEC_ID_GSM_MS },
    { VLC_CODEC_ATRAC3, AV_CODEC_ID_ATRAC3 },
    { VLC_CODEC_APE, AV_CODEC_ID_APE },
    { VLC_CODEC_NELLYMOSER, AV_CODEC_ID_NELLYMOSER },
    { VLC_CODEC_MUSEPACK8, AV_CODEC_ID_MUSEPACK8 },
    { VLC_CODEC_SPEEX, AV_CODEC_ID_SPEEX },
    { VLC_CODEC_WMAS, AV_CODEC_ID_WMAVOICE },
    { VLC_CODEC_WMAP, AV_CODEC_ID_WMAPRO },
    { VLC_CODEC_WMAL, AV_CODEC_ID_WMALOSSLESS },
    { VLC_CODEC_ATRAC3P, AV_CODEC_ID_ATRAC3P },
    { VLC_CODEC_EAC3, AV_CODEC_ID_EAC3 },
    { VLC_CODEC_SIPR, AV_CODEC_ID_SIPR },
    /* AV_CODEC_ID_MP1 */
    { VLC_CODEC_TWINVQ, AV_CODEC_ID_TWINVQ },
    { VLC_CODEC_TRUEHD, AV_CODEC_ID_TRUEHD },
    { VLC_CODEC_ALS, AV_CODEC_ID_MP4ALS },
    { VLC_CODEC_ATRAC1, AV_CODEC_ID_ATRAC1 },
    { VLC_CODEC_BINKAUDIO_RDFT, AV_CODEC_ID_BINKAUDIO_RDFT },
    { VLC_CODEC_BINKAUDIO_DCT, AV_CODEC_ID_BINKAUDIO_DCT },
    { VLC_CODEC_MP4A, AV_CODEC_ID_AAC_LATM },
    /* AV_CODEC_ID_QDMC */
    /* AV_CODEC_ID_CELT */
    { VLC_CODEC_G723_1, AV_CODEC_ID_G723_1 },
    /* AV_CODEC_ID_G729 */
    /* AV_CODEC_ID_8SVX_EXP */
    /* AV_CODEC_ID_8SVX_FIB */
    { VLC_CODEC_BMVAUDIO, AV_CODEC_ID_BMV_AUDIO },
    { VLC_CODEC_RALF, AV_CODEC_ID_RALF },
    { VLC_CODEC_INDEO_AUDIO, AV_CODEC_ID_IAC },
    /* AV_CODEC_ID_ILBC */
    { VLC_CODEC_OPUS, AV_CODEC_ID_OPUS },
    /* AV_CODEC_ID_COMFORT_NOISE */
    { VLC_CODEC_TAK, AV_CODEC_ID_TAK },
    { VLC_CODEC_METASOUND, AV_CODEC_ID_METASOUND },
    /* AV_CODEC_ID_PAF_AUDIO */
    { VLC_CODEC_ON2AVC, AV_CODEC_ID_ON2AVC },

    /* ffmpeg only: AV_CODEC_ID_FFWAVESYNTH */
    /* ffmpeg only: AV_CODEC_ID_SONIC */
    /* ffmpeg only: AV_CODEC_ID_SONIC_LS */
    /* ffmpeg only: AV_CODEC_ID_PAF_AUDIO */
    /* ffmpeg only: AV_CODEC_ID_EVRC */
    /* ffmpeg only: AV_CODEC_ID_SMV */
};

/* Subtitle streams */
static const struct vlc_avcodec_fourcc spu_codecs[] =
{
    { VLC_CODEC_SPU, AV_CODEC_ID_DVD_SUBTITLE },
    { VLC_CODEC_DVBS, AV_CODEC_ID_DVB_SUBTITLE },
    { VLC_CODEC_SUBT, AV_CODEC_ID_TEXT },
    { VLC_CODEC_XSUB, AV_CODEC_ID_XSUB },
    { VLC_CODEC_SSA, AV_CODEC_ID_SSA },
    /* AV_CODEC_ID_MOV_TEXT */
    { VLC_CODEC_BD_PG, AV_CODEC_ID_HDMV_PGS_SUBTITLE },
#if LIBAVCODEC_VERSION_CHECK( 57, 999, 999, 71, 100 )
    { VLC_CODEC_BD_TEXT, AV_CODEC_ID_HDMV_TEXT_SUBTITLE },
#endif
    { VLC_CODEC_TELETEXT, AV_CODEC_ID_DVB_TELETEXT },
    /* AV_CODEC_ID_SRT */
    /* ffmpeg only: AV_CODEC_ID_MICRODVD */
    /* ffmpeg only: AV_CODEC_ID_EIA_608 */
    /* ffmpeg only: AV_CODEC_ID_JACOSUB */
    /* ffmpeg only: AV_CODEC_ID_SAMI */
    /* ffmpeg only: AV_CODEC_ID_REALTEXT */
    /* ffmpeg only: AV_CODEC_ID_SUBVIEWER1 */
    /* ffmpeg only: AV_CODEC_ID_SUBVIEWER */
    /* ffmpeg only: AV_CODEC_ID_SUBRIP */
    /* ffmpeg only: AV_CODEC_ID_WEBVTT */
    /* ffmpeg only: AV_CODEC_ID_MPL2 */
    /* ffmpeg only: AV_CODEC_ID_VPLAYER */
    /* ffmpeg only: AV_CODEC_ID_PJS */
    /* ffmpeg only: AV_CODEC_ID_ASS */
};

static bool GetFfmpegCodec( enum es_format_category_e cat, vlc_fourcc_t i_fourcc,
                     unsigned *pi_ffmpeg_codec, const char **ppsz_name )
{
    const struct vlc_avcodec_fourcc *base;
    size_t count;

    switch( cat )
    {
        case VIDEO_ES:
            base = video_codecs;
            count = ARRAY_SIZE(video_codecs);
            break;
        case AUDIO_ES:
            base = audio_codecs;
            count = ARRAY_SIZE(audio_codecs);
            break;
        case SPU_ES:
            base = spu_codecs;
            count = ARRAY_SIZE(spu_codecs);
            break;
        default:
            base = NULL;
            count = 0;
    }

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

/*****************************************************************************
 * Chroma fourcc -> libavutil pixfmt mapping
 *****************************************************************************/
#if defined(WORDS_BIGENDIAN)
#   define VLC_RGB_ES( fcc, leid, beid ) \
    { fcc, beid, 0, 0, 0 },
#else
#   define VLC_RGB_ES( fcc, leid, beid ) \
    { fcc, leid, 0, 0, 0 },
#endif

#define VLC_RGB( fcc, leid, beid, rmask, gmask, bmask ) \
    { fcc, leid, rmask, gmask, bmask }, \
    { fcc, beid, bmask, gmask, rmask }, \
    VLC_RGB_ES( fcc, leid, beid )


static const struct
{
    vlc_fourcc_t  i_chroma;
    int           i_chroma_id;
    uint32_t      i_rmask;
    uint32_t      i_gmask;
    uint32_t      i_bmask;

} chroma_table[] =
{
    // Sand
//    {VLC_CODEC_MMAL_OPAQUE, AV_PIX_FMT_SAND128, 0, 0, 0 },
    {VLC_CODEC_MMAL_ZC_SAND8, AV_PIX_FMT_SAND128, 0, 0, 0 },
    {VLC_CODEC_MMAL_ZC_SAND10, AV_PIX_FMT_SAND64_10, 0, 0, 0 },

    /* Planar YUV formats */
    {VLC_CODEC_I444, AV_PIX_FMT_YUV444P, 0, 0, 0 },
    {VLC_CODEC_J444, AV_PIX_FMT_YUVJ444P, 0, 0, 0 },

    {VLC_CODEC_I440, AV_PIX_FMT_YUV440P, 0, 0, 0 },
    {VLC_CODEC_J440, AV_PIX_FMT_YUVJ440P, 0, 0, 0 },

    {VLC_CODEC_I422, AV_PIX_FMT_YUV422P, 0, 0, 0 },
    {VLC_CODEC_J422, AV_PIX_FMT_YUVJ422P, 0, 0, 0 },

    {VLC_CODEC_MMAL_ZC_I420, AV_PIX_FMT_YUV420P, 0, 0, 0 },
    {VLC_CODEC_YV12, AV_PIX_FMT_YUV420P, 0, 0, 0 },
    {VLC_FOURCC('I','Y','U','V'), AV_PIX_FMT_YUV420P, 0, 0, 0 },
    {VLC_CODEC_J420, AV_PIX_FMT_YUVJ420P, 0, 0, 0 },
    {VLC_CODEC_I411, AV_PIX_FMT_YUV411P, 0, 0, 0 },
    {VLC_CODEC_I410, AV_PIX_FMT_YUV410P, 0, 0, 0 },
    {VLC_FOURCC('Y','V','U','9'), AV_PIX_FMT_YUV410P, 0, 0, 0 },

    {VLC_CODEC_NV12, AV_PIX_FMT_NV12, 0, 0, 0 },
    {VLC_CODEC_NV21, AV_PIX_FMT_NV21, 0, 0, 0 },

    {VLC_CODEC_I420_9L, AV_PIX_FMT_YUV420P9LE, 0, 0, 0 },
    {VLC_CODEC_I420_9B, AV_PIX_FMT_YUV420P9BE, 0, 0, 0 },
    {VLC_CODEC_I420_10L, AV_PIX_FMT_YUV420P10LE, 0, 0, 0 },
    {VLC_CODEC_I420_10B, AV_PIX_FMT_YUV420P10BE, 0, 0, 0 },
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 54, 17, 100 ) )
    {VLC_CODEC_I420_12L, AV_PIX_FMT_YUV420P12LE, 0, 0, 0 },
    {VLC_CODEC_I420_12B, AV_PIX_FMT_YUV420P12BE, 0, 0, 0 },
#endif
    {VLC_CODEC_I420_16L, AV_PIX_FMT_YUV420P16LE, 0, 0, 0 },
    {VLC_CODEC_I420_16B, AV_PIX_FMT_YUV420P16BE, 0, 0, 0 },
#ifdef AV_PIX_FMT_P010
    {VLC_CODEC_P010, AV_PIX_FMT_P010, 0, 0, 0 },
#endif

    {VLC_CODEC_I422_9L, AV_PIX_FMT_YUV422P9LE, 0, 0, 0 },
    {VLC_CODEC_I422_9B, AV_PIX_FMT_YUV422P9BE, 0, 0, 0 },
    {VLC_CODEC_I422_10L, AV_PIX_FMT_YUV422P10LE, 0, 0, 0 },
    {VLC_CODEC_I422_10B, AV_PIX_FMT_YUV422P10BE, 0, 0, 0 },
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 54, 17, 100 ) )
    {VLC_CODEC_I422_12L, AV_PIX_FMT_YUV422P12LE, 0, 0, 0 },
    {VLC_CODEC_I422_12B, AV_PIX_FMT_YUV422P12BE, 0, 0, 0 },
#endif

    {VLC_CODEC_YUV420A, AV_PIX_FMT_YUVA420P, 0, 0, 0 },
    {VLC_CODEC_YUV422A, AV_PIX_FMT_YUVA422P, 0, 0, 0 },
    {VLC_CODEC_YUVA,    AV_PIX_FMT_YUVA444P, 0, 0, 0 },

    {VLC_CODEC_YUVA_444_10L, AV_PIX_FMT_YUVA444P10LE, 0, 0, 0 },
    {VLC_CODEC_YUVA_444_10B, AV_PIX_FMT_YUVA444P10BE, 0, 0, 0 },

    {VLC_CODEC_I444_9L, AV_PIX_FMT_YUV444P9LE, 0, 0, 0 },
    {VLC_CODEC_I444_9B, AV_PIX_FMT_YUV444P9BE, 0, 0, 0 },
    {VLC_CODEC_I444_10L, AV_PIX_FMT_YUV444P10LE, 0, 0, 0 },
    {VLC_CODEC_I444_10B, AV_PIX_FMT_YUV444P10BE, 0, 0, 0 },
#if (LIBAVUTIL_VERSION_MICRO >= 100 && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT( 54, 17, 100 ) )
    {VLC_CODEC_I444_12L, AV_PIX_FMT_YUV444P12LE, 0, 0, 0 },
    {VLC_CODEC_I444_12B, AV_PIX_FMT_YUV444P12BE, 0, 0, 0 },
#endif
    {VLC_CODEC_I444_16L, AV_PIX_FMT_YUV444P16LE, 0, 0, 0 },
    {VLC_CODEC_I444_16B, AV_PIX_FMT_YUV444P16BE, 0, 0, 0 },

    /* Packed YUV formats */
    {VLC_CODEC_YUYV, AV_PIX_FMT_YUYV422, 0, 0, 0 },
    {VLC_FOURCC('Y','U','Y','V'), AV_PIX_FMT_YUYV422, 0, 0, 0 },
    {VLC_CODEC_UYVY, AV_PIX_FMT_UYVY422, 0, 0, 0 },
    {VLC_CODEC_YVYU, AV_PIX_FMT_YVYU422, 0, 0, 0 },
    {VLC_FOURCC('Y','4','1','1'), AV_PIX_FMT_UYYVYY411, 0, 0, 0 },

    /* Packed RGB formats */
    VLC_RGB( VLC_FOURCC('R','G','B','4'), AV_PIX_FMT_RGB4, AV_PIX_FMT_BGR4, 0x10, 0x06, 0x01 )
    VLC_RGB( VLC_CODEC_RGB8, AV_PIX_FMT_RGB8, AV_PIX_FMT_BGR8, 0xC0, 0x38, 0x07 )

    VLC_RGB( VLC_CODEC_RGB15, AV_PIX_FMT_RGB555, AV_PIX_FMT_BGR555, 0x7c00, 0x03e0, 0x001f )
    VLC_RGB( VLC_CODEC_RGB16, AV_PIX_FMT_RGB565, AV_PIX_FMT_BGR565, 0xf800, 0x07e0, 0x001f )
    VLC_RGB( VLC_CODEC_RGB24, AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24, 0xff0000, 0x00ff00, 0x0000ff )

    VLC_RGB( VLC_CODEC_RGB32, AV_PIX_FMT_RGB32, AV_PIX_FMT_BGR32, 0x00ff0000, 0x0000ff00, 0x000000ff )
    VLC_RGB( VLC_CODEC_RGB32, AV_PIX_FMT_RGB32_1, AV_PIX_FMT_BGR32_1, 0xff000000, 0x00ff0000, 0x0000ff00 )

#ifdef AV_PIX_FMT_0BGR32
    VLC_RGB( VLC_CODEC_RGB32, AV_PIX_FMT_0BGR32, AV_PIX_FMT_0RGB32, 0x000000ff, 0x0000ff00, 0x00ff0000 )
#endif

    {VLC_CODEC_RGBA, AV_PIX_FMT_RGBA, 0, 0, 0 },
    {VLC_CODEC_ARGB, AV_PIX_FMT_ARGB, 0, 0, 0 },
    {VLC_CODEC_BGRA, AV_PIX_FMT_BGRA, 0, 0, 0 },
    {VLC_CODEC_GREY, AV_PIX_FMT_GRAY8, 0, 0, 0},

     /* Paletized RGB */
    {VLC_CODEC_RGBP, AV_PIX_FMT_PAL8, 0, 0, 0},

    {VLC_CODEC_GBR_PLANAR, AV_PIX_FMT_GBRP, 0, 0, 0 },
    {VLC_CODEC_GBR_PLANAR_9L, AV_PIX_FMT_GBRP9LE, 0, 0, 0 },
    {VLC_CODEC_GBR_PLANAR_9B, AV_PIX_FMT_GBRP9BE, 0, 0, 0 },
    {VLC_CODEC_GBR_PLANAR_10L, AV_PIX_FMT_GBRP10LE, 0, 0, 0 },
    {VLC_CODEC_GBR_PLANAR_10B, AV_PIX_FMT_GBRP10BE, 0, 0, 0 },

    /* XYZ */
#if LIBAVUTIL_VERSION_CHECK(52, 10, 0, 25, 100)
    {VLC_CODEC_XYZ12, AV_PIX_FMT_XYZ12, 0xfff0, 0xfff0, 0xfff0},
#endif
    { 0, 0, 0, 0, 0 }
};

static inline void wait_mt(decoder_sys_t *sys)
{
//    vlc_sem_wait(&sys->sem_mt);
}

static inline void post_mt(decoder_sys_t *sys)
{
//    vlc_sem_post(&sys->sem_mt);
}

static vlc_fourcc_t get_hw_chroma(const enum AVPixelFormat ffmpeg_hw_chroma, const enum AVPixelFormat ffmpeg_ww_chroma)
{
    VLC_UNUSED(ffmpeg_ww_chroma);
    switch (ffmpeg_hw_chroma)
    {
        case AV_PIX_FMT_RPI4_8:
            return VLC_CODEC_MMAL_ZC_SAND8;
        case AV_PIX_FMT_RPI4_10:
            return VLC_CODEC_MMAL_ZC_SAND30;
        default:
            break;
    }
    return 0;
}

static int GetVlcChroma( video_format_t *fmt, int i_ffmpeg_chroma )
{
    /* TODO FIXME for rgb format we HAVE to set rgb mask/shift */
    for( int i = 0; chroma_table[i].i_chroma != 0; i++ )
    {
        if( chroma_table[i].i_chroma_id == i_ffmpeg_chroma )
        {
            fmt->i_rmask = chroma_table[i].i_rmask;
            fmt->i_gmask = chroma_table[i].i_gmask;
            fmt->i_bmask = chroma_table[i].i_bmask;
            fmt->i_chroma = chroma_table[i].i_chroma;
            return VLC_SUCCESS;
        }
    }
    return VLC_EGENERIC;
}

//#include "../codec/cc.h"

static AVCodecContext *ffmpeg_AllocContext( decoder_t *p_dec,
                                     const AVCodec **restrict codecp )
{
    unsigned i_codec_id;
    const char *psz_namecodec;
    const AVCodec *p_codec = NULL;

    /* *** determine codec type *** */
    if( !GetFfmpegCodec( p_dec->fmt_in.i_cat, p_dec->fmt_in.i_codec,
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
    {
        if( p_dec->fmt_in.i_codec != VLC_CODEC_HEVC )
            p_codec = avcodec_find_decoder(i_codec_id);
        else
        {
            psz_namecodec = rpi_is_model_pi4() ? "hevc" : "hevc_rpi";
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
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    AVCodecContext *p_context;
    const AVCodec  *p_codec;

    /* Video decoder specific part */
    date_t  pts;

    /* Closed captions for decoders */
//    cc_data_t cc;

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
    enum AVPixelFormat pix_fmt;
    int profile;
    int level;

    cma_buf_pool_t * cma_pool;
    vcsm_init_type_t vcsm_init_type;
};

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void ffmpeg_InitCodec      ( decoder_t * );
static enum PixelFormat ffmpeg_GetFormat( AVCodecContext *,
                                          const enum PixelFormat * );
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
        fmt->i_chroma = get_hw_chroma(pix_fmt, sw_pix_fmt);

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

    if( ctx->color_range == AVCOL_RANGE_JPEG )
        fmt->b_color_range_full = true;

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

    val = lavc_GetVideoFormat(dec, &fmt_out, ctx, fmt, swfmt);
    if (val)
        return val;

    /* always have date in fields/ticks units */
    if(dec->p_sys->pts.i_divider_num)
        date_Change(&dec->p_sys->pts, fmt_out.i_frame_rate *
                                      __MAX(ctx->ticks_per_frame, 1),
                                      fmt_out.i_frame_rate_base);
    else
        date_Init(&dec->p_sys->pts, fmt_out.i_frame_rate *
                                    __MAX(ctx->ticks_per_frame, 1),
                                    fmt_out.i_frame_rate_base);

    fmt_out.p_palette = dec->fmt_out.video.p_palette;
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

    return decoder_UpdateVideoFormat(dec);
}

/*****************************************************************************
 * Flush:
 *****************************************************************************/
static void Flush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

    date_Set(&p_sys->pts, VLC_TS_INVALID); /* To make sure we recover properly */
    p_sys->i_late_frames = 0;
//    cc_Flush( &p_sys->cc );

    /* Abort pictures in order to unblock all avcodec workers threads waiting
     * for a picture. This will avoid a deadlock between avcodec_flush_buffers
     * and workers threads */
    decoder_AbortPictures( p_dec, true );

    /* do not flush buffers if codec hasn't been opened (theora/vorbis/VC1) */
    if( avcodec_is_open( p_context ) )
        avcodec_flush_buffers( p_context );

    /* Reset cancel state to false */
    decoder_AbortPictures( p_dec, false );
}

static bool check_block_validity( decoder_sys_t *p_sys, block_t *block )
{
    if( !block)
        return true;

    if( block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        date_Set( &p_sys->pts, VLC_TS_INVALID ); /* To make sure we recover properly */
//        cc_Flush( &p_sys->cc );

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

static void interpolate_next_pts( decoder_t *p_dec, AVFrame *frame )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

    if( date_Get( &p_sys->pts ) == VLC_TS_INVALID ||
        p_sys->pts.i_divider_num == 0 )
        return;

    int i_tick = p_context->ticks_per_frame;
    if( i_tick <= 0 )
        i_tick = 1;

    /* interpolate the next PTS */
    date_Increment( &p_sys->pts, i_tick + frame->repeat_pict );
}

static void update_late_frame_count( decoder_t *p_dec, block_t *p_block, mtime_t current_time, mtime_t i_pts )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
   /* Update frame late count (except when doing preroll) */
   mtime_t i_display_date = VLC_TS_INVALID;
   if( !p_block || !(p_block->i_flags & BLOCK_FLAG_PREROLL) )
       i_display_date = decoder_GetDisplayDate( p_dec, i_pts );

   if( i_display_date > VLC_TS_INVALID && i_display_date <= current_time )
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
//    decoder_sys_t *p_sys = p_dec->p_sys;
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
#if 0
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
#endif
    return 0;
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


static int ffmpeg_OpenCodec( decoder_t *p_dec, AVCodecContext *ctx,
                      const AVCodec *codec )
{
    char *psz_opts = var_InheritString( p_dec, "avcodec-options" );
    AVDictionary *options = NULL;
    int ret;

    if (psz_opts) {
        vlc_av_get_options(psz_opts, &options);
        free(psz_opts);
    }

    if (av_rpi_zc_init2(ctx, p_dec, zc_alloc_buf, zc_free_pool) != 0)
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

static int OpenVideoCodec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
    const AVCodec *codec = p_sys->p_codec;
    int ret;

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s", __func__);
#endif

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
//    cc_Init( &p_sys->cc );

    set_video_color_settings( &p_dec->fmt_in.video, ctx );

    ret = ffmpeg_OpenCodec( p_dec, ctx, codec );
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


static const uint8_t shift_01[] = {0,1,1,1};
static const uint8_t pb_1[] = {1,1,1,1};
static const uint8_t pb_12[] = {1,2,2,2};
static const uint8_t pb_24[] = {2,4,4,4};
static const uint8_t pb_4[] = {4,4,4,4};

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

    uint8_t * frame_end = frame->data[0] + cma_buf_size(cb);
    for (int i = 0; i != pic->i_planes; ++i) {
        // Calculate lines from gap between planes
        // This will give us an accurate "height" for later use by MMAL
        const int lines = ((i + 1 == pic->i_planes ? frame_end : frame->data[i + 1]) -
                           frame->data[i]) / frame->linesize[i];
        pic->p[i] = (plane_t){
            .p_pixels = data + (frame->data[i] - frame->data[0]),
            .i_lines = lines,
            .i_pitch = frame->linesize[i],
            .i_pixel_pitch = pb[i],
            .i_visible_lines = av_frame_cropped_height(frame) >> hs[i],
            .i_visible_pitch = av_frame_cropped_width(frame) >> ws[i]
        };
    }
    return 0;
}

/*****************************************************************************
 * DecodeBlock: Called to decode one or more frames
 *****************************************************************************/


// Returns
// -ve    error
// 0      Need more input (EAGAIN)
// 1      Frame decoded (dropped or Qed)
// 2      Decode err
// 3      EOF

static int rx_frame(decoder_t * const p_dec, decoder_sys_t * const p_sys, AVCodecContext * const p_context)
{
    AVFrame * frame = av_frame_alloc();
    picture_t * p_pic = NULL;
    int ret;

    if (frame == NULL)
        return VLC_ENOMEM;

    ret = avcodec_receive_frame(p_context, frame);

    if (ret != 0)
    {
        av_frame_free(&frame);
        switch (ret)
        {
            case AVERROR(EAGAIN):
                return 0;

            case AVERROR(ENOMEM):
            case AVERROR(EINVAL):
                msg_Err(p_dec, "avcodec_receive_frame critical error");
                return VLC_EGENERIC;

            case AVERROR_EOF:
                msg_Dbg(p_dec, "Rx EOF");
                avcodec_flush_buffers(p_context);
                return 2;

            default:
                msg_Warn(p_dec, "Decode error: %d", ret);
                return 1;
        }
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

    interpolate_next_pts( p_dec, frame );

    // update_late_frame_count( p_dec, p_block, current_time, i_pts); // ** ?? **

    if( ( /* !p_sys->p_va && */ !frame->linesize[0] ) ||
       ( p_dec->b_frame_drop_allowed && (frame->flags & AV_FRAME_FLAG_CORRUPT) &&
         !p_sys->b_show_corrupted ) )
    {
        msg_Dbg(p_dec, "Frame drop");
        av_frame_free(&frame);
        return 1;
    }

    lavc_UpdateVideoFormat(p_dec, p_context, p_context->pix_fmt,
                              p_context->pix_fmt);


    {
        cma_buf_t * const cb = av_rpi_zc_buf_v(frame->buf[0]);

        if (cb == NULL)
        {
            msg_Err(p_dec, "Frame has no attached CMA buffer");
            av_frame_free(&frame);
            return 1;
        }

        if ((p_pic = decoder_NewPicture(p_dec)) == NULL)
        {
            msg_Err(p_dec, "Failed to allocate pic");
            av_frame_free(&frame);
            return 1;
        }

        cma_buf_in_flight(cb);

        if (cma_buf_pic_attach(cma_buf_ref(cb), p_pic) != 0)
        {
            char dbuf0[5];
            msg_Err(p_dec, "Failed to attach bufs to pic: fmt=%s", str_fourcc(dbuf0, p_pic->format.i_chroma));
            cma_buf_unref(cb);
            av_frame_free(&frame);
            picture_Release(p_pic);
            return 1;
        }

        // ****** Set planes etc.
        set_pic_from_frame(p_pic, frame);
    }


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
#if TRACE_ALL
    msg_Dbg(p_dec, "--- QueueVideo");
#endif
    p_sys->b_first_frame = false;
    decoder_QueueVideo(p_dec, p_pic);

    return 1;
}





static int DecodeVideo( decoder_t *p_dec, block_t * p_block)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;
    /* Boolean if we assume that we should get valid pic as result */
    bool b_need_output_picture = true;

    /* Boolean for END_OF_SEQUENCE */
    bool eos_spotted = false;
    mtime_t current_time;
    int rv = VLCDEC_SUCCESS;

#if TRACE_ALL
    msg_Dbg(p_dec, "<<< %s: (buf_size=%d)", __func__, p_block == NULL ? 0 : p_block->i_buffer);
#endif

    if( !p_context->extradata_size && p_dec->fmt_in.i_extra )
    {
        ffmpeg_InitCodec( p_dec );
        if( !avcodec_is_open( p_context ) )
            OpenVideoCodec( p_dec );
    }

    if(!p_block && !(p_sys->p_codec->capabilities & AV_CODEC_CAP_DELAY) )
        return VLCDEC_SUCCESS;

    if( !avcodec_is_open( p_context ) )
    {
        if( p_block )
            block_Release( p_block );
        return VLCDEC_ECRITICAL;
    }

    if( !check_block_validity( p_sys, p_block ) )
        return VLCDEC_SUCCESS;

    current_time = mdate();
    if( p_dec->b_frame_drop_allowed &&  check_block_being_late( p_sys, p_block, current_time) )
    {
        msg_Err( p_dec, "more than 5 seconds of late video -> "
                 "dropping frame (computer too slow ?)" );
        return VLCDEC_SUCCESS;
    }


    /* A good idea could be to decode all I pictures and see for the other */
    b_need_output_picture = true;

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
            return VLCDEC_SUCCESS;
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
            return VLCDEC_ECRITICAL;

        p_block->i_buffer -= FF_INPUT_BUFFER_PADDING_SIZE;
        memset( p_block->p_buffer + p_block->i_buffer, 0,
                FF_INPUT_BUFFER_PADDING_SIZE );
    }

    AVPacket pkt = {.data = NULL, .size = 0};

    av_init_packet( &pkt );
    if( p_block && p_block->i_buffer > 0 )
    {
        pkt.data = p_block->p_buffer;
        pkt.size = p_block->i_buffer;
        pkt.pts = p_block->i_pts > VLC_TS_INVALID ? p_block->i_pts : AV_NOPTS_VALUE;
        pkt.dts = p_block->i_dts > VLC_TS_INVALID ? p_block->i_dts : AV_NOPTS_VALUE;
    }

    if( !p_sys->palette_sent )
    {
        uint8_t *pal = av_packet_new_side_data(&pkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
        if (pal) {
            memcpy(pal, p_dec->fmt_in.video.p_palette->palette, AVPALETTE_SIZE);
            p_sys->palette_sent = true;
        }
    }

#if LIBAVCODEC_VERSION_CHECK( 57, 0, 0xFFFFFFFFU, 64, 101 )
    if( !b_need_output_picture )
        pkt.flags |= AV_PKT_FLAG_DISCARD;
#endif

    int ret = avcodec_send_packet(p_context, &pkt);

    if (ret == AVERROR(EAGAIN))
    {
        // Cannot send more data until output drained - so do drain
        while (rx_frame(p_dec, p_sys, p_context) == 1)
            /* Loop */;

        // And try again - should not fail the same way
        ret = avcodec_send_packet(p_context, &pkt);
    }

    // Now done with pkt & block
    av_packet_unref(&pkt);
    if (p_block != NULL)
    {
        block_Release( p_block );
        p_block = NULL;
    }

    if (ret != 0)
    {
        if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
        {
            msg_Err(p_dec, "avcodec_send_packet critical error");
            rv = VLCDEC_ECRITICAL;
        }
    }

    while (rx_frame(p_dec, p_sys, p_context) == 1)
        /* Loop */;

    if (eos_spotted)
        p_sys->b_first_frame = true;

#if TRACE_ALL
    msg_Dbg(p_dec, ">>> %s", __func__);
#endif

    return rv;
}

/*****************************************************************************
 * EndVideo: decoder destruction
 *****************************************************************************
 * This function is called when the thread ends after a successful
 * initialization.
 *****************************************************************************/
static void MmalAvcodecCloseDecoder(vlc_object_t *obj)
{
    decoder_t *p_dec = (decoder_t *)obj;
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
//    void *hwaccel_context;

    msg_Dbg(obj, "<<< %s", __func__);

    /* do not flush buffers if codec hasn't been opened (theora/vorbis/VC1) */
    if( avcodec_is_open( ctx ) )
        avcodec_flush_buffers( ctx );

//    cc_Flush( &p_sys->cc );

    av_rpi_zc_uninit2(ctx);

//    hwaccel_context = ctx->hwaccel_context;
    avcodec_free_context( &ctx );

    cma_vcsm_exit(p_sys->vcsm_init_type);

//    if( p_sys->p_va )
//        vlc_va_Delete( p_sys->p_va, &hwaccel_context );

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


static enum PixelFormat ffmpeg_GetFormat( AVCodecContext *p_context,
                                          const enum PixelFormat *pi_fmt )
{
    decoder_t *p_dec = p_context->opaque;
    decoder_sys_t *p_sys = p_dec->p_sys;
    video_format_t fmt;

    /* Enumerate available formats */
    enum PixelFormat swfmt = avcodec_default_get_format(p_context, pi_fmt);
    bool can_hwaccel = false;

    for (size_t i = 0; pi_fmt[i] != AV_PIX_FMT_NONE; i++)
    {
        const AVPixFmtDescriptor *dsc = av_pix_fmt_desc_get(pi_fmt[i]);
        if (dsc == NULL)
            continue;
        bool hwaccel = (dsc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;

        msg_Dbg( p_dec, "available %sware decoder output format %d (%s)",
                 hwaccel ? "hard" : "soft", pi_fmt[i], dsc->name );
        if (hwaccel)
            can_hwaccel = true;
    }

    /* If the format did not actually change (e.g. seeking), try to reuse the
     * existing output format, and if present, hardware acceleration back-end.
     * This avoids resetting the pipeline downstream. This also avoids
     * needlessly probing for hardware acceleration support. */
    if (p_sys->pix_fmt != AV_PIX_FMT_NONE
     && lavc_GetVideoFormat(p_dec, &fmt, p_context, p_sys->pix_fmt, swfmt) == 0
     && fmt.i_width == p_dec->fmt_out.video.i_width
     && fmt.i_height == p_dec->fmt_out.video.i_height
     && p_context->profile == p_sys->profile
     && p_context->level <= p_sys->level)
    {
        for (size_t i = 0; pi_fmt[i] != AV_PIX_FMT_NONE; i++)
            if (pi_fmt[i] == p_sys->pix_fmt)
            {
                msg_Dbg(p_dec, "reusing decoder output format %d", pi_fmt[i]);
                return p_sys->pix_fmt;
            }
    }

//    if (p_sys->p_va != NULL)
//    {
//        msg_Err(p_dec, "existing hardware acceleration cannot be reused");
//        vlc_va_Delete(p_sys->p_va, &p_context->hwaccel_context);
//        p_sys->p_va = NULL;
//    }

    p_sys->profile = p_context->profile;
    p_sys->level = p_context->level;

    if (!can_hwaccel)
        return swfmt;

#if (LIBAVCODEC_VERSION_MICRO >= 100) \
  && (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 83, 101))
    if (p_context->active_thread_type)
    {
        msg_Warn(p_dec, "thread type %d: disabling hardware acceleration",
                 p_context->active_thread_type);
        return swfmt;
    }
#endif

    wait_mt(p_sys);

    static const enum PixelFormat hwfmts[] =
    {
#ifdef _WIN32
#if LIBAVUTIL_VERSION_CHECK(54, 13, 1, 24, 100)
        AV_PIX_FMT_D3D11VA_VLD,
#endif
        AV_PIX_FMT_DXVA2_VLD,
#endif
        AV_PIX_FMT_VAAPI_VLD,
#if (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(52, 4, 0))
        AV_PIX_FMT_VDPAU,
#endif
        AV_PIX_FMT_RPI4_10,
        AV_PIX_FMT_RPI4_8,
        AV_PIX_FMT_NONE,
    };

    for( size_t i = 0; hwfmts[i] != AV_PIX_FMT_NONE; i++ )
    {
        enum PixelFormat hwfmt = AV_PIX_FMT_NONE;
        for( size_t j = 0; hwfmt == AV_PIX_FMT_NONE && pi_fmt[j] != AV_PIX_FMT_NONE; j++ )
            if( hwfmts[i] == pi_fmt[j] )
                hwfmt = hwfmts[i];

        if( hwfmt == AV_PIX_FMT_NONE )
            continue;

        p_dec->fmt_out.video.i_chroma = get_hw_chroma(hwfmt, swfmt);
        if (p_dec->fmt_out.video.i_chroma == 0)
            continue; /* Unknown brand of hardware acceleration */
        if (p_context->width == 0 || p_context->height == 0)
        {   /* should never happen */
            msg_Err(p_dec, "unspecified video dimensions");
            continue;
        }
        const AVPixFmtDescriptor *dsc = av_pix_fmt_desc_get(hwfmt);
        msg_Dbg(p_dec, "trying format %s", dsc ? dsc->name : "unknown");
        if (lavc_UpdateVideoFormat(p_dec, p_context, hwfmt, swfmt))
            continue; /* Unsupported brand of hardware acceleration */
#if 0
        post_mt(p_sys);

        picture_t *test_pic = decoder_NewPicture(p_dec);
        assert(!test_pic || test_pic->format.i_chroma == p_dec->fmt_out.video.i_chroma);
        vlc_va_t *va = vlc_va_New(VLC_OBJECT(p_dec), p_context, hwfmt,
                                  &p_dec->fmt_in,
                                  test_pic ? test_pic->p_sys : NULL);
        if (test_pic)
            picture_Release(test_pic);
        if (va == NULL)
        {
            wait_mt(p_sys);
            continue; /* Unsupported codec profile or such */
        }

        if (va->description != NULL)
            msg_Info(p_dec, "Using %s for hardware decoding", va->description);
        p_sys->p_va = va;
#endif

        p_sys->pix_fmt = hwfmt;
        p_context->draw_horiz_band = NULL;
        return hwfmt;
    }

    post_mt(p_sys);
    /* Fallback to default behaviour */
    p_sys->pix_fmt = swfmt;
    return swfmt;
}

/*****************************************************************************
 * InitVideo: initialize the video decoder
 *****************************************************************************
 * the ffmpeg codec will be opened, some memory allocated. The vout is not yet
 * opened (done after the first decoded frame).
 *****************************************************************************/

/*****************************************************************************
 * ffmpeg_OpenCodec:
 *****************************************************************************/

static int MmalAvcodecOpenDecoder( vlc_object_t *obj )
{
    decoder_t *p_dec = (decoder_t *)obj;
    const AVCodec *p_codec;
    const vcsm_init_type_t vcsm_type = cma_vcsm_init();
    const int vcsm_size =
        vcsm_type == VCSM_INIT_LEGACY ? hw_mmal_get_gpu_mem() : 512 << 20;

#if 1
    {
        char buf1[5], buf2[5], buf2a[5];
        char buf3[5], buf4[5];
        uint32_t in_fcc = 0;
        msg_Dbg(p_dec, "%s: <<< (%s/%s)[%s] %dx%d -> (%s/%s) %dx%d [%s/%d]", __func__,
                str_fourcc(buf1, p_dec->fmt_in.i_codec),
                str_fourcc(buf2, p_dec->fmt_in.video.i_chroma),
                str_fourcc(buf2a, in_fcc),
                p_dec->fmt_in.video.i_width, p_dec->fmt_in.video.i_height,
                str_fourcc(buf3, p_dec->fmt_out.i_codec),
                str_fourcc(buf4, p_dec->fmt_out.video.i_chroma),
                p_dec->fmt_out.video.i_width, p_dec->fmt_out.video.i_height,
                cma_vcsm_init_str(vcsm_type), vcsm_size);
    }
#endif

    if( vcsm_type == VCSM_INIT_NONE )
        return VLC_EGENERIC;

    if( (p_dec->fmt_in.i_codec != VLC_CODEC_HEVC &&
         (vcsm_type == VCSM_INIT_CMA || vcsm_size < (96 << 20))) ||
        (p_dec->fmt_in.i_codec == VLC_CODEC_HEVC &&
         vcsm_size < (128 << 20)))
    {
        cma_vcsm_exit(vcsm_type);
        return VLC_EGENERIC;
    }

    AVCodecContext *p_context = ffmpeg_AllocContext( p_dec, &p_codec );
    if( p_context == NULL )
    {
        cma_vcsm_exit(vcsm_type);
        return VLC_EGENERIC;
    }

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
    p_sys->vcsm_init_type = vcsm_type;
//    p_sys->p_va = NULL;

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

    p_context->get_format = ffmpeg_GetFormat;
    /* Always use our get_buffer wrapper so we can calculate the
     * PTS correctly */
//    p_context->get_buffer2 = lavc_GetFrame;
//    p_context->opaque = p_dec;

    int i_thread_count = var_InheritInteger( p_dec, "avcodec-threads" );
    // 6 Threads for everything except Pi4 HEVC where 3 should do
    if( i_thread_count <= 0 )
        i_thread_count = (p_dec->fmt_in.i_codec == VLC_CODEC_HEVC && rpi_is_model_pi4()) ? 3 : 6;
    msg_Dbg( p_dec, "allowing %d thread(s) for decoding", i_thread_count );
    p_context->thread_count = i_thread_count;
    p_context->thread_safe_callbacks = true;

    p_context->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;

    if( p_context->thread_type & FF_THREAD_FRAME )
        p_dec->i_extra_picture_buffers = 2 * p_context->thread_count;

    /* ***** misc init ***** */
    date_Init(&p_sys->pts, 1, 30001);
    date_Set(&p_sys->pts, VLC_TS_INVALID);
    p_sys->b_first_frame = true;
    p_sys->i_late_frames = 0;
    p_sys->b_from_preroll = false;

    /* Set output properties */
    if( GetVlcChroma( &p_dec->fmt_out.video, p_context->pix_fmt ) != VLC_SUCCESS )
    {
        /* we are doomed. but not really, because most codecs set their pix_fmt later on */
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

    if ((p_sys->cma_pool = cma_buf_pool_new(5, 5, false)) == NULL)
    {
        msg_Err(p_dec, "CMA pool alloc failure");
        goto fail;
    }

    /* ***** init this codec with special data ***** */
    ffmpeg_InitCodec( p_dec );

    /* ***** Open the codec ***** */
    if( OpenVideoCodec( p_dec ) < 0 )
    {
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



vlc_module_begin()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_shortname(N_("MMAL avcodec"))
    set_description(N_("MMAL buffered avcodec "))
    set_capability("video decoder", 80)
    add_shortcut("mmal_avcodec")
    set_callbacks(MmalAvcodecOpenDecoder, MmalAvcodecCloseDecoder)
vlc_module_end()

