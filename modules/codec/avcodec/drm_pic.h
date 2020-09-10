#include "vlc_fourcc.h"

struct picture_t;
struct AVFrame;
struct AVBufferRef;
struct decoder_t;

typedef struct drm_prime_video_sys_s {
    struct AVBufferRef * buf;
} drm_prime_video_sys_t;

int drm_prime_attach_buf_to_pic(struct decoder_t *dec, struct picture_t *pic, struct AVFrame *frame);

