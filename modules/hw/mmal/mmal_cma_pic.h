#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include "mmal_cma_int.h"

static inline int
cma_buf_fd(const cma_buf_t *const cb)
{
    return cb->fd;
}

static inline void *
cma_buf_addr(const cma_buf_t *const cb)
{
    return cb->mmap;
}

#define CTX_BUFS_MAX 4
struct MMAL_BUFFER_HEADER_T;

typedef struct pic_ctx_mmal_s {
    picture_context_t cmn;  // PARENT: Common els at start

    cma_buf_t * cb;

    unsigned int buf_count;
    struct MMAL_BUFFER_HEADER_T * bufs[CTX_BUFS_MAX];

} pic_ctx_mmal_t;

static inline bool
is_cma_buf_pic_chroma(const uint32_t chroma)
{
    return chroma == VLC_CODEC_MMAL_ZC_RGB32 ||
        chroma == VLC_CODEC_MMAL_ZC_SAND8 ||
        chroma == VLC_CODEC_MMAL_ZC_SAND10 ||
        chroma == VLC_CODEC_MMAL_ZC_SAND30 ||
        chroma == VLC_CODEC_MMAL_ZC_I420;
}

// Returns a pointer to the cma_buf attached to the pic
// Just a pointer - doesn't add a ref
static inline cma_buf_t *
cma_buf_pic_get(picture_t * const pic)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic->context;
    return !is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx  == NULL ? 0 : ctx->cb;
}


