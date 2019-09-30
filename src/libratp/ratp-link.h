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

#ifndef RATP_LINK_H
#define RATP_LINK_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <event2/event.h>
#include <event2/thread.h>

#include "ratp.h"
#include "ratp-message.h"

/* Backend implementatio */
struct ratp_backend_s {
    void          (* free)       (ratp_link_t *self);
    ratp_status_t (* initialize) (ratp_link_t *self);
    void          (* shutdown)   (ratp_link_t *self);
    int           (* get_in_fd)  (ratp_link_t *self);
    int           (* get_out_fd) (ratp_link_t *self);
    void          (* flush)      (ratp_link_t *self);
};

/* Outgoing data item */
struct ratp_data_s {
    ratp_link_t               *self;
    struct ratp_data_s        *next;
    struct ratp_data_s        *prev;
    size_t                     buffer_size;
    size_t                     buffer_sent;
    struct event              *user_timeout;
    ratp_link_send_ready_func  ready_func;
    void                      *user_data;
    uint8_t                    buffer[];
};

struct ratp_link_s {
    /* Link settings */
    const struct ratp_backend_s *backend;
    uint8_t                      local_mdl;
    unsigned int                 max_retransmissions;

    /* Thread and event loop */
    pthread_t                   tid;
    pthread_mutex_t             mutex;
    struct event_base          *base;
    struct event               *keepalive;
    struct event               *input;
    bool                        shutdown;
    ratp_link_initialized_func  initialized_callback;
    void                       *initialized_callback_user_data;

    /* State machine */
    ratp_link_state_t            state;
    ratp_status_t                status_reason;
    ratp_link_state_update_func  state_update_callback;
    void                        *state_update_callback_user_data;

    /* Ongoing RX */
    uint8_t rx_buffer[RATP_MAX_MSG_SIZE];
    size_t  rx_buffer_n;

    /* Transmission Control Block */
    bool    active;
    bool    sn_sent;
    bool    sn_received;
    uint8_t peer_mdl;

    /* Ongoing TX with retransmission support */
    uint8_t          tx_buffer[RATP_MAX_MSG_SIZE];
    struct timespec  tx_retransmission_start;
    struct event    *tx_retransmission_timeout;
    unsigned int     tx_retransmission_n;
    unsigned long    rto;

    /* Time wait state support */
    struct event  *time_wait_timeout;
    unsigned long  srtt;

    /* Queued data items to be sent */
    struct ratp_data_s *outgoing_data_queue_first;
    struct ratp_data_s *outgoing_data_queue_last;

    /* Ongoing data items being received */
    uint8_t                      *incoming_data;
    size_t                        incoming_data_size;
    ratp_link_receive_ready_func  incoming_ready_callback;
    void                         *incoming_ready_callback_user_data;
};

/* Common initialization */
void ratp_link_common_init (ratp_link_t                 *self,
                            const struct ratp_backend_s *backend,
                            uint8_t                      mdl);

#endif /* RATP_LINK_H */
