#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_picture_pool.h>

#include <libavcodec/avcodec.h>

#include "avcodec.h"
#include "va.h"
#include "drm_pic.h"

typedef struct drm_prime_video_sys_s {
    AVBufferRef * buf;
} drm_prime_video_sys_t;

static void drm_prime_video_sys_destroy(void * v)
{
    // Just free contents - container is freed by caller
    drm_prime_video_sys_t * const vsys = v;
    av_buffer_unref(&vsys->buf);
}

static void drm_prime_pic_ctx_destroy(struct picture_context_t * ctx)
{
    // vctx is released by caller
    free(ctx);
}

static struct picture_context_t * drm_prime_pic_ctx_copy(struct picture_context_t * src)
{
    picture_context_t * const dst = malloc(sizeof (*dst));
    if (!dst)
        return NULL;

    *dst = (picture_context_t){
        .destroy = src->destroy,
        .copy = src->copy,
        .vctx = vlc_video_context_Hold(src->vctx)
    };
    return dst;
}

static vlc_video_context *
drm_prime_video_context_new(AVBufferRef * buf)
{
    static const struct vlc_video_context_operations ops = {
        .destroy = drm_prime_video_sys_destroy
    };
    vlc_video_context * const vctx =
        vlc_video_context_Create(NULL, VLC_VIDEO_CONTEXT_DRM_PRIME, sizeof(drm_prime_video_sys_t), &ops);
    if (!vctx)
        return NULL;
    drm_prime_video_sys_t * const vsys = vlc_video_context_GetPrivate(vctx, VLC_VIDEO_CONTEXT_DRM_PRIME);
    vsys->buf = av_buffer_ref(buf);
    return vctx;
}

static picture_context_t *
drm_prime_picture_context_new(AVBufferRef * buf)
{
    picture_context_t * const pctx = malloc(sizeof (*pctx));
    *pctx = (picture_context_t){
        .destroy = drm_prime_pic_ctx_destroy,
        .copy = drm_prime_pic_ctx_copy,
        .vctx = drm_prime_video_context_new(buf)
    };
    return pctx;
}

int drm_prime_attach_buf_to_pic(struct decoder_t *dec, picture_t *pic, AVFrame *frame)
{
    if (pic->context)
        return VLC_EGENERIC;
    pic->context = drm_prime_picture_context_new(frame->buf[0]);
    return VLC_SUCCESS;
}


