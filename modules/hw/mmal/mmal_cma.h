typedef enum vcsm_init_type_e {
    VCSM_INIT_NONE = 0,
    VCSM_INIT_LEGACY,
    VCSM_INIT_CMA
} vcsm_init_type_t;

vcsm_init_type_t cma_vcsm_init(void);
void cma_vcsm_exit(const vcsm_init_type_t init_mode);
const char * cma_vcsm_init_str(const vcsm_init_type_t init_mode);

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

void cma_buf_pool_delete(cma_pool_fixed_t * const p);
cma_pool_fixed_t * cma_buf_pool_new(void);

int cma_buf_pic_attach(cma_pool_fixed_t * const p, picture_t * const pic, const size_t size);
int cma_buf_pic_add_context2(picture_t *const pic, picture_context_t * const ctx2);
unsigned int cma_buf_pic_vc_handle(const picture_t * const pic);
int cma_buf_pic_fd(const picture_t * const pic);
void * cma_buf_pic_addr(const picture_t * const pic);
picture_context_t * cma_buf_pic_context2(const picture_t * const pic);
struct cma_pic_context_s;
struct cma_pic_context_s * cma_buf_pic_context_ref(const picture_t * const pic);
void cma_buf_pic_context_unref(struct cma_pic_context_s * const ctx);

#include <vlc_fourcc.h>

static inline bool is_cma_buf_pic_chroma(const uint32_t chroma)
{
    return chroma == VLC_CODEC_MMAL_ZC_RGB32 || chroma == VLC_CODEC_MMAL_ZC_SAND8 || chroma == VLC_CODEC_MMAL_ZC_I420;
}

static inline void cma_buf_pool_deletez(cma_pool_fixed_t ** const pp)
{
    cma_pool_fixed_t * const p = *pp;
    if (p != NULL) {
        *pp = NULL;
        cma_buf_pool_delete(p);
    }
}


