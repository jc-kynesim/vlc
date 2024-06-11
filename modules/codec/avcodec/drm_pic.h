#include <vlc_fourcc.h>
#include <vlc_picture.h>

struct picture_t;
struct AVFrame;
struct AVBufferRef;
struct AVDRMFrameDescriptor;

typedef struct drm_prime_video_sys_s {
    picture_context_t cmn;  // PARENT: Common els at start

    struct AVBufferRef * buf;
    const struct AVDRMFrameDescriptor * desc;
    struct AVBufferRef * hw_frames_ctx;
} drm_prime_video_sys_t;

static inline const struct AVDRMFrameDescriptor *
drm_prime_get_desc(picture_t *pic)
{
    drm_prime_video_sys_t * const pctx = (drm_prime_video_sys_t *)pic->context;

    return !pctx ? NULL : pctx->desc;
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

int drm_prime_attach_buf_to_pic(struct picture_t *pic, struct AVFrame *frame);

