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
    vlc_mutex_t lock;
    unsigned int n_in;
    unsigned int n_out;
    unsigned int pool_size;
    void ** pool;

    void * alloc_v;
    cma_pool_alloc_fn * el_alloc_fn;
    cma_pool_free_fn * el_free_fn;

} cma_pool_fixed_t;


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
    if (deadpool != NULL)
    {
        while (deadpool[dead_n] != NULL)
        {
            p->el_alloc_free(p->alloc_v, deadpool[dead_n], dead_size);
            deadpool[dead_n] = NULL;
            dead_n = dead_size + 1 < p->pool_size ? dead_n + 1 : 0;
        }
        free(deadpool);
    }

    if (v == NULL && req_el_size != 0)
        v = p->el_alloc_fn(p->alloc_v, req_el_size);

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
}





typedef struct hw_mmal_cma_env_s
{
    Display * dpy;
    int drm_fd;
} hw_mmal_cma_env_t;


cma_buf_t * hw_mmal_cma_buf_new(hw_mmal_cma_env_t * cenv, size_t size)
{
}

hw_mmal_cma_env_delete(hw_mmal_cma_env_t * const cenv)
{
}

hw_mmal_cma_env_t * hw_mmal_cma_env_new()
{
}

