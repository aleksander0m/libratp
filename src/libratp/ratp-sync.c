/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libratp - Reliable Asynchronous Transport Protocol library
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2017 Zodiac Inflight Innovations
 * Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#define _GNU_SOURCE
#include <malloc.h>
#include <stdint.h>
#include <pthread.h>

#include "ratp.h"
#include "ratp-log.h"

/******************************************************************************/

typedef void (* sync_state_update_trigger_func) (ratp_link_t *self);

struct sync_context_s {
    sync_state_update_trigger_func trigger_func;
    ratp_link_state_t              expected_state;
    pthread_mutex_t                sync_lock;
    pthread_cond_t                 sync_cond;
};

/******************************************************************************/

static void
sync_state_update_ready (ratp_link_t       *self,
                         ratp_link_state_t  old_state,
                         ratp_link_state_t  new_state,
                         ratp_status_t      status_reason,
                         void              *user_data)
{
    struct sync_context_s *ctx = (struct sync_context_s *) user_data;

    if (new_state == ctx->expected_state) {
        pthread_mutex_lock   (&ctx->sync_lock);
        pthread_cond_signal  (&ctx->sync_cond);
        pthread_mutex_unlock (&ctx->sync_lock);
    }
}

static ratp_status_t
sync_state_update (ratp_link_t                    *ratp,
                   unsigned long                   timeout_ms,
                   sync_state_update_trigger_func  trigger_func,
                   ratp_link_state_t               expected_state)
{
    ratp_status_t         st;
    struct sync_context_s ctx;
    struct timespec       absolute_timeout;

    pthread_mutex_init (&ctx.sync_lock, NULL);
    pthread_cond_init  (&ctx.sync_cond, NULL);
    ctx.expected_state = expected_state;

    clock_gettime (CLOCK_REALTIME, &absolute_timeout);
    absolute_timeout.tv_sec  += (timeout_ms / 1000);
    absolute_timeout.tv_nsec += ((timeout_ms % 1000) * 1E6);
    if (absolute_timeout.tv_nsec >= 1E9) {
        absolute_timeout.tv_nsec -= 1E9;
        absolute_timeout.tv_sec++;
    }

    ratp_link_set_state_update_callback (ratp, sync_state_update_ready, &ctx);

    trigger_func (ratp);

    pthread_mutex_lock (&ctx.sync_lock);
    st = (pthread_cond_timedwait (&ctx.sync_cond, &ctx.sync_lock, &absolute_timeout) == 0) ? RATP_STATUS_OK : RATP_STATUS_TIMEOUT;

    ratp_link_set_state_update_callback (ratp, NULL, NULL);

    pthread_mutex_destroy (&ctx.sync_lock);
    pthread_cond_destroy  (&ctx.sync_cond);

    return st;
}

/******************************************************************************/

ratp_status_t
ratp_link_active_open_sync (ratp_link_t   *self,
                            unsigned long  timeout_ms)
{
    return sync_state_update (self,
                              timeout_ms,
                              (sync_state_update_trigger_func) ratp_link_active_open,
                              RATP_LINK_STATE_ESTABLISHED);
}

ratp_status_t
ratp_link_close_sync (ratp_link_t   *self,
                      unsigned long  timeout_ms)
{
    return sync_state_update (self,
                              timeout_ms,
                              (sync_state_update_trigger_func) ratp_link_close,
                              RATP_LINK_STATE_CLOSED);
}
