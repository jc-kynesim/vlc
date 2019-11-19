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
#include "mmal_picture.h"

#include <assert.h>

#define TRACE_ALL 0

//-----------------------------------------------------------------------------
//
// Generic pool functions
// Knows nothing about pool entries

typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);

#if TRACE_ALL
static atomic_int pool_seq;
#endif

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

    int in_flight;
    vlc_cond_t flight_cond;

    void * alloc_v;
    cma_pool_alloc_fn * el_alloc_fn;
    cma_pool_free_fn * el_free_fn;
    cma_pool_on_delete_fn * on_delete_fn;

    const char * name;
#if TRACE_ALL
    int seq;
#endif
};

static void free_pool(const cma_pool_fixed_t * const p, void ** pool, unsigned int n, size_t el_size)
{
    assert(pool != NULL);

    while (pool[n] != NULL)
    {
        p->el_free_fn(p->alloc_v, pool[n], el_size);
        pool[n] = NULL;
        n = n + 1 < p->pool_size ? n + 1 : 0;
    }
    free(pool);
}

// Just kill this - no checks
static void cma_pool_fixed_delete(cma_pool_fixed_t * const p)
{
    cma_pool_on_delete_fn *const on_delete_fn = p->on_delete_fn;
    void *const v = p->alloc_v;

    if (p->pool != NULL)
        free_pool(p, p->pool, p->n_in, p->el_size);

    if (p->name != NULL)
        free((void *)p->name);  // Discard const

    vlc_mutex_destroy(&p->lock);
    free(p);

    // Inform our container that we are dead (if it cares)
    if (on_delete_fn)
        on_delete_fn(v);
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

void * cma_pool_fixed_get(cma_pool_fixed_t * const p, const size_t req_el_size, const bool is_in_flight)
{
    void * v = NULL;
    const bool inc_flight = is_in_flight && req_el_size != 0;

    vlc_mutex_lock(&p->lock);

    do
    {
        if (req_el_size != p->el_size)
        {
            void ** const deadpool = p->pool;
            const size_t dead_size = p->el_size;
            const unsigned int dead_n = p->n_in;

            p->pool = NULL;
            p->n_in = 0;
            p->n_out = 0;
            p->el_size = req_el_size;

            if (deadpool != NULL)
            {
                vlc_mutex_unlock(&p->lock);
                // Do the free old op outside the mutex in case the free is slow
                free_pool(p, deadpool, dead_n, dead_size);
                vlc_mutex_lock(&p->lock);
                continue;
            }
        }

        if (req_el_size == 0)
            break;

        if (p->pool != NULL)
        {
            v = p->pool[p->n_in];
            if (v != NULL)
            {
                p->pool[p->n_in] = NULL;
                p->n_in = p->n_in + 1 < p->pool_size ? p->n_in + 1 : 0;
                break;
            }
        }

        if (p->in_flight <= 0)
            break;

        vlc_cond_wait(&p->flight_cond, &p->lock);

    } while (1);

    if (inc_flight)
        ++p->in_flight;

    vlc_mutex_unlock(&p->lock);

    if (v == NULL && req_el_size != 0)
        v = p->el_alloc_fn(p->alloc_v, req_el_size);

    // Tag ref
    if (v != NULL)
        cma_pool_fixed_ref(p);
    // Remove flight if we set it and error
    else if (inc_flight)
        cma_pool_fixed_dec_in_flight(p);

    return v;
}

void cma_pool_fixed_put(cma_pool_fixed_t * const p, void * v, const size_t el_size, const bool was_in_flight)
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

    if (was_in_flight)
        --p->in_flight;

    vlc_mutex_unlock(&p->lock);

    vlc_cond_signal(&p->flight_cond);

    if (v != NULL)
        p->el_free_fn(p->alloc_v, v, el_size);

    cma_pool_fixed_unref(p);
}

void cma_pool_fixed_inc_in_flight(cma_pool_fixed_t * const p)
{
    vlc_mutex_lock(&p->lock);
    ++p->in_flight;
    vlc_mutex_unlock(&p->lock);
}

void cma_pool_fixed_dec_in_flight(cma_pool_fixed_t * const p)
{
    vlc_mutex_lock(&p->lock);
    if (--p->in_flight == 0)
        vlc_cond_signal(&p->flight_cond);
    vlc_mutex_unlock(&p->lock);
}

// Purge pool & unref
void cma_pool_fixed_kill(cma_pool_fixed_t * const p)
{
    if (p == NULL)
        return;

    // This flush is not strictly needed but it reclaims what memory we can reclaim asap
    cma_pool_fixed_get(p, 0, false);
    cma_pool_fixed_unref(p);
}

// Create a new pool
cma_pool_fixed_t*
cma_pool_fixed_new(const unsigned int pool_size,
                   const int flight_size,
                   void * const alloc_v,
                   cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn,
                   cma_pool_on_delete_fn * const on_delete_fn,
                   const char * const name)
{
    cma_pool_fixed_t* const p = calloc(1, sizeof(cma_pool_fixed_t));
    if (p == NULL)
        return NULL;

    atomic_store(&p->ref_count, 1);
    vlc_mutex_init(&p->lock);

    p->pool_size = pool_size;
    p->in_flight = -flight_size;

    p->alloc_v = alloc_v;
    p->el_alloc_fn = alloc_fn;
    p->el_free_fn = free_fn;
    p->on_delete_fn = on_delete_fn;
    p->name = name == NULL ? NULL : strdup(name);
#if TRACE_ALL
    p->seq = atomic_fetch_add(&pool_seq, 1);
#endif

    return p;
}

// ---------------------------------------------------------------------------
//
// CMA buffer functions - uses cma_pool_fixed for pooling

struct cma_buf_pool_s {
    cma_pool_fixed_t * pool;
    vcsm_init_type_t init_type;

    bool all_in_flight;
#if TRACE_ALL
    size_t alloc_n;
    size_t alloc_size;
#endif
};

typedef struct cma_buf_s {
    atomic_int ref_count;
    cma_buf_pool_t * cbp;
    bool in_flight;
    size_t size;
    unsigned int vcsm_h;   // VCSM handle from initial alloc
    unsigned int vc_h;     // VC handle for ZC mmal buffers
    unsigned int vc_addr;  // VC addr - unused by us but wanted by FFmpeg
    int fd;                // dmabuf handle for GL
    void * mmap;           // ARM mapped address
    picture_context_t *ctx2;
} cma_buf_t;

static void cma_pool_delete(cma_buf_t * const cb)
{
    assert(atomic_load(&cb->ref_count) == 0);
#if TRACE_ALL
    cb->cbp->alloc_size -= cb->size;
    --cb->cbp->alloc_n;
    fprintf(stderr, "%s[%d:%s]: N=%d, Total=%d\n", __func__, cb->cbp->pool->seq, cb->cbp->pool->name, cb->cbp->alloc_n, cb->cbp->alloc_size);
#endif

    if (cb->ctx2 != NULL)
        cb->ctx2->destroy(cb->ctx2);

    if (cb->mmap != MAP_FAILED)
    {
        if (cb->cbp->init_type == VCSM_INIT_CMA)
            munmap(cb->mmap, cb->size);
        else
            vcsm_unlock_hdl(cb->vcsm_h);
    }
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
    cma_buf_pool_t * const cbp = v;

    cma_buf_t * const cb = malloc(sizeof(cma_buf_t));
    if (cb == NULL)
        return NULL;

    *cb = (cma_buf_t){
        .ref_count = ATOMIC_VAR_INIT(0),
        .cbp = cbp,
        .in_flight = 0,
        .size = size,
        .vcsm_h = 0,
        .vc_h = 0,
        .fd = -1,
        .mmap = MAP_FAILED,
        .ctx2 = NULL
    };
#if TRACE_ALL
    cb->cbp->alloc_size += cb->size;
    ++cb->cbp->alloc_n;
    fprintf(stderr, "%s[%d:%s]: N=%d, Total=%d\n", __func__, cbp->pool->seq, cbp->pool->name, cbp->alloc_n, cbp->alloc_size);
#endif

    // 0x80 is magic value to force full ARM-side mapping - otherwise
    // cache requests can cause kernel crashes
    if ((cb->vcsm_h = vcsm_malloc_cache(size, VCSM_CACHE_TYPE_HOST | 0x80, "VLC frame")) == 0)
    {
#if TRACE_ALL
        fprintf(stderr, "vcsm_malloc_cache fail\n");
#endif
        goto fail;
    }

    if ((cb->vc_h = vcsm_vc_hdl_from_hdl(cb->vcsm_h)) == 0)
    {
#if TRACE_ALL
        fprintf(stderr, "vcsm_vc_hdl_from_hdl fail\n");
#endif
        goto fail;
    }

    if (cbp->init_type == VCSM_INIT_CMA)
    {
        if ((cb->fd = vcsm_export_dmabuf(cb->vcsm_h)) == -1)
        {
#if TRACE_ALL
            fprintf(stderr, "vcsm_export_dmabuf fail\n");
#endif
            goto fail;
        }

        if ((cb->mmap = mmap(NULL, cb->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, cb->fd, 0)) == MAP_FAILED)
            goto fail;
    }
    else
    {
        void * arm_addr;
        if ((arm_addr = vcsm_lock(cb->vcsm_h)) == NULL)
        {
#if TRACE_ALL
            fprintf(stderr, "vcsm_lock fail\n");
#endif
            goto fail;
        }
        cb->mmap = arm_addr;
    }

    cb->vc_addr = vcsm_vc_addr_from_hdl(cb->vcsm_h);

    return cb;

fail:
    cma_pool_delete(cb);
    return NULL;
}

// Pool has died - safe now to exit vcsm
static void cma_buf_pool_on_delete_cb(void * v)
{
    cma_buf_pool_t * const cbp = v;

    cma_vcsm_exit(cbp->init_type);
    free(cbp);
}

// User finished with pool
void cma_buf_pool_delete(cma_buf_pool_t * const cbp)
{
    if (cbp == NULL)
        return;

    if (cbp->pool != NULL)
    {
        // We will call cma_buf_pool_on_delete_cb when the pool finally dies
        // (might be now) which will free up our env.
        cma_pool_fixed_kill(cbp->pool);
    }
    else
    {
        // Had no pool for some reason (error) but must still finish cleanup
        cma_buf_pool_on_delete_cb(cbp);
    }
}

cma_buf_pool_t * cma_buf_pool_new(const unsigned int pool_size, const unsigned int flight_size, const bool all_in_flight, const char * const name)
{
    vcsm_init_type_t const init_type = cma_vcsm_init();
    if (init_type == VCSM_INIT_NONE)
        return NULL;

    cma_buf_pool_t * const cbp = calloc(1, sizeof(cma_buf_pool_t));
    if (cbp == NULL)
        return NULL;

    cbp->init_type = init_type;
    cbp->all_in_flight = all_in_flight;

    if ((cbp->pool = cma_pool_fixed_new(pool_size, flight_size, cbp, cma_pool_alloc_cb, cma_pool_free_cb, cma_buf_pool_on_delete_cb, name)) == NULL)
        goto fail;
    return cbp;

fail:
    cma_buf_pool_delete(cbp);
    return NULL;
}


void cma_buf_in_flight(cma_buf_t * const cb)
{
    if (!cb->cbp->all_in_flight)
    {
        assert(!cb->in_flight);
        cb->in_flight = true;
        cma_pool_fixed_inc_in_flight(cb->cbp->pool);
    }
}

void cma_buf_end_flight(cma_buf_t * const cb)
{
    if (cb != NULL && !cb->cbp->all_in_flight && cb->in_flight)
    {
        cb->in_flight = false;
        cma_pool_fixed_dec_in_flight(cb->cbp->pool);
    }
}


// Return vcsm handle
unsigned int cma_buf_vcsm_handle(const cma_buf_t * const cb)
{
    return cb->vcsm_h;
}

size_t cma_buf_size(const cma_buf_t * const cb)
{
    return cb->size;
}

int cma_buf_add_context2(cma_buf_t *const cb, picture_context_t * const ctx2)
{
    if (cb->ctx2 != NULL)
        return VLC_EGENERIC;

    cb->ctx2 = ctx2;
    return VLC_SUCCESS;
}

unsigned int cma_buf_vc_handle(const cma_buf_t *const cb)
{
    return cb->vc_h;
}

int cma_buf_fd(const cma_buf_t *const cb)
{
    return cb->fd;
}

void * cma_buf_addr(const cma_buf_t *const cb)
{
    return cb->mmap;
}

unsigned int cma_buf_vc_addr(const cma_buf_t *const cb)
{
    return cb->vc_addr;
}


picture_context_t * cma_buf_context2(const cma_buf_t *const cb)
{
    return cb->ctx2;
}


void cma_buf_unref(cma_buf_t * const cb)
{
    if (cb == NULL)
        return;
    if (atomic_fetch_sub(&cb->ref_count, 1) <= 1)
    {
        const bool was_in_flight = cb->in_flight;
        cb->in_flight = false;
        cma_pool_fixed_put(cb->cbp->pool, cb, cb->size, was_in_flight);
    }
}

cma_buf_t * cma_buf_ref(cma_buf_t * const cb)
{
    if (cb == NULL)
        return NULL;
    atomic_fetch_add(&cb->ref_count, 1);
    return cb;
}

cma_buf_t * cma_buf_pool_alloc_buf(cma_buf_pool_t * const cbp, const size_t size)
{
    cma_buf_t *const cb = cma_pool_fixed_get(cbp->pool, size, cbp->all_in_flight);

    if (cb == NULL)
        return NULL;

    cb->in_flight = cbp->all_in_flight;
    // When 1st allocated or retrieved from the pool the block will have a
    // ref count of 0 so ref here
    return cma_buf_ref(cb);
}

