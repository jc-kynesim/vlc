struct picture_t;
struct AVFrame;

int drm_prime_attach_buf_to_pic(struct picture_t *pic, struct AVFrame *frame);

