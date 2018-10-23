#ifndef VLC_HW_MMAL_SUBPIC_H_
#define VLC_HW_MMAL_SUBPIC_H_

typedef struct subpic_reg_stash_s
{
    MMAL_RECT_T dest_rect;
    unsigned int alpha;
    unsigned int seq;
} subpic_reg_stash_t;

int hw_mmal_subpic_update(vlc_object_t * const p_filter,
    picture_t * const p_pic, const unsigned int sub_no,
    MMAL_PORT_T * const port,
    subpic_reg_stash_t * const stash,
    MMAL_PORT_T * const scale_out_port, MMAL_PORT_T * const scale_in_port,
    MMAL_POOL_T * const pool, const uint64_t pts);

#endif

