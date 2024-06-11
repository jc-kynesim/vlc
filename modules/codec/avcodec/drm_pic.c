#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_picture.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>

#include "drm_pic.h"

static picture_context_t *
drm_prime_picture_context_new(AVBufferRef * const buf, const void * data, AVBufferRef * const hw_frames_ctx);

static void
drm_prime_pic_ctx_destroy(struct picture_context_t * ctx)
{
    drm_prime_video_sys_t * const pctx = (drm_prime_video_sys_t *)ctx;
    av_buffer_unref(&pctx->buf);
    av_buffer_unref(&pctx->hw_frames_ctx);
    free(pctx);
}

static picture_context_t *
drm_prime_pic_ctx_copy(struct picture_context_t * src)
{
    drm_prime_video_sys_t * const pctx = (drm_prime_video_sys_t *)src;

    // We could ref count this structure but easier to just create a new one
    // (which will ref the buf)
    return drm_prime_picture_context_new(pctx->buf, pctx->desc, pctx->hw_frames_ctx);
}

static picture_context_t *
drm_prime_picture_context_new(AVBufferRef * const buf, const void * data, AVBufferRef * const hw_frames_ctx)
{
    drm_prime_video_sys_t * const pctx = calloc(1, sizeof(*pctx));

    if (pctx == NULL)
        return NULL;

    pctx->cmn.copy = drm_prime_pic_ctx_copy;
    pctx->cmn.destroy = drm_prime_pic_ctx_destroy;

    pctx->buf = av_buffer_ref(buf);
    pctx->desc = data;
    pctx->hw_frames_ctx = !hw_frames_ctx ? NULL : av_buffer_ref(hw_frames_ctx);
    return &pctx->cmn;
}

int drm_prime_attach_buf_to_pic(picture_t *pic, AVFrame *frame)
{
    if (pic->context)
        return VLC_EGENERIC;

    pic->context = drm_prime_picture_context_new(frame->buf[0], frame->data[0], frame->hw_frames_ctx);
    return VLC_SUCCESS;
}



