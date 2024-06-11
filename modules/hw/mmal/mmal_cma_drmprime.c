#include <vlc_common.h>

#include "mmal_cma.h"
#include "mmal_cma_int.h"
#include "mmal_cma_drmprime.h"

#include <sys/mman.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <interface/vcsm/user-vcsm.h>

#define TRACE_ALL 0

typedef struct cma_drmprime_buf_s
{
    cma_buf_t cb;
    const AVDRMFrameDescriptor *desc;
    AVBufferRef * avbuf;
} cma_drmprime_buf_t;

static void drmprime_pool_free_cb(void * v, void * el, size_t size)
{
    VLC_UNUSED(v);
    VLC_UNUSED(size);

    cma_pool_delete(el);
}

static void * drmprime_pool_alloc_cb(void * v, size_t size)
{
    cma_buf_pool_t * const cbp = v;

    cma_drmprime_buf_t * const cdb = malloc(sizeof(cma_drmprime_buf_t));
    if (cdb == NULL)
        return NULL;

    *cdb = (cma_drmprime_buf_t){
        .cb = {
            .ref_count = ATOMIC_VAR_INIT(0),
            .cbp = cbp,
            .in_flight = 0,
            .size = size,
            .vcsm_h = 0,
            .vc_h = 0,
            .fd = -1,
            .mmap = MAP_FAILED,
            .ctx2 = NULL
        }
    };
#if TRACE_ALL
    cdb->cbp->alloc_size += cdb->size;
    ++cdb->cbp->alloc_n;
    fprintf(stderr, "%s[%d:%s]: N=%d, Total=%d\n", __func__, cbp->pool->seq, cbp->pool->name, cbp->alloc_n, cbp->alloc_size);
#endif

    return cdb;
}

// Buf being returned to pool
static void drmprime_buf_pool_on_put_cb(void * v)
{
    cma_drmprime_buf_t * const cdb = v;
    cdb->cb.fd = -1;
    if (cdb->cb.vcsm_h != 0)
        vcsm_free(cdb->cb.vcsm_h);
    cdb->cb.vcsm_h = 0;
    cdb->cb.vc_h = 0;
    cdb->cb.vc_addr = 0;
    av_buffer_unref(&cdb->avbuf);
    if (cdb->cb.ctx2) {
        cdb->cb.ctx2->destroy(cdb->cb.ctx2);
        cdb->cb.ctx2 = NULL;
    }
}

// Pool has died - safe now to exit vcsm
static void drmprime_buf_pool_on_delete_cb(void * v)
{
    cma_buf_pool_t * const cbp = v;
    free(cbp);
}

cma_buf_pool_t * cma_drmprime_pool_new(const unsigned int pool_size, const unsigned int flight_size, const bool all_in_flight, const char * const name)
{
    cma_buf_pool_t * const cbp = calloc(1, sizeof(cma_buf_pool_t));
    if (cbp == NULL)
        return NULL;

    cbp->buf_type = CMA_BUF_TYPE_DRMPRIME;
    cbp->all_in_flight = all_in_flight;

    if ((cbp->pool = cma_pool_fixed_new(pool_size, flight_size, cbp,
                                        drmprime_pool_alloc_cb, drmprime_pool_free_cb,
                                        drmprime_buf_pool_on_put_cb, drmprime_buf_pool_on_delete_cb,
                                        name)) == NULL)
        goto fail;
    return cbp;

fail:
    cma_buf_pool_delete(cbp);
    return NULL;
}

cma_buf_t * cma_drmprime_pool_alloc_buf(cma_buf_pool_t * const cbp, struct AVFrame * frame)
{
    const AVDRMFrameDescriptor* const desc = (AVDRMFrameDescriptor*)frame->data[0];
    cma_drmprime_buf_t *const cdb = (cma_drmprime_buf_t *)cma_buf_pool_alloc_buf(cbp, desc->objects[0].size);

    if (cdb == NULL)
        return NULL;

    cdb->cb.type = CMA_BUF_TYPE_DRMPRIME;
    cdb->cb.fd = desc->objects[0].fd;

    cdb->desc = desc;
    cdb->avbuf = av_buffer_ref(frame->buf[0]);
    return &cdb->cb;
}


