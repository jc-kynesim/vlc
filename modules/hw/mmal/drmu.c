#include "drmu_int.h"

#include <assert.h>
#include <sys/mman.h>


drmu_ufrac_t
drmu_ufrac_reduce(drmu_ufrac_t x)
{
    static const unsigned int primes[] = {2,3,5,7,11,13,17,19,23,0};
    const unsigned int * p;
    for (p = primes; *p != 0; ++p) {
        while (x.den % *p == 0 && x.num % *p ==0) {
            x.den /= *p;
            x.num /= *p;
        }
    }
    return x;
}


//----------------------------------------------------------------------------
//
// BO fns

static int
bo_close(drmu_env_t * const du, uint32_t * const ph)
{
    struct drm_gem_close gem_close = {.handle = *ph};

    if (gem_close.handle == 0)
        return 0;
    *ph = 0;

    return drmIoctl(du->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
}

// BOE lock expected
static void
bo_free_dumb(drmu_bo_t * const bo)
{
    if (bo->handle != 0) {
        drmu_env_t * const du = bo->du;
        struct drm_mode_destroy_dumb destroy_env = {.handle = bo->handle};

        if (drmIoctl(du->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_env) != 0)
            drmu_warn(du, "%s: Failed to destroy dumb handle %d", __func__, bo->handle);
    }
    free(bo);
}

static void
bo_free_fd(drmu_bo_t * const bo)
{
    if (bo->handle != 0) {
        drmu_env_t * const du = bo->du;
        drmu_bo_env_t * const boe = &du->boe;
        const uint32_t h = bo->handle;

        if (bo_close(du, &bo->handle) != 0)
            drmu_warn(du, "%s: Failed to close BO handle %d", __func__, h);
        if (bo->next != NULL)
            bo->next->prev = bo->prev;
        if (bo->prev != NULL)
            bo->prev->next = bo->next;
        else
            boe->fd_head = bo->next;
    }
    free(bo);
}


void
drmu_bo_unref(drmu_bo_t ** const ppbo)
{
    drmu_bo_t * const bo = *ppbo;

    if (bo == NULL)
        return;
    *ppbo = NULL;

    switch (bo->bo_type) {
        case BO_TYPE_FD:
        {
            drmu_bo_env_t * const boe = &bo->du->boe;

            pthread_mutex_lock(&boe->lock);
            if (atomic_fetch_sub(&bo->ref_count, 1) == 0)
                bo_free_fd(bo);
            pthread_mutex_unlock(&boe->lock);
            break;
        }
        case BO_TYPE_DUMB:
            if (atomic_fetch_sub(&bo->ref_count, 1) == 0)
                bo_free_dumb(bo);
            break;
        case BO_TYPE_NONE:
        default:
            free(bo);
            break;
    }
}


drmu_bo_t *
drmu_bo_ref(drmu_bo_t * const bo)
{
    if (bo != NULL)
        atomic_fetch_add(&bo->ref_count, 1);
    return bo;
}

static drmu_bo_t *
bo_alloc(drmu_env_t *const du, enum drmu_bo_type_e bo_type)
{
    drmu_bo_t * const bo = calloc(1, sizeof(*bo));
    if (bo == NULL) {
        drmu_err(du, "Failed to alloc BO");
        return NULL;
    }

    bo->du = du;
    bo->bo_type = bo_type;
    atomic_init(&bo->ref_count, 0);
    return bo;
}

drmu_bo_t *
drmu_bo_new_fd(drmu_env_t *const du, const int fd)
{
    drmu_bo_env_t * const boe = &du->boe;
    drmu_bo_t * bo = NULL;
    uint32_t h = 0;

    pthread_mutex_lock(&boe->lock);

    if (drmPrimeFDToHandle(du->fd, fd, &h) != 0) {
        drmu_err(du, "%s: Failed to convert fd %d to BO: %s", __func__, fd, strerror(errno));
        goto unlock;
    }

    bo = boe->fd_head;
    while (bo != NULL && bo->handle != h)
        bo = bo->next;

    if (bo != NULL) {
        drmu_bo_ref(bo);
    }
    else {
        if ((bo = bo_alloc(du, BO_TYPE_FD)) == NULL) {
            bo_close(du, &h);
        }
        else {
            bo->handle = h;

            if ((bo->next = boe->fd_head) != NULL)
                bo->next->prev = bo;
            boe->fd_head = bo;
        }
    }

unlock:
    pthread_mutex_unlock(&boe->lock);
    return bo;
}

// Updates the passed dumb structure with the results of creation
drmu_bo_t *
drmu_bo_new_dumb(drmu_env_t *const du, struct drm_mode_create_dumb * const d)
{
    drmu_bo_t *bo = bo_alloc(du, BO_TYPE_DUMB);

    if (bo == NULL)
        return NULL;

    if (drmIoctl(du->fd, DRM_IOCTL_MODE_CREATE_DUMB, d) != 0)
    {
        drmu_err(du, "%s: Create dumb %dx%dx%d failed: %s", __func__,
                 d->width, d->height, d->bpp, strerror(errno));
        drmu_bo_unref(&bo);  // After this point aux is bound to dfb and gets freed with it
        return NULL;
    }

    bo->handle = d->handle;
    return bo;
}

void
drmu_bo_env_uninit(drmu_bo_env_t * const boe)
{
    if (boe->fd_head != NULL)
        drmu_warn(boe->fd_head->du, "%s: fd chain not null", __func__);
    boe->fd_head = NULL;
    pthread_mutex_destroy(&boe->lock);
}

void
drmu_bo_env_init(drmu_bo_env_t * boe)
{
    boe->fd_head = NULL;
    pthread_mutex_init(&boe->lock, NULL);
}

//----------------------------------------------------------------------------
//
// FB fns

void
drmu_fb_int_free(drmu_fb_t * const dfb)
{
    drmu_env_t * const du = dfb->du;
    unsigned int i;

    if (dfb->pre_delete_fn && dfb->pre_delete_fn(dfb, dfb->pre_delete_v) != 0)
        return;

    if (dfb->handle != 0)
        drmModeRmFB(du->fd, dfb->handle);

    if (dfb->map_ptr != NULL && dfb->map_ptr != MAP_FAILED)
        munmap(dfb->map_ptr, dfb->map_size);

    for (i = 0; i != 4; ++i)
        drmu_bo_unref(dfb->bo_list + i);

    // Call on_delete last so we have stopped using anything that might be
    // freed by it
    if (dfb->on_delete_fn)
        dfb->on_delete_fn(dfb, dfb->on_delete_v);

    free(dfb);
}

void
drmu_fb_unref(drmu_fb_t ** const ppdfb)
{
    drmu_fb_t * const dfb = *ppdfb;

    if (dfb == NULL)
        return;
    *ppdfb = NULL;

    if (atomic_fetch_sub(&dfb->ref_count, 1) > 0)
        return;

    drmu_fb_int_free(dfb);
}

drmu_fb_t *
drmu_fb_ref(drmu_fb_t * const dfb)
{
    if (dfb != NULL)
        atomic_fetch_add(&dfb->ref_count, 1);
    return dfb;
}

// Beware: used by pool fns
void
drmu_fb_pre_delete_set(drmu_fb_t *const dfb, drmu_fb_pre_delete_fn fn, void * v)
{
    dfb->pre_delete_fn = fn;
    dfb->pre_delete_v  = v;
}

void
drmu_fb_pre_delete_unset(drmu_fb_t *const dfb)
{
    dfb->pre_delete_fn = (drmu_fb_pre_delete_fn)0;
    dfb->pre_delete_v  = NULL;
}

drmu_fb_t *
drmu_fb_int_alloc(drmu_env_t * const du)
{
    drmu_fb_t * const dfb = calloc(1, sizeof(*dfb));
    if (dfb == NULL)
        return NULL;

    dfb->du = du;
    return dfb;
}

// Bits per pixel on plane 0
unsigned int
drmu_fb_pixel_bits(const drmu_fb_t * const dfb)
{
    switch (dfb->format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_AYUV:
            return 32;
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_YVYU:
        case DRM_FORMAT_VYUY:
        case DRM_FORMAT_UYVY:
            return 16;
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_YUV420:
            return 8;
        default:
            break;
    }
    return 0;
}

// For allocation purposes given fb_pixel bits how tall
// does the frame have to be to fit all planes
static unsigned int
fb_total_height(const drmu_fb_t * const dfb, unsigned int h)
{
    switch (dfb->format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_YUV420:
            return h * 3 / 2;
        default:
            break;
    }
    return h;
}

static void
fb_pitches_set(drmu_fb_t * const dfb)
{
    memset(dfb->offsets, 0, sizeof(dfb->offsets));
    memset(dfb->pitches, 0, sizeof(dfb->pitches));

    switch (dfb->format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_AYUV:
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_YVYU:
        case DRM_FORMAT_VYUY:
        case DRM_FORMAT_UYVY:
            dfb->pitches[0] = dfb->map_pitch;
            break;
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
            dfb->pitches[0] = dfb->map_pitch;
            dfb->pitches[1] = dfb->map_pitch;
            dfb->offsets[1] = dfb->pitches[0] * dfb->height;
            break;
        case DRM_FORMAT_YUV420:
            dfb->pitches[0] = dfb->map_pitch;
            dfb->pitches[1] = dfb->map_pitch / 2;
            dfb->pitches[2] = dfb->map_pitch / 2;
            dfb->offsets[1] = dfb->pitches[0] * dfb->height;
            dfb->offsets[2] = dfb->offsets[1] + dfb->pitches[1] * dfb->height / 2;
            break;
        default:
            break;
    }
}

drmu_fb_t *
drmu_fb_new_dumb(drmu_env_t * const du, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_fb_t * const dfb = drmu_fb_int_alloc(du);
    uint32_t bpp;

    if (dfb == NULL) {
        drmu_err(du, "%s: Alloc failure", __func__);
        return NULL;
    }
    dfb->width = (w + 63) & ~63;
    dfb->height = (h + 63) & ~63;
    dfb->cropped = drmu_rect_wh(w, h);
    dfb->format = format;

    if ((bpp = drmu_fb_pixel_bits(dfb)) == 0) {
        drmu_err(du, "%s: Unexpected format %#x", __func__, format);
        goto fail;
    }

    {
        struct drm_mode_create_dumb dumb = {
            .height = fb_total_height(dfb, dfb->height),
            .width = dfb->width,
            .bpp = bpp
        };
        if ((dfb->bo_list[0] = drmu_bo_new_dumb(du, &dumb)) == NULL)
            goto fail;

        dfb->map_pitch = dumb.pitch;
        dfb->map_size = (size_t)dumb.size;
    }

    {
        struct drm_mode_map_dumb map_dumb = {
            .handle = dfb->bo_list[0]->handle
        };
        if (drmIoctl(du->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) != 0)
        {
            drmu_err(du, "%s: map dumb failed: %s", __func__, strerror(errno));
            goto fail;
        }

        if ((dfb->map_ptr = mmap(NULL, dfb->map_size,
                                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                 du->fd, (off_t)map_dumb.offset)) == MAP_FAILED) {
            drmu_err(du, "%s: mmap failed (size=%zd, fd=%d, off=%zd): %s", __func__,
                     dfb->map_size, du->fd, (size_t)map_dumb.offset, strerror(errno));
            goto fail;
        }
    }

    {
        uint32_t bo_handles[4] = { dfb->bo_list[0]->handle };

        fb_pitches_set(dfb);

        if (dfb->pitches[1] != 0)
            bo_handles[1] = bo_handles[0];
        if (dfb->pitches[2] != 0)
            bo_handles[2] = bo_handles[0];

        if (drmModeAddFB2WithModifiers(du->fd,
                                       dfb->width, dfb->height, dfb->format,
                                       bo_handles, dfb->pitches, dfb->offsets, NULL,
                                       &dfb->handle, 0) != 0)
        {
            drmu_err(du, "%s: drmModeAddFB2WithModifiers failed: %s\n", __func__, strerror(errno));
            goto fail;
        }
    }

    drmu_debug(du, "Create dumb %p %dx%d / %dx%d size: %zd", dfb, dfb->width, dfb->height, dfb->cropped.w, dfb->cropped.h, dfb->map_size);

    return dfb;

fail:
    drmu_fb_int_free(dfb);
    return NULL;
}

static int
fb_try_reuse(drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format)
{
    if (w > dfb->width || h > dfb->height || format != dfb->format)
        return 0;

    dfb->cropped = drmu_rect_wh(w, h);
    return 1;
}

drmu_fb_t *
drmu_fb_realloc_dumb(drmu_env_t * const du, drmu_fb_t * dfb, uint32_t w, uint32_t h, const uint32_t format)
{
    if (dfb == NULL)
        return drmu_fb_new_dumb(du, w, h, format);

    if (fb_try_reuse(dfb, w, h, format))
        return dfb;

    drmu_fb_unref(&dfb);
    return drmu_fb_new_dumb(du, w, h, format);
}

// Pool fns

static void
fb_list_add_tail(drmu_fb_list_t * const fbl, drmu_fb_t * const dfb)
{
    assert(dfb->prev == NULL && dfb->next == NULL);

    if (fbl->tail == NULL)
        fbl->head = dfb;
    else
        fbl->tail->next = dfb;
    dfb->prev = fbl->tail;
    fbl->tail = dfb;
}

static drmu_fb_t *
fb_list_extract(drmu_fb_list_t * const fbl, drmu_fb_t * const dfb)
{
    if (dfb == NULL)
        return NULL;

    if (dfb->prev == NULL)
        fbl->head = dfb->next;
    else
        dfb->prev->next = dfb->next;

    if (dfb->next == NULL)
        fbl->tail = dfb->prev;
    else
        dfb->next->prev = dfb->prev;

    dfb->next = NULL;
    dfb->prev = NULL;
    return dfb;
}

static drmu_fb_t *
fb_list_extract_head(drmu_fb_list_t * const fbl)
{
    return fb_list_extract(fbl, fbl->head);
}

static drmu_fb_t *
fb_list_peek_head(drmu_fb_list_t * const fbl)
{
    return fbl->head;
}

static bool
fb_list_is_empty(drmu_fb_list_t * const fbl)
{
    return fbl->head == NULL;
}

static void
pool_free_pool(drmu_pool_t * const pool)
{
    drmu_fb_t * dfb;
    while ((dfb = fb_list_extract_head(&pool->free_fbs)) != NULL)
        drmu_fb_unref(&dfb);
}

static void
pool_free(drmu_pool_t * const pool)
{
    pool_free_pool(pool);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

void
drmu_pool_unref(drmu_pool_t ** const pppool)
{
    drmu_pool_t * const pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    if (atomic_fetch_sub(&pool->ref_count, 1) != 0)
        return;

    pool_free(pool);
}

drmu_pool_t *
drmu_pool_ref(drmu_pool_t * const pool)
{
    atomic_fetch_add(&pool->ref_count, 1);
    return pool;
}

drmu_pool_t *
drmu_pool_new(drmu_env_t * const du, unsigned int total_fbs_max)
{
    drmu_pool_t * const pool = calloc(1, sizeof(*pool));

    if (pool == NULL) {
        drmu_err(du, "Failed pool env alloc");
        return NULL;
    }

    pool->du = du;
    pool->fb_max = total_fbs_max;
    pthread_mutex_init(&pool->lock, NULL);

    return pool;
}

static int
pool_fb_pre_delete_cb(drmu_fb_t * dfb, void * v)
{
    drmu_pool_t * pool = v;

    // Ensure we cannot end up in a delete loop
    drmu_fb_pre_delete_unset(dfb);

    // If dead set then might as well delete now
    // It should all work without this shortcut but this reclaims
    // storage quicker
    if (pool->dead) {
        drmu_pool_unref(&pool);
        return 0;
    }

    drmu_fb_ref(dfb);  // Restore ref

    pthread_mutex_lock(&pool->lock);
    fb_list_add_tail(&pool->free_fbs, dfb);
    pthread_mutex_unlock(&pool->lock);

    // May cause suicide & recursion on fb delete, but that should be OK as
    // the 1 we return here should cause simple exit of fb delete
    drmu_pool_unref(&pool);
    return 1;  // Stop delete
}

drmu_fb_t *
drmu_pool_fb_new_dumb(drmu_pool_t * const pool, uint32_t w, uint32_t h, const uint32_t format)
{
    drmu_env_t * const du = pool->du;
    drmu_fb_t * dfb;

    pthread_mutex_lock(&pool->lock);

    dfb = fb_list_peek_head(&pool->free_fbs);
    while (dfb != NULL) {
        if (fb_try_reuse(dfb, w, h, format)) {
            fb_list_extract(&pool->free_fbs, dfb);
            break;
        }
        dfb = dfb->next;
    }

    if (dfb == NULL) {
        if (pool->fb_count >= pool->fb_max && !fb_list_is_empty(&pool->free_fbs)) {
            --pool->fb_count;
            dfb = fb_list_extract_head(&pool->free_fbs);
        }
        ++pool->fb_count;
        pthread_mutex_unlock(&pool->lock);

        drmu_fb_unref(&dfb);  // Will free the dfb as pre-delete CB will be unset
        if ((dfb = drmu_fb_realloc_dumb(du, NULL, w, h, format)) == NULL) {
            --pool->fb_count;  // ??? lock
            return NULL;
        }
    }
    else {
        pthread_mutex_unlock(&pool->lock);
    }

    drmu_fb_pre_delete_set(dfb, pool_fb_pre_delete_cb, pool);
    drmu_pool_ref(pool);
    return dfb;
}

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
void
drmu_pool_delete(drmu_pool_t ** const pppool)
{
    drmu_pool_t * pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    pool->dead = 1;
    pool_free_pool(pool);

    drmu_pool_unref(&pool);
}


