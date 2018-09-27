/*****************************************************************************
 * mmal_picture.c: MMAL picture related shared functionality
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Julian Scheel <julian@jusst.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

// We would really like to use vlc_thread.h but the detach thread stuff can't be
// used here :-(
#include <pthread.h>

#include <vlc_common.h>
#include <vlc_picture.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/vmcs_host/vcgencmd.h>
#include <interface/vcsm/user-vcsm.h>

#include "mmal_picture.h"


void vlc_to_mmal_pic_fmt(MMAL_PORT_T * const port, const es_format_t * const es_vlc)
{
    const video_format_t *const vf_vlc = &es_vlc->video;
    MMAL_VIDEO_FORMAT_T * vf_mmal = &port->format->es->video;

    vf_mmal->width          = (vf_vlc->i_width + 31) & ~31;
    vf_mmal->height         = (vf_vlc->i_height + 15) & ~15;;
    vf_mmal->crop.x         = vf_vlc->i_x_offset;
    vf_mmal->crop.y         = vf_vlc->i_y_offset;
    vf_mmal->crop.width     = vf_vlc->i_visible_width;
    vf_mmal->crop.height    = vf_vlc->i_visible_height;
    if (vf_vlc->i_sar_num == 0 || vf_vlc->i_sar_den == 0) {
        vf_mmal->par.num        = 1;
        vf_mmal->par.den        = 1;
    } else {
        vf_mmal->par.num        = vf_vlc->i_sar_num;
        vf_mmal->par.den        = vf_vlc->i_sar_den;
    }
    vf_mmal->frame_rate.num = vf_vlc->i_frame_rate;
    vf_mmal->frame_rate.den = vf_vlc->i_frame_rate_base;
}

hw_mmal_port_pool_ref_t * hw_mmal_port_pool_ref_create(MMAL_PORT_T * const port,
   const unsigned int headers, const uint32_t payload_size)
{
    hw_mmal_port_pool_ref_t * ppr = calloc(1, sizeof(hw_mmal_port_pool_ref_t));
    if (ppr == NULL)
        return NULL;

    if ((ppr->pool = mmal_port_pool_create(port, headers, payload_size)) == NULL)
        goto fail;

    ppr->port = port;
    atomic_store(&ppr->refs, 1);
    return ppr;

fail:
    free(ppr);
    return NULL;
}

static void do_detached(void *(*fn)(void *), void * v)
{
    pthread_t dothread;
    pthread_create(&dothread, NULL, fn, v);
    pthread_detach(dothread);
}

// Destroy a ppr - aranged s.t. it has the correct prototype for a pthread
static void * kill_ppr(void * v)
{
    hw_mmal_port_pool_ref_t * const ppr = v;
    if (ppr->port->is_enabled)
        mmal_port_disable(ppr->port);  // Avoid annoyed messages from MMAL when we kill the pool
    mmal_port_pool_destroy(ppr->port, ppr->pool);
    free(ppr);
    return NULL;
}

void hw_mmal_port_pool_ref_release(hw_mmal_port_pool_ref_t * const ppr, const bool in_cb)
{
    if (ppr == NULL)
        return;
    if (atomic_fetch_sub(&ppr->refs, 1) != 1)
        return;
    if (in_cb)
        do_detached(kill_ppr, ppr);
    else
        kill_ppr(ppr);
}

// Put buffer in port if possible - if not then release to pool
// Returns true if sent, false if recycled
bool hw_mmal_port_pool_ref_recycle(hw_mmal_port_pool_ref_t * const ppr, MMAL_BUFFER_HEADER_T * const buf)
{
    mmal_buffer_header_reset(buf);
    buf->user_data = NULL;

    if (mmal_port_send_buffer(ppr->port, buf) == MMAL_SUCCESS)
        return true;
    mmal_buffer_header_release(buf);
    return false;
}

MMAL_STATUS_T hw_mmal_port_pool_ref_fill(hw_mmal_port_pool_ref_t * const ppr)
{
    MMAL_BUFFER_HEADER_T * buf;
    MMAL_STATUS_T err = MMAL_SUCCESS;

    while ((buf = mmal_queue_get(ppr->pool->queue)) != NULL) {
        if ((err = mmal_port_send_buffer(ppr->port, buf)) != MMAL_SUCCESS)
        {
            mmal_queue_put_back(ppr->pool->queue, buf);
            break;
        }
    }
    return err;
}

void hw_mmal_pic_ctx_destroy(picture_context_t * pic_ctx_cmn)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    mmal_buffer_header_release(ctx->buf);
}

picture_context_t * hw_mmal_pic_ctx_copy(picture_context_t * pic_ctx_cmn)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    mmal_buffer_header_acquire(ctx->buf);
    return pic_ctx_cmn;
}

static MMAL_BOOL_T
buf_pre_release_cb(MMAL_BUFFER_HEADER_T * buf, void *userdata)
{
    pic_ctx_mmal_t * const ctx = userdata;

    // Kill the callback - otherwise we will go in circles!
    mmal_buffer_header_pre_release_cb_set(buf, (MMAL_BH_PRE_RELEASE_CB_T)0, NULL);
    mmal_buffer_header_acquire(buf);  // Ref it again

    // Kill any sub-pic
    if (ctx->sub.subpic != NULL) {
        picture_Release(ctx->sub.subpic);
        ctx->sub.subpic = NULL;  // Not needed but be tidy to make sure we can't reuse accidentaly
    }

    // As we have re-acquired the buffer we need a full release
    // (not continue) to zap the ref count back to zero
    // This is "safe" 'cos we have already reset the cb
    hw_mmal_port_pool_ref_recycle(ctx->ppr, buf);
    hw_mmal_port_pool_ref_release(ctx->ppr, true); // Assume in callback

    free(ctx); // Free self
    return MMAL_TRUE;
}

picture_context_t *
hw_mmal_gen_context(MMAL_BUFFER_HEADER_T * buf, hw_mmal_port_pool_ref_t * const ppr,
                    vlc_object_t * obj)
{
    pic_ctx_mmal_t * const ctx = calloc(1, sizeof(pic_ctx_mmal_t));

    if (ctx == NULL)
        return NULL;

    hw_mmal_port_pool_ref_acquire(ppr);
    mmal_buffer_header_pre_release_cb_set(buf, buf_pre_release_cb, ctx);

    ctx->cmn.copy = hw_mmal_pic_ctx_copy;
    ctx->cmn.destroy = hw_mmal_pic_ctx_destroy;
    ctx->buf = buf;
    ctx->ppr = ppr;
    ctx->obj = obj;

    buf->user_data = ctx;
    return &ctx->cmn;
}

int hw_mmal_get_gpu_mem(void) {
    static int stashed_val = -2;
    VCHI_INSTANCE_T vchi_instance;
    VCHI_CONNECTION_T *vchi_connection = NULL;
    char rbuf[1024] = { 0 };

    if (stashed_val >= -1)
        return stashed_val;

    if (vchi_initialise(&vchi_instance) != 0)
        goto fail0;

    //create a vchi connection
    if (vchi_connect(NULL, 0, vchi_instance) != 0)
        goto fail0;

    vc_vchi_gencmd_init(vchi_instance, &vchi_connection, 1);

    //send the gencmd for the argument
    if (vc_gencmd_send("get_mem gpu") != 0)
        goto fail;

    if (vc_gencmd_read_response(rbuf, sizeof(rbuf) - 1) != 0)
        goto fail;

    if (strncmp(rbuf, "gpu=", 4) != 0)
        goto fail;

    char *p;
    unsigned long m = strtoul(rbuf + 4, &p, 10);

    if (p[0] != 'M' || p[1] != '\0')
        stashed_val = -1;
    else
        stashed_val = (int)m << 20;

    vc_gencmd_stop();

    //close the vchi connection
    vchi_disconnect(vchi_instance);

    return stashed_val;

fail:
    vc_gencmd_stop();
    vchi_disconnect(vchi_instance);
fail0:
    stashed_val = -1;
    return -1;
};

// ===========================================================================

typedef struct pool_ent_s
{
    struct pool_ent_s * next;
    struct pool_ent_s * prev;
    size_t size;

    int vcsm_hdl;
    int vc_hdl;
    void * buf;
} pool_ent_t;

typedef struct pool_ctl_s
{
    pool_ent_t * ents;
    pool_ent_t * tail;
    unsigned int n;
    unsigned int max_n;
} pool_ctl_t;


pool_ent_t * pool_extract_ent(pool_ctl_t * const pc, pool_ent_t * const ent)
{
    if (ent->next == NULL)
        pc->tail = ent->prev;
    else
        ent->next->prev = ent->prev;

    if (ent->prev == NULL)
        pc->ents = ent->next;
    else
        ent->prev->next = ent->next;

    ent->prev = ent->next = NULL;

    --pc->n;

    return ent;  // For convienience
}

void pool_free_last(pool_ctl_t * const pc)
{
    pool_ent_t * const ent = pool_extract_ent(pc, pc->tail);

    // Free contents
    vcsm_unlock_hdl(ent->vcsm_hdl);

    vcsm_free(ent->vcsm_hdl);

    free(ent);
}

pool_ent_t * pool_alloc_new(pool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * ent = malloc(sizeof(*ent));

    if (ent == NULL)
        return NULL;

    ent->next = ent->prev = NULL;

    // Alloc from vcsm
    if ((ent->vcsm_hdl = vcsm_malloc_cache(req_size, VCSM_CACHE_TYPE_HOST, (char *)"vlc-subpic")) == -1)
        goto fail1;
    if ((ent->vc_hdl = vcsm_vc_hdl_from_hdl(ent->vcsm_hdl)) == 0)
        goto fail2;
    if ((ent->buf = vcsm_lock(ent->vcsm_hdl)) == NULL)
        goto fail2;

    ent->size = req_size;

    return ent;

fail2:
    vcsm_free(ent->vcsm_hdl);
fail1:
    free(ent);
    return NULL;
}


void pool_recycle(pool_ctl_t * const pc, pool_ent_t * const ent)
{
    if (pc->n >= pc->max_n)
        pool_free_last(pc);

    if ((ent->next = pc->ents) == NULL)
        pc->tail = ent;
    else
        ent->next->prev = ent;

    ent->prev = NULL;
    pc->ents = ent;
}


pool_ent_t * pool_best_fit(pool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * ent = pc->ents;
    pool_ent_t * best = NULL;

    // Simple scan
    while (ent != NULL) {
        if (ent->size >= req_size && ent->size <= req_size * 2 && (best == NULL || best->size > ent->size))
            best = ent;
        ent = ent->next;
    }

    // extract best from chain if we've found it
    if (best != NULL)
        return pool_extract_ent(pc, best);

    return pool_alloc_new(pc, req_size);
}

int pool_init()
{
    vcsm_init();
    return 0;
}
