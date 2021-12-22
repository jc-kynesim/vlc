#include "drmu.h"

#include <stdatomic.h>
#include <pthread.h>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vlc_common.h>

#define drmu_err_log(...)       msg_Err(__VA_ARGS__)
#define drmu_warn_log(...)      msg_Warn(__VA_ARGS__)
#define drmu_info_log(...)      msg_Info(__VA_ARGS__)
#define drmu_debug_log(...)     msg_Dbg(__VA_ARGS__)

#define drmu_err(_du, ...)      drmu_err_log((_du)->log, __VA_ARGS__)
#define drmu_warn(_du, ...)     drmu_warn_log((_du)->log, __VA_ARGS__)
#define drmu_info(_du, ...)     drmu_info_log((_du)->log, __VA_ARGS__)
#define drmu_debug(_du, ...)    drmu_debug_log((_du)->log, __VA_ARGS__)

typedef struct drmu_props_s {
    struct drmu_env_s * du;
    unsigned int prop_count;
    drmModePropertyPtr * props;
} drmu_props_t;

typedef struct drmu_prop_enum_s {
    uint32_t id;
    uint32_t flags;
    unsigned int n;
    const struct drm_mode_property_enum * enums;
    char name[DRM_PROP_NAME_LEN];
} drmu_prop_enum_t;

typedef struct drmu_prop_range_s {
    uint32_t id;
    uint32_t flags;
    uint64_t range[2];
    char name[DRM_PROP_NAME_LEN];
} drmu_prop_range_t;

typedef struct drmu_blob_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init
    struct drmu_env_s * du;
    uint32_t blob_id;
} drmu_blob_t;

enum drmu_bo_type_e {
    BO_TYPE_NONE = 0,
    BO_TYPE_FD,
    BO_TYPE_DUMB
};

// BO handles come in 2 very distinct types: DUMB and FD
// They need very different alloc & free but BO usage is the same for both
// so it is better to have a single type.
typedef struct drmu_bo_s {
    // Arguably could be non-atomic for FD as then it is always protected by mutex
    atomic_int ref_count;
    struct drmu_env_s * du;
    enum drmu_bo_type_e bo_type;
    uint32_t handle;

    // FD only els - FD BOs need to be tracked globally
    struct drmu_bo_s * next;
    struct drmu_bo_s * prev;
} drmu_bo_t;

typedef struct drmu_bo_env_s {
    pthread_mutex_t lock;
    drmu_bo_t * fd_head;
} drmu_bo_env_t;


typedef enum drmu_isset_e {
    DRMU_ISSET_UNSET = 0,  // Thing unset
    DRMU_ISSET_NULL,       // Thing is empty
    DRMU_ISSET_SET,        // Thing has valid data
} drmu_isset_t;

typedef struct drmu_fb_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init
    struct drmu_fb_s * prev;
    struct drmu_fb_s * next;

    struct drmu_env_s * du;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    drmu_rect_t cropped;
    unsigned int handle;

    void * map_ptr;
    size_t map_size;
    size_t map_pitch;

    uint32_t pitches[4];
    uint32_t offsets[4];
    drmu_bo_t * bo_list[4];

    const char * color_encoding; // Assumed to be constant strings that don't need freeing
    const char * color_range;

    // Do not set colorspace or metadata if not the "master" plane
    const char * colorspace;
    drmu_isset_t hdr_metadata_isset;
    struct hdr_output_metadata hdr_metadata;

    void * pre_delete_v;
    drmu_fb_pre_delete_fn pre_delete_fn;

    void * on_delete_v;
    drmu_fb_on_delete_fn on_delete_fn;
} drmu_fb_t;

typedef struct drmu_fb_list_s {
    drmu_fb_t * head;
    drmu_fb_t * tail;
} drmu_fb_list_t;

typedef struct drmu_pool_s {
    atomic_int ref_count;  // 0 == 1 ref for ease of init

    struct drmu_env_s * du;

    pthread_mutex_t lock;
    int dead;

    unsigned int seq;  // debug

    unsigned int fb_count;
    unsigned int fb_max;

    drmu_fb_list_t free_fbs;
} drmu_pool_t;

typedef struct drmu_plane_s {
    struct drmu_env_s * du;
    struct drmu_crtc_s * dc;    // NULL if not in use
    const drmModePlane * plane;

    struct {
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t crtc_h;
        uint32_t crtc_w;
        uint32_t crtc_x;
        uint32_t crtc_y;
        uint32_t src_h;
        uint32_t src_w;
        uint32_t src_x;
        uint32_t src_y;
        drmu_prop_enum_t * color_encoding;
        drmu_prop_enum_t * color_range;
    } pid;

} drmu_plane_t;

typedef int (* drmu_mode_score_fn)(void * v, const drmModeModeInfo * mode);

typedef struct drmu_crtc_s {
    struct drmu_env_s * du;
//    drmModeCrtcPtr crtc;
    drmModeEncoderPtr enc;
    drmModeConnectorPtr con;
    int crtc_idx;
    bool hi_bpc_ok;
    drmu_ufrac_t sar;
    drmu_ufrac_t par;

    struct drm_mode_crtc crtc;

    struct {
        // crtc
        uint32_t mode_id;
        // connection
        drmu_prop_range_t * max_bpc;
        drmu_prop_enum_t * colorspace;
        uint32_t hdr_output_metadata;
    } pid;

    int cur_mode_id;
    drmu_blob_t * mode_id_blob;
    drmu_blob_t * hdr_metadata_blob;
    struct hdr_output_metadata hdr_metadata;

} drmu_crtc_t;

typedef struct drmu_atomic_q_s {
    pthread_mutex_t lock;
    drmu_atomic_t * next_flip;
    drmu_atomic_t * cur_flip;
    drmu_atomic_t * last_flip;
    unsigned int retry_count;
    struct polltask * retry_task;
} drmu_atomic_q_t;

typedef struct drmu_env_s {
    vlc_object_t * log;
    int fd;
    uint32_t plane_count;
    drmu_plane_t * planes;
    drmModeResPtr res;

    bool modeset_allow;

    // global env for atomic flip
    drmu_atomic_q_t aq;
    // global env for bo tracking
    drmu_bo_env_t boe;

    struct pollqueue * pq;
    struct polltask * pt;
} drmu_env_t;

drmu_fb_t * drmu_fb_int_alloc(drmu_env_t * const du);
void drmu_fb_int_free(drmu_fb_t * const dfb);


