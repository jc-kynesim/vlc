#include "vlc_fourcc.h"

struct picture_t;
struct AVFrame;
struct AVBufferRef;
struct AVDRMFrameDescriptor;
struct decoder_t;

typedef struct drm_prime_video_sys_s {
    struct AVBufferRef * buf;
    const struct AVDRMFrameDescriptor * desc;
} drm_prime_video_sys_t;

static inline const struct AVDRMFrameDescriptor *
drm_prime_get_desc(picture_t *pic)
{
    const drm_prime_video_sys_t * const vsys =
        vlc_video_context_GetPrivate(pic->context->vctx, VLC_VIDEO_CONTEXT_DRM_PRIME);
    if (!vsys)
        return NULL;
    return vsys->desc;
}

static inline char safechar(unsigned int x)
{
    const unsigned int c = x & 0xff;
    return c > ' ' && c < 0x7f ? (char)c : '.';
}

static inline const char *
str_fourcc(char buf[5], const uint32_t fcc)
{
    buf[0] = safechar(fcc);
    buf[1] = safechar(fcc >> 8);
    buf[2] = safechar(fcc >> 16);
    buf[3] = safechar(fcc >> 24);
    buf[4] = 0;
    return buf;
}

#define fourcc2str(fcc) \
    str_fourcc((char[5]){0}, fcc)

int drm_prime_attach_buf_to_pic(struct decoder_t *dec, struct picture_t *pic, struct AVFrame *frame);

