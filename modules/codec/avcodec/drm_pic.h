struct picture_t;
struct AVFrame;
struct decoder_t *dec;

int drm_prime_attach_buf_to_pic(struct decoder_t *dec, struct picture_t *pic, struct AVFrame *frame);

