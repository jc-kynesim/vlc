#ifndef VLC_HW_MMAL_MMAL_CMA_INT_H_
#define VLC_HW_MMAL_MMAL_CMA_INT_H_

#include <stdatomic.h>

typedef void * cma_pool_alloc_fn(void * usr_v, size_t size);
typedef void cma_pool_free_fn(void * usr_v, void * buffer_v, size_t size);
typedef void cma_pool_on_delete_fn(void * usr_v);
typedef void cma_pool_on_put_fn(void * buffer_v);

// Pool structure
// Ref count is held by pool owner and pool els that have been got
// Els in the pool do not count towards its ref count
struct cma_pool_fixed_s;
typedef struct cma_pool_fixed_s cma_pool_fixed_t;

cma_pool_fixed_t*
cma_pool_fixed_new(const unsigned int pool_size,
                   const int flight_size,
                   void * const alloc_v,
                   cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn,
                   cma_pool_on_put_fn * const on_put_fn, cma_pool_on_delete_fn * const on_delete_fn,
                   const char * const name);

typedef enum cma_buf_type_e {
    CMA_BUF_TYPE_NONE = 0,
    CMA_BUF_TYPE_CMA,
    CMA_BUF_TYPE_VCSM,
    CMA_BUF_TYPE_DRMPRIME,
} cma_buf_type_t;

struct cma_buf_pool_s {
    cma_pool_fixed_t * pool;
    cma_buf_type_t buf_type;

    bool all_in_flight;
    size_t alloc_n;
    size_t alloc_size;
};

typedef struct cma_buf_s {
    atomic_int ref_count;
    cma_buf_type_t type;
    struct cma_buf_pool_s * cbp;
    bool in_flight;
    size_t size;
    unsigned int vcsm_h;   // VCSM handle from initial alloc
    unsigned int vc_h;     // VC handle for ZC mmal buffers
    unsigned int vc_addr;  // VC addr - unused by us but wanted by FFmpeg
    int fd;                // dmabuf handle for GL
    void * mmap;           // ARM mapped address
    picture_context_t *ctx2;
} cma_buf_t;

void cma_pool_delete(cma_buf_t * const cb);

#endif

