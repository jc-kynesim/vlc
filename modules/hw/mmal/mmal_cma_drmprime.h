#ifndef VLC_HW_MMAL_MMAL_CMA_DRMPRIME_H_
#define VLC_HW_MMAL_MMAL_CMA_DRMPRIME_H_

struct AVFrame;
cma_buf_pool_t * cma_drmprime_pool_new(const unsigned int pool_size, const unsigned int flight_size, const bool all_in_flight, const char * const name);
cma_buf_t * cma_drmprime_pool_alloc_buf(cma_buf_pool_t * const cbp, struct AVFrame * frame);

#endif // VLC_MMAL_MMAL_CMA_H_

