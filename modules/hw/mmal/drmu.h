#ifndef _DRMU_DRMU_H
#define _DRMU_DRMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct drmu_bo_s;
typedef struct drmu_bo_s drmu_bo_t;
struct drmu_bo_env_s;
typedef struct drmu_bo_env_s drmu_bo_env_t;

struct drmu_fb_s;
typedef struct drmu_fb_s drmu_fb_t;

struct drmu_pool_s;
typedef struct drmu_pool_s drmu_pool_t;

struct drmu_crtc_s;
typedef struct drmu_crtc_s drmu_crtc_t;

struct drmu_env_s;
typedef struct drmu_env_s drmu_env_t;

typedef struct drmu_rect_s {
    int32_t x, y;
    uint32_t w, h;
} drmu_rect_t;

typedef struct drmu_ufrac_s {
    unsigned int num;
    unsigned int den;
} drmu_ufrac_t;

drmu_ufrac_t drmu_ufrac_reduce(drmu_ufrac_t x);

struct drm_mode_create_dumb;

static inline int
drmu_rect_rescale_1(int x, int mul, int div)
{
    return div == 0 ? x * mul : (x * mul + div/2) / div;
}

static inline drmu_rect_t
drmu_rect_rescale(const drmu_rect_t s, const drmu_rect_t mul, const drmu_rect_t div)
{
    return (drmu_rect_t){
        .x = drmu_rect_rescale_1(s.x - div.x, mul.w, div.w) + mul.x,
        .y = drmu_rect_rescale_1(s.y - div.y, mul.h, div.h) + mul.y,
        .w = drmu_rect_rescale_1(s.w,         mul.w, div.w),
        .h = drmu_rect_rescale_1(s.h,         mul.h, div.h)
    };
}

static inline drmu_rect_t
drmu_rect_add_xy(const drmu_rect_t a, const drmu_rect_t b)
{
    return (drmu_rect_t){
        .x = a.x + b.x,
        .y = a.y + b.y,
        .w = a.w,
        .h = a.h
    };
}

static inline drmu_rect_t
drmu_rect_wh(const unsigned int w, const unsigned int h)
{
    return (drmu_rect_t){
        .w = w,
        .h = h
    };
}



void drmu_bo_unref(drmu_bo_t ** const ppbo);
drmu_bo_t * drmu_bo_ref(drmu_bo_t * const bo);
drmu_bo_t * drmu_bo_new_fd(drmu_env_t *const du, const int fd);
drmu_bo_t * drmu_bo_new_dumb(drmu_env_t *const du, struct drm_mode_create_dumb * const d);
void drmu_bo_env_uninit(drmu_bo_env_t * const boe);
void drmu_bo_env_init(drmu_bo_env_t * boe);

void drmu_fb_unref(drmu_fb_t ** const ppdfb);
drmu_fb_t * drmu_fb_ref(drmu_fb_t * const dfb);

// fb

// Called pre delete.
// Zero returned means continue delete.
// Non-zero means stop delete - fb will have zero refs so will probably want a new ref
//   before next use
typedef int (* drmu_fb_pre_delete_fn)(struct drmu_fb_s * dfb, void * v);
typedef void (* drmu_fb_on_delete_fn)(struct drmu_fb_s * dfb, void * v);

void drmu_fb_pre_delete_set(drmu_fb_t *const dfb, drmu_fb_pre_delete_fn fn, void * v);
void drmu_fb_pre_delete_unset(drmu_fb_t *const dfb);
unsigned int drmu_fb_pixel_bits(const drmu_fb_t * const dfb);
drmu_fb_t * drmu_fb_new_dumb(drmu_env_t * const du, uint32_t w, uint32_t h, const uint32_t format);
drmu_fb_t * drmu_fb_realloc_dumb(drmu_env_t * const du, drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format);

// fb pool

void drmu_pool_unref(drmu_pool_t ** const pppool);
drmu_pool_t * drmu_pool_ref(drmu_pool_t * const pool);
drmu_pool_t * drmu_pool_new(drmu_env_t * const du, unsigned int total_fbs_max);
drmu_fb_t * drmu_pool_fb_new_dumb(drmu_pool_t * const pool, uint32_t w, uint32_t h, const uint32_t format);
void drmu_pool_delete(drmu_pool_t ** const pppool);

// drmu_atomic

struct drmu_atomic_s;
typedef struct drmu_atomic_s drmu_atomic_t;

void drmu_atomic_dump(const drmu_atomic_t * const da);
drmu_env_t * drmu_atomic_env(const drmu_atomic_t * const da);
void drmu_atomic_unref(drmu_atomic_t ** const ppda);
drmu_atomic_t * drmu_atomic_ref(drmu_atomic_t * const da);
drmu_atomic_t * drmu_atomic_new(drmu_env_t * const du);
int drmu_atomic_merge(drmu_atomic_t * const a, drmu_atomic_t ** const ppb);
int drmu_atomic_commit(drmu_atomic_t * const da, uint32_t flags);

typedef void (* drmu_prop_del_fn)(void * v);
typedef void (* drmu_prop_ref_fn)(void * v);

int drmu_atomic_add_prop_generic(drmu_atomic_t * const da,
        const uint32_t obj_id, const uint32_t prop_id, const uint64_t value,
        const drmu_prop_ref_fn ref_fn, const drmu_prop_del_fn del_fn, void * const v);

#ifdef __cplusplus
}
#endif

#endif

