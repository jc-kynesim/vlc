//----------------------------------------------------------------------------
//
// Simple copy in to ZC

typedef struct to_nv12_sys_s {
    vcsm_init_type_t vcsm_init_type;
    cma_buf_pool_t * cma_out_pool;
} to_nv12_sys_t;


static size_t buf_alloc_size(const vlc_fourcc_t i_chroma, const unsigned int width, const unsigned int height)
{
    const unsigned int pels = width * height;

    switch (i_chroma)
    {
        case VLC_CODEC_MMAL_ZC_RGB32:
            return pels * 4;
        case VLC_CODEC_MMAL_ZC_I420:
            return pels * 3 / 2;
        default:
            break;
    }
    return 0;
}


static picture_t *
to_nv12_filter(filter_t *p_filter, picture_t *in_pic)
{
    to_nv12_sys_t * const sys = (to_nv12_sys_t *)p_filter->p_sys;
#if TRACE_ALL
    msg_Dbg(p_filter, "<<< %s", __func__);
#endif

    assert(p_filter->fmt_out.video.i_chroma == VLC_CODEC_MMAL_ZC_I420);

    picture_t * const out_pic = filter_NewPicture(p_filter);
    if (out_pic == NULL)
        goto fail0;

    MMAL_ES_SPECIFIC_FORMAT_T mm_vfmt = {.video={0}};
    MMAL_ES_FORMAT_T mm_esfmt = {
        .encoding = vlc_to_mmal_video_fourcc(&p_filter->fmt_out.video),
        .es = &mm_vfmt};

    hw_mmal_vlc_fmt_to_mmal_fmt(&mm_esfmt, &p_filter->fmt_out.video);

    const size_t buf_alloc = buf_alloc_size(p_filter->fmt_out.video.i_chroma,
                                            mm_vfmt.video.width, mm_vfmt.video.height);
    if (buf_alloc == 0)
        goto fail1;
    cma_buf_t *const cb = cma_buf_pool_alloc_buf(sys->cma_out_pool, buf_alloc);
    if (cb == NULL)
        goto fail1;

    if (cma_buf_pic_attach(cb, out_pic) != VLC_SUCCESS)
        goto fail2;
    cma_pic_set_data(out_pic, &mm_esfmt, NULL);

    hw_mmal_copy_pic_to_buf(cma_buf_addr(cb), NULL, &mm_esfmt, in_pic);

    // Copy pic properties
    out_pic->date              = in_pic->date;
    out_pic->b_force           = in_pic->b_force;
    out_pic->b_progressive     = in_pic->b_progressive;
    out_pic->b_top_field_first = in_pic->b_top_field_first;
    out_pic->i_nb_fields       = in_pic->i_nb_fields;

    picture_Release(in_pic);

    return out_pic;

fail2:
    cma_buf_unref(cb);
fail1:
    picture_Release(out_pic);
fail0:
    picture_Release(in_pic);
    return NULL;
}

static void to_nv12_flush(filter_t * p_filter)
{
    VLC_UNUSED(p_filter);
}

static void CloseConverterToNv12(vlc_object_t * obj)
{
    filter_t * const p_filter = (filter_t *)obj;
    to_nv12_sys_t * const sys = (to_nv12_sys_t *)p_filter->p_sys;

    if (sys == NULL)
        return;

    p_filter->p_sys = NULL;

    cma_buf_pool_deletez(&sys->cma_out_pool);
    cma_vcsm_exit(sys->vcsm_init_type);

    free(sys);
}

static bool to_nv12_validate_fmt(const video_format_t * const f_in, const video_format_t * const f_out)
{
    if (!(f_in->i_chroma == VLC_CODEC_DRM_PRIME_SAND30 &&
          f_out->i_chroma == VLC_CODEC_NV12))
    {
        return false;
    }
    if (f_in->i_height != f_out->i_height ||
        f_in->i_width  != f_out->i_width)
    {
        return false;
    }

    return true;
}

static int OpenConverterToNv12(vlc_object_t * obj)
{
    int ret = VLC_EGENERIC;
    filter_t * const p_filter = (filter_t *)obj;

    if (!to_nv12_validate_fmt(&p_filter->fmt_in.video, &p_filter->fmt_out.video))
        goto fail;

    {
        char dbuf0[5], dbuf1[5];
        msg_Dbg(p_filter, "%s: %s,%dx%d [(%d,%d) %d/%d] sar:%d/%d->%s,%dx%d [(%d,%d) %dx%d] rgb:%#x:%#x:%#x sar:%d/%d", __func__,
                str_fourcc(dbuf0, p_filter->fmt_in.video.i_chroma),
                p_filter->fmt_in.video.i_width, p_filter->fmt_in.video.i_height,
                p_filter->fmt_in.video.i_x_offset, p_filter->fmt_in.video.i_y_offset,
                p_filter->fmt_in.video.i_visible_width, p_filter->fmt_in.video.i_visible_height,
                p_filter->fmt_in.video.i_sar_num, p_filter->fmt_in.video.i_sar_den,
                str_fourcc(dbuf1, p_filter->fmt_out.video.i_chroma),
                p_filter->fmt_out.video.i_width, p_filter->fmt_out.video.i_height,
                p_filter->fmt_out.video.i_x_offset, p_filter->fmt_out.video.i_y_offset,
                p_filter->fmt_out.video.i_visible_width, p_filter->fmt_out.video.i_visible_height,
                p_filter->fmt_out.video.i_rmask, p_filter->fmt_out.video.i_gmask, p_filter->fmt_out.video.i_bmask,
                p_filter->fmt_out.video.i_sar_num, p_filter->fmt_out.video.i_sar_den);
    }

    to_nv12_sys_t * const sys = calloc(1, sizeof(*sys));
    if (!sys) {
        ret = VLC_ENOMEM;
        goto fail;
    }
    p_filter->p_sys = (filter_sys_t *)sys;

    if ((sys->vcsm_init_type = cma_vcsm_init()) == VCSM_INIT_NONE) {
        msg_Err(p_filter, "VCSM init failed");
        goto fail;
    }

    if ((sys->cma_out_pool = cma_buf_pool_new(5, 5, true, "conv-to-zc")) == NULL)
    {
        msg_Err(p_filter, "Failed to allocate input CMA pool");
        goto fail;
    }

    p_filter->pf_video_filter = to_nv12_filter;
    p_filter->pf_flush = to_nv12_flush;
    return VLC_SUCCESS;

fail:
    CloseConverterToNv12(obj);
    return ret;
}

vlc_module_begin()
    add_submodule()
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_shortname(N_("DRMPRIME-SAND30 to NV12"))
    set_description(N_("DRMPRIME-SAND30 to NV12 filter"))
    add_shortcut("sand30_to_nv12")
    set_capability( "video converter", 901 )
    set_callbacks(OpenConverterToNv12, CloseConverterToNv12)
vlc_module_end()

