#ifndef VLC_MMAL_MMAL_CMA_H_
#define VLC_MMAL_MMAL_CMA_H_


struct cma_pool_fixed_s;
typedef struct cma_pool_fixed_s cma_pool_fixed_t;

typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);
typedef void cma_pool_on_delete_fn(void * v);

void cma_pool_fixed_unref(cma_pool_fixed_t * const p);
void cma_pool_fixed_ref(cma_pool_fixed_t * const p);
void * cma_pool_fixed_get(cma_pool_fixed_t * const p, const size_t req_el_size, const bool in_flight);
void cma_pool_fixed_put(cma_pool_fixed_t * const p, void * v, const size_t el_size, const bool was_in_flight);
void cma_pool_fixed_inc_in_flight(cma_pool_fixed_t * const p);
void cma_pool_fixed_dec_in_flight(cma_pool_fixed_t * const p);
void cma_pool_fixed_cancel(cma_pool_fixed_t * const p);
void cma_pool_fixed_uncancel(cma_pool_fixed_t * const p);
void cma_pool_fixed_kill(cma_pool_fixed_t * const p);
int cma_pool_fixed_resize(cma_pool_fixed_t * const p,
                          const unsigned int new_pool_size, const int new_flight_size);
cma_pool_fixed_t * cma_pool_fixed_new(const unsigned int pool_size,
                   const int flight_size,
                   void * const alloc_v,
                   cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn,
                   cma_pool_on_delete_fn * const on_delete_fn,
                   const char * const name);


struct cma_buf_s;
typedef struct cma_buf_s cma_buf_t;

void cma_buf_in_flight(cma_buf_t * const cb);
void cma_buf_end_flight(cma_buf_t * const cb);
unsigned int cma_buf_vcsm_handle(const cma_buf_t * const cb);
size_t cma_buf_size(const cma_buf_t * const cb);
int cma_buf_add_context2(cma_buf_t *const cb, picture_context_t * const ctx2);
unsigned int cma_buf_vc_handle(const cma_buf_t *const cb);
int cma_buf_fd(const cma_buf_t *const cb);
void * cma_buf_addr(const cma_buf_t *const cb);
unsigned int cma_buf_vc_addr(const cma_buf_t *const cb);
picture_context_t * cma_buf_context2(const cma_buf_t *const cb);

void cma_buf_unref(cma_buf_t * const cb);
cma_buf_t * cma_buf_ref(cma_buf_t * const cb);

struct cma_buf_pool_s;
typedef struct cma_buf_pool_s cma_buf_pool_t;

cma_buf_t * cma_buf_pool_alloc_buf(cma_buf_pool_t * const p, const size_t size);
void cma_buf_pool_cancel(cma_buf_pool_t * const cbp);
void cma_buf_pool_uncancel(cma_buf_pool_t * const cbp);
void cma_buf_pool_delete(cma_buf_pool_t * const p);
int cma_buf_pool_resize(cma_buf_pool_t * const cbp,
                          const unsigned int new_pool_size, const int new_flight_size);
cma_buf_pool_t * cma_buf_pool_new(const unsigned int pool_size, const unsigned int flight_size,
                                  const bool all_in_flight, const char * const name);

static inline void cma_buf_pool_deletez(cma_buf_pool_t ** const pp)
{
    cma_buf_pool_t * const p = *pp;
    if (p != NULL) {
        *pp = NULL;
        cma_buf_pool_delete(p);
    }
}

#endif // VLC_MMAL_MMAL_CMA_H_
