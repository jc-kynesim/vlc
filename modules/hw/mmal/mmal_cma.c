#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <interface/vcsm/user-vcsm.h>

#include <vlc_common.h>
#include <vlc_picture.h>

#include "mmal_cma.h"

#include <assert.h>


typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);

// Pool structure
// Ref count is held by pool owner and pool els that have been got
// Els in the pool do not count towards its ref count
struct cma_pool_fixed_s
{
    atomic_int ref_count;

    vlc_mutex_t lock;
    unsigned int n_in;
    unsigned int n_out;
    unsigned int pool_size;
    size_t el_size;
    void ** pool;

    void * alloc_v;
    cma_pool_alloc_fn * el_alloc_fn;
    cma_pool_free_fn * el_free_fn;
};

typedef struct cma_buf_s {
    size_t size;
    unsigned int vcsm_h;   // VCSM handle from initial alloc
    unsigned int vc_h;     // VC handle for ZC mmal buffers
    int fd;                // dmabuf handle for GL
    void * mmap;           // ARM mapped address
    picture_context_t *ctx2;
} cma_buf_t;

static int free_pool(const cma_pool_fixed_t * const p, void ** pool, unsigned int n, size_t el_size)
{
    int i = 0;
    assert(pool != NULL);

    while (pool[n] != NULL)
    {
        p->el_free_fn(p->alloc_v, pool[n], el_size);
        pool[n] = NULL;
        n = n + 1 < p->pool_size ? n + 1 : 0;
        ++i;
    }
    free(pool);
    return i;
}

// Just kill this - no checks
static void cma_pool_fixed_delete(cma_pool_fixed_t * const p)
{
    if (p->pool != NULL)
        free_pool(p, p->pool, p->n_in, p->el_size);

    vlc_mutex_destroy(&p->lock);
    free(p);
}

void cma_pool_fixed_unref(cma_pool_fixed_t * const p)
{
    if (atomic_fetch_sub(&p->ref_count, 1) <= 1)
        cma_pool_fixed_delete(p);
}

void cma_pool_fixed_ref(cma_pool_fixed_t * const p)
{
    atomic_fetch_add(&p->ref_count, 1);
}

void * cma_pool_fixed_get(cma_pool_fixed_t * const p, const size_t req_el_size)
{
    void * v = NULL;
    void ** deadpool = NULL;
    size_t dead_size = 0;
    unsigned int dead_n = 0;

    vlc_mutex_lock(&p->lock);

    if (req_el_size != p->el_size)
    {
        deadpool = p->pool;
        dead_n = p->n_in;
        dead_size = p->el_size;

        p->pool = NULL;
        p->n_in = 0;
        p->n_out = 0;
        p->el_size = req_el_size;
    }
    else if (p->pool != NULL)
    {
        v = p->pool[p->n_in];
        if (v != NULL)
        {
            p->pool[p->n_in] = NULL;
            p->n_in = p->n_in + 1 < p->pool_size ? p->n_in + 1 : 0;
        }
    }

    vlc_mutex_unlock(&p->lock);

    // Do the free old op outside the mutex in case the free is slow
    if (deadpool != NULL)
        free_pool(p, deadpool, dead_n, dead_size);

    if (v == NULL && req_el_size != 0)
        v = p->el_alloc_fn(p->alloc_v, req_el_size);

    // Tag ref
    if (v != NULL)
        cma_pool_fixed_ref(p);

    return v;
}

void cma_pool_fixed_put(cma_pool_fixed_t * const p, void * v, const size_t el_size)
{
    vlc_mutex_lock(&p->lock);

    if (el_size == p->el_size && (p->pool == NULL || p->pool[p->n_out] == NULL))
    {
        if (p->pool == NULL)
            p->pool = calloc(p->pool_size, sizeof(void*));

        p->pool[p->n_out] = v;
        p->n_out = p->n_out + 1 < p->pool_size ? p->n_out + 1 : 0;
        v = NULL;
    }

    vlc_mutex_unlock(&p->lock);

    if (v != NULL)
        p->el_free_fn(p->alloc_v, v, el_size);

    cma_pool_fixed_unref(p);
}

// Purge pool & unref
void cma_pool_fixed_kill(cma_pool_fixed_t * const p)
{
    // This flush is not strictly needed but it reclaims what memory we can reclaim asap
    cma_pool_fixed_get(p, 0);
    cma_pool_fixed_unref(p);
}

cma_pool_fixed_t*
cma_pool_fixed_new(const unsigned int pool_size, void * const alloc_v,
                   cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn)
{
    cma_pool_fixed_t* const p = calloc(1, sizeof(cma_pool_fixed_t));
    if (p == NULL)
        return NULL;

    atomic_store(&p->ref_count, 1);
    vlc_mutex_init(&p->lock);

    p->pool_size = pool_size;

    p->alloc_v = alloc_v;
    p->el_alloc_fn = alloc_fn;
    p->el_free_fn = free_fn;

    return p;
}


static void cma_pool_delete(cma_buf_t * const cb)
{
    if (cb->ctx2 != NULL)
        cb->ctx2->destroy(cb->ctx2);

    if (cb->mmap != MAP_FAILED)
        munmap(cb->mmap, cb->size);
    if (cb->fd != -1)
        close(cb->fd);
    if (cb->vcsm_h != 0)
        vcsm_free(cb->vcsm_h);
    free(cb);
}

static void cma_pool_free_cb(void * v, void * el, size_t size)
{
    VLC_UNUSED(v);
    VLC_UNUSED(size);

    cma_pool_delete(el);
}

static void * cma_pool_alloc_cb(void * v, size_t size)
{
    VLC_UNUSED(v);

    cma_buf_t * const cb = malloc(sizeof(cma_buf_t));
    if (cb == NULL)
        return NULL;

    *cb = (cma_buf_t){
        .size = size,
        .vcsm_h = 0,
        .vc_h = 0,
        .fd = -1,
        .mmap = MAP_FAILED,
        .ctx2 = NULL
    };

    if ((cb->vcsm_h = vcsm_malloc_cache(size, VCSM_CACHE_TYPE_HOST, (char*)"VLC frame")) == 0)
        goto fail;

    if ((cb->vc_h = vcsm_vc_hdl_from_hdl(cb->vcsm_h)) == 0)
        goto fail;

    if ((cb->fd = vcsm_export_dmabuf(cb->vcsm_h)) == -1)
        goto fail;

    if ((cb->mmap = mmap(NULL, cb->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, cb->fd, 0)) == MAP_FAILED)
        goto fail;

    return cb;

fail:
    cma_pool_delete(cb);
    return NULL;
}

void cma_buf_pool_delete(cma_pool_fixed_t * const p)
{
    assert(p != NULL);

    cma_pool_fixed_kill(p);
}

cma_pool_fixed_t * cma_buf_pool_new(void)
{
    return cma_pool_fixed_new(5, NULL, cma_pool_alloc_cb, cma_pool_free_cb);
}


typedef struct cma_pic_context_s {
    picture_context_t cmn;

    atomic_int ref_count;
    cma_pool_fixed_t * p;
    cma_buf_t * cb;
} cma_pic_context_t;


static void cma_buf_pic_ctx_ref(cma_pic_context_t * const ctx)
{
    atomic_fetch_add(&ctx->ref_count, 1);
}

static void cma_buf_pic_ctx_unref(cma_pic_context_t * const ctx)
{
    if (atomic_fetch_sub(&ctx->ref_count, 1) > 0)
        return;

    if (ctx->cb != NULL)
        cma_pool_fixed_put(ctx->p, ctx->cb, ctx->cb->size);

    free(ctx);
}

static picture_context_t * cma_buf_pic_ctx_copy(picture_context_t * pic_ctx)
{
    cma_buf_pic_ctx_ref((cma_pic_context_t *)pic_ctx);
    return pic_ctx;
}

static void cma_buf_pic_ctx_destroy(picture_context_t * pic_ctx)
{
    cma_buf_pic_ctx_unref((cma_pic_context_t *)pic_ctx);
}

int cma_buf_pic_attach(cma_pool_fixed_t * const p, picture_t * const pic, const size_t size)
{
    if (!is_cma_buf_pic_chroma(pic->format.i_chroma))
        return VLC_EGENERIC;
    if (pic->context != NULL)
        return VLC_EBADVAR;

    cma_buf_t * const cb = cma_pool_fixed_get(p, size);
    if (cb == NULL)
        return VLC_ENOMEM;

    cma_pic_context_t * const ctx = malloc(sizeof(cma_pic_context_t));
    if (ctx == NULL)
        goto fail;

    *ctx = (cma_pic_context_t){
        .cmn = {
            .destroy = cma_buf_pic_ctx_destroy,
            .copy = cma_buf_pic_ctx_copy
        },
        .ref_count = 0,
        .p = p,
        .cb = cb
    };

    pic->context = &ctx->cmn;
    return VLC_SUCCESS;

fail:
    cma_pool_fixed_put(p, cb, size);
    return VLC_EGENERIC;
}

int cma_buf_pic_add_context2(picture_t *const pic, picture_context_t * const ctx2)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;
    if (!is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx == NULL || ctx->cb == NULL || ctx->cb->ctx2 != NULL)
        return VLC_EGENERIC;

    ctx->cb->ctx2 = ctx2;
    return VLC_SUCCESS;
}

unsigned int cma_buf_pic_vc_handle(const picture_t * const pic)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;
    return !is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx  == NULL ? 0 : ctx->cb == NULL ? 0 : ctx->cb->vc_h;
}

int cma_buf_pic_fd(const picture_t * const pic)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;
    return !is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx == NULL ? -1 : ctx->cb == NULL ? -1 : ctx->cb->fd;
}

void * cma_buf_pic_addr(const picture_t * const pic)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;
    return !is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx == NULL ? NULL : ctx->cb == NULL ? NULL : ctx->cb->mmap;
}

picture_context_t * cma_buf_pic_context2(const picture_t * const pic)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;
    return !is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx == NULL ? NULL : ctx->cb == NULL ? NULL :  ctx->cb->ctx2;
}

cma_pic_context_t * cma_buf_pic_context_ref(const picture_t * const pic)
{
    cma_pic_context_t *const ctx = (cma_pic_context_t *)pic->context;

    if (!is_cma_buf_pic_chroma(pic->format.i_chroma) || ctx == NULL || ctx->cb == NULL)
        return NULL;

    cma_buf_pic_ctx_ref(ctx);
    return ctx;
}

void cma_buf_pic_context_unref(cma_pic_context_t * const ctx)
{
    if (ctx != NULL)
        cma_buf_pic_ctx_unref(ctx);
}


//----------------------------------------------------------------------------

vcsm_init_type_t cma_vcsm_init(void)
{
    if (vcsm_init_ex(1, -1) == 0) {
        return VCSM_INIT_CMA;
    }
    else if (vcsm_init_ex(0, -1) == 0) {
        return VCSM_INIT_LEGACY;
    }
    return VCSM_INIT_NONE;
}

void cma_vcsm_exit(const vcsm_init_type_t init_mode)
{
    if (init_mode != VCSM_INIT_NONE)
        vcsm_exit();
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

