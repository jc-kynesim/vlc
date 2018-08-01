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

#include "mmal_picture.h"


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


