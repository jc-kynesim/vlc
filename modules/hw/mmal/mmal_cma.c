#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <interface/vcsm/user-vcsm.h>


#include <vlc_common.h>
#include <vlc_picture.h>

#include "mmal_gl.h"

#include <vlc_xlib.h>
#include "../../video_output/opengl/converter.h"
#include <vlc_vout_window.h>

#include "mmal_picture.h"

#include <assert.h>


typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);

typedef struct cma_pool_fixed_s
{
    atomic_int ref_count;

    vlc_mutex_t lock;
    unsigned int n_in;
    unsigned int n_out;
    unsigned int pool_size;
    void ** pool;

    void * alloc_v;
    cma_pool_alloc_fn * el_alloc_fn;
    cma_pool_free_fn * el_free_fn;

} cma_pool_fixed_t;

static int free_pool(const cma_pool_fixed_t * const p, void ** pool, unsigned int n, size_t el_size)
{
    int i = 0;

    while (pool[n] != NULL)
    {
        p->el_alloc_free(p->alloc_v, pool[n], el_size);
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
    void * v;
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
        v = NULL;
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
    // We cannot run out of refs here
    if (deadpool != NULL)
        atomic_fetch_sub(&p->ref_count, free_pool(p, deadpool, dead_n, dead_size));

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
        p->el_alloc_free(p->alloc_v, v, el_size);

    cma_pool_unref(p);
}

// Purge pool & unref
void cma_pool_fixed_kill(cma_pool_fixed_t * const p)
{
    cma_pool_fixed_get(p, 0);
    cma_pool_fixed_unref(p);
}

cma_pool_fixed_t*
cma_pool_fixed_new(void * const alloc_v,
                   cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn)
{
    cma_pool_fixed_t* const p = calloc(1, sizeof(cma_pool_fixed_t));
    if (p == NULL)
        return NULL;

    atomic_store(&p->ref_count, 1);
    vlc_mutex_init(&p->lock);

    p->alloc_v = alloc_v;
    p->el_alloc_fn = alloc_fn;
    p->el_free_fn = free_fn;

    return p;
}

