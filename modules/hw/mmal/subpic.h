#ifndef VLC_HW_MMAL_SUBPIC_H_
#define VLC_HW_MMAL_SUBPIC_H_

typedef struct subpic_reg_stash_s
{
    MMAL_PORT_T * port;
    MMAL_POOL_T * pool;
    unsigned int layer;
    // Shadow  vars so we can tell if stuff has changed
    MMAL_RECT_T dest_rect;
    unsigned int alpha;
    unsigned int seq;
} subpic_reg_stash_t;

int hw_mmal_subpic_update(vlc_object_t * const p_filter,
                          MMAL_BUFFER_HEADER_T * const sub_buf,
                          subpic_reg_stash_t * const spe,
                          const video_format_t * const fmt,
                          const MMAL_RECT_T * const scale_out,
                          const uint64_t pts);

void hw_mmal_subpic_flush(vlc_object_t * const p_filter, subpic_reg_stash_t * const spe);

void hw_mmal_subpic_close(vlc_object_t * const p_filter, subpic_reg_stash_t * const spe);

MMAL_STATUS_T hw_mmal_subpic_open(vlc_object_t * const p_filter, subpic_reg_stash_t * const spe, MMAL_PORT_T * const port, const unsigned int layer);

#endif

