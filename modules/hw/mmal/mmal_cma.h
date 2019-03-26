struct cma_pool_fixed_s;
typedef struct cma_pool_fixed_s cma_pool_fixed_t;

typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);

void cma_pool_fixed_unref(cma_pool_fixed_t * const p);
void cma_pool_fixed_ref(cma_pool_fixed_t * const p);
void * cma_pool_fixed_get(cma_pool_fixed_t * const p, const size_t req_el_size);
void cma_pool_fixed_put(cma_pool_fixed_t * const p, void * v, const size_t el_size);
void cma_pool_fixed_kill(cma_pool_fixed_t * const p);
cma_pool_fixed_t* cma_pool_fixed_new(const unsigned int pool_size, void * const alloc_v,
    cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn);

typedef struct cma_buf_s {
    size_t size;
    unsigned int vcsm_h;   // VCSM handle from initial alloc
    unsigned int vc_h;     // VC handle for ZC mmal buffers
    int fd;                // dmabuf handle for GL
    void * mmap;           // ARM mapped address
} cma_buf_t;

void cma_buf_pool_delete(cma_pool_fixed_t * const p);
cma_pool_fixed_t * cma_buf_pool_new(void);

int cma_buf_pic_attach(cma_pool_fixed_t * const p, picture_t * const pic, const size_t size);
unsigned int cma_buf_pic_vc_handle(const picture_t * const pic);
int cma_buf_pic_fd(const picture_t * const pic);
void * cma_buf_pic_addr(const picture_t * const pic);

#include <vlc_fourcc.h>

// ******* Lie - until we build a proper 4cc
#define VLC_CODEC_MMAL_ZC_RGB32 VLC_CODEC_MMAL_ZC_SAND10

static inline bool is_cma_buf_pic_chroma(const uint32_t chroma)
{
    return chroma == VLC_CODEC_MMAL_ZC_RGB32;
}

static inline void cma_buf_pool_deletez(cma_pool_fixed_t ** const pp)
{
    cma_pool_fixed_t * const p = *pp;
    if (p != NULL) {
        *pp = NULL;
        cma_buf_pool_delete(p);
    }
}


