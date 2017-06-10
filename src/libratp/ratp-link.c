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

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <malloc.h>
#include <sys/param.h>

#include <common.h>

#include "ratp.h"
#include "ratp-link.h"
#include "ratp-message.h"
#include "ratp-log.h"

#define DEFAULT_KEEPALIVE_TIMEOUT_S  10
#define DEFAULT_MAX_RETRANSMISSIONS  100
#define DEFAULT_SRTT_MS              100
#define DEFAULT_RTO_MS               100

/******************************************************************************/

static void
reset_tcb (ratp_link_t *self)
{
    self->peer_mdl    = 0;
    self->active      = false;
    self->rx_buffer_n = 0;
}

void
ratp_link_common_init (ratp_link_t                 *self,
                       const struct ratp_backend_s *backend,
                       uint8_t                      mdl)
{
    self->backend             = backend;
    self->state               = RATP_LINK_STATE_CLOSED;
    self->status_reason       = RATP_STATUS_OK;
    self->local_mdl           = mdl;
    self->max_retransmissions = DEFAULT_MAX_RETRANSMISSIONS;
    self->srtt                = DEFAULT_SRTT_MS;
    self->rto                 = DEFAULT_RTO_MS;

    reset_tcb (self);

    pthread_mutex_init (&self->mutex, NULL);
}

/******************************************************************************/

static void
raw_send (ratp_link_t   *self,
          const uint8_t *buffer,
          size_t         buffer_size)
{
    int fd;

    fd = self->backend->get_out_fd (self);

    while (1) {
        ssize_t n_written;

        errno = 0;
        if ((n_written = write (fd, buffer, buffer_size)) < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            /* fatal error, discard data */
            ratp_error ("couldn't write: %s", strerror (errno));
            return;
        }

        if (n_written < buffer_size) {
            ratp_error ("couldn't write full message: %zd/%zu bytes written", n_written, buffer_size);
            return;
        }

        self->backend->flush (self);
        return;
    }
}

/******************************************************************************/

static void stop_retransmission_logic (ratp_link_t *self,
                                       bool         success);

static void
link_update_state_locked (ratp_link_t       *self,
                          ratp_link_state_t  state,
                          ratp_status_t      status_reason)
{
    ratp_link_state_t old_state;
    bool              notify_update = false;

    /* We're locked when we get here! */

    if (self->state != state) {
        notify_update       = true;
        old_state           = self->state;
        self->state         = state;
        self->status_reason = status_reason;
    }

    pthread_mutex_unlock (&self->mutex);

    if (notify_update) {
        ratp_info ("state update: %s -> %s (%s)",
                   ratp_link_state_str (old_state),
                   ratp_link_state_str (state),
                   ratp_status_str     (status_reason));
        if (self->state_update_callback)
            self->state_update_callback (self, old_state, state, status_reason, self->state_update_callback_user_data);
    }

    pthread_mutex_lock (&self->mutex);
}

/******************************************************************************/

static void cleanup_event                          (struct event       **pevent,
                                                    short                events);
static void send_complete                          (ratp_link_t         *self,
                                                    struct ratp_data_s  *data,
                                                    ratp_status_t        status);
static void remove_from_outgoing_data_queue_locked (ratp_link_t         *self,
                                                    struct ratp_data_s  *data);
static void flush_outgoing_data_queue_locked       (ratp_link_t         *self,
                                                    ratp_status_t        reason);

static void
stop_retransmission_logic (ratp_link_t *self,
                           bool         success)
{
    if (!self->tx_retransmission_timeout)
        return;

    /* Update SRTT and RTO */
    if (success) {
        struct timespec now;
        struct timespec difference;
        unsigned long   rtt, alpha = 8, beta = 15;

        clock_gettime (CLOCK_MONOTONIC, &now);
        common_timespec_diff (&self->tx_retransmission_start, &now, &difference);
        rtt = (difference.tv_sec * 1E3) + (difference.tv_nsec / 1E6);

        self->srtt = (alpha * self->srtt + (10 - alpha) * rtt) / 10;
        self->rto = MAX (200, beta * self->srtt / 10);

        ratp_debug ("last packet acknowledged in %lums (SRTT: %lums, RTO: %lums)", rtt, self->srtt, self->rto);
    }

    cleanup_event (&self->tx_retransmission_timeout, EV_TIMEOUT);
}

static void
retransmission_timeout (evutil_socket_t  fd,
                        short            events,
                        ratp_link_t     *self)
{
    pthread_mutex_lock (&self->mutex);

    if (++self->tx_retransmission_n == self->max_retransmissions) {
        struct ratp_data_s *current;

        /* Notify data timeout */
        current = self->outgoing_data_queue_first;
        if (current && current->buffer_sent > 0) {
            remove_from_outgoing_data_queue_locked (self, current);
            ratp_warning ("retransmission timeout expired: %zu/%zu bytes sent", current->buffer_sent, current->buffer_size);
            send_complete (self, current, RATP_STATUS_CONNECTION_ABORTED_RETRANSMISSION_FAILURE);
        }

        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_ABORTED_RETRANSMISSION_FAILURE);
        stop_retransmission_logic (self, false);
        reset_tcb (self);
        link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_ABORTED_RETRANSMISSION_FAILURE);
        pthread_mutex_unlock (&self->mutex);
        return;
    }

    clock_gettime (CLOCK_MONOTONIC, &self->tx_retransmission_start);
    raw_send (self, (const uint8_t *) self->tx_buffer, ratp_message_get_size ((struct ratp_message_s *)self->tx_buffer));
    ratp_message_debug ((struct ratp_message_s *)self->tx_buffer, "message sent (r)");

    pthread_mutex_unlock (&self->mutex);
}

static void
start_retransmission_logic (ratp_link_t *self)
{
    struct timeval retransmission_time = {
        .tv_sec  = self->rto / 1000,
        .tv_usec = (self->rto % 1000) * 1000,
    };

    clock_gettime (CLOCK_MONOTONIC, &self->tx_retransmission_start);
    self->tx_retransmission_n = 0;

    assert (!self->tx_retransmission_timeout);
    self->tx_retransmission_timeout = event_new (self->base, -1, EV_TIMEOUT | EV_PERSIST, (event_callback_fn) retransmission_timeout, self);
    if (!self->tx_retransmission_timeout) {
        ratp_error ("couldn't create retransmission timeout");
        return;
    }

    if (event_add (self->tx_retransmission_timeout, &retransmission_time) < 0) {
        cleanup_event (&self->tx_retransmission_timeout, EV_TIMEOUT);
        ratp_error ("couldn't schedule retransmission timeout");
        return;
    }
}

/******************************************************************************/

static void
stop_time_wait_logic (ratp_link_t *self)
{
    if (!self->time_wait_timeout)
        return;
    cleanup_event (&self->time_wait_timeout, EV_TIMEOUT);
}

static void
time_wait_timeout (evutil_socket_t  fd,
                   short            events,
                   ratp_link_t     *self)
{
    ratp_debug ("TIME-WAIT timeout expired");

    pthread_mutex_lock (&self->mutex);
    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_OK);
    pthread_mutex_unlock (&self->mutex);
}

static void
start_time_wait_logic (ratp_link_t *self)
{
    struct timeval time_wait_time;
    unsigned long  time_wait_time_ms;

    time_wait_time_ms = 2 * self->srtt;
    time_wait_time.tv_sec  = time_wait_time_ms / 1000;
    time_wait_time.tv_usec = (time_wait_time_ms % 1000) * 1000,

    assert (!self->time_wait_timeout);
    self->time_wait_timeout = event_new (self->base, -1, EV_TIMEOUT, (event_callback_fn) time_wait_timeout, self);
    if (!self->time_wait_timeout) {
        ratp_error ("couldn't create TIME-WAIT timeout");
        return;
    }

    if (event_add (self->time_wait_timeout, &time_wait_time) < 0) {
        cleanup_event (&self->time_wait_timeout, EV_TIMEOUT);
        ratp_error ("couldn't schedule TIME-WAIT timeout");
        return;
    }

    ratp_debug ("scheduled TIME-WAIT timeout in %lums", time_wait_time_ms);
}

/******************************************************************************/

static void
send_ack_no_data (ratp_link_t                 *self,
                  const struct ratp_message_s *msg)
{
    struct ratp_message_s ack;

    /* no retransmission schedule needed */

    ratp_message_header_init     (&ack, RATP_CONTROL_ACK, 0);
    ratp_message_set_sn          (&ack, ratp_message_get_an (msg));
    ratp_message_set_next_an     (&ack, ratp_message_get_sn (msg));
    ratp_message_header_complete (&ack);

    raw_send (self, (const uint8_t *) &ack, sizeof (ack));
    ratp_message_debug (&ack, "message sent    ");
}

static void
send_tx_buffer (ratp_link_t *self)
{
    struct ratp_message_s *msg;

    msg = (struct ratp_message_s *)(&self->tx_buffer[0]);

    /* retransmission schedule needed */
    assert ((msg->control & (RATP_CONTROL_SYN | RATP_CONTROL_RST | RATP_CONTROL_FIN)) || ratp_message_has_data (msg));
    start_retransmission_logic (self);
    self->sn_sent = ratp_message_get_sn (msg);

    ratp_message_header_complete (msg);
    ratp_message_data_complete   (msg);

    raw_send (self, (const uint8_t *) msg, ratp_message_get_size (msg));
    ratp_message_debug (msg, "message sent    ");
}

static void
send_message (ratp_link_t                 *self,
              const struct ratp_message_s *msg)
{
    memcpy (self->tx_buffer, msg, ratp_message_get_size (msg));
    send_tx_buffer (self);
}

/******************************************************************************/

static void
append_incoming_data (ratp_link_t   *self,
                      const uint8_t *data,
                      size_t         data_size)
{
    uint8_t *aux;

    aux = realloc (self->incoming_data, self->incoming_data_size + data_size);
    if (!aux) {
        ratp_error ("couldn't reallocate buffer for incoming data (%zu + %zu bytes)", self->incoming_data_size, data_size);
        return;
    }

    self->incoming_data = aux;
    memcpy (&self->incoming_data[self->incoming_data_size], data, data_size);
    self->incoming_data_size += data_size;
}

static void
common_process_incoming_data (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    if (msg->control & RATP_CONTROL_SO)
        append_incoming_data (self, &msg->data_length, 1);
    else
        append_incoming_data (self, msg->data, msg->data_length);

    /* On EOR, we notify the data */
    if (msg->control & RATP_CONTROL_EOR) {
        if (self->incoming_ready_callback)
            self->incoming_ready_callback (self, self->incoming_data, self->incoming_data_size, self->incoming_ready_callback_user_data);
        free (self->incoming_data);
        self->incoming_data = NULL;
        self->incoming_data_size = 0;
    }
}

void
ratp_link_set_receive_callback (ratp_link_t                  *self,
                                ratp_link_receive_ready_func  callback,
                                void                         *user_data)
{
    pthread_mutex_lock (&self->mutex);
    self->incoming_ready_callback           = callback;
    self->incoming_ready_callback_user_data = user_data;
    pthread_mutex_unlock (&self->mutex);
}

/******************************************************************************/
/*
 *  STATE                BEHAVIOR
 *  =============+========================
 *  LISTEN       |  A
 *  -------------+------------------------
 *  SYN-SENT     |  B
 *  -------------+------------------------
 *  SYN-RECEIVED |  C1  D1  E  F1  H1
 *  -------------+------------------------
 *  ESTABLISHED  |  C2  D2  E  F2  H2  I1
 *  -------------+------------------------
 *  FIN-WAIT     |  C2  D2  E  F3  H3
 *  -------------+------------------------
 *  LAST-ACK     |  C2  D3  E  F3  H4
 *  -------------+------------------------
 *  CLOSING      |  C2  D3  E  F3  H5
 *  -------------+------------------------
 *  TIME-WAIT    |  D3  E  F3 H6
 *  -------------+------------------------
 *  CLOSED       |  G
 *  -------------+------------------------
 */

static void
dispatch_message_behavior_a (ratp_link_t                 *self,
                             const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior A]");

    /*
     * This procedure details the behavior of the LISTEN state.  First
     * check the packet for the RST flag.  If it is set then packet is
     * discarded and ignored, return and continue the processing
     * associated with this state.
     */
    if (msg->control & RATP_CONTROL_RST)
        return;

    /*
     * We assume now that the RST flag was not set.  Check the packet
     * for the ACK flag.  If it is set we have an illegal condition
     * since no connection has yet been opened.  Send a RST packet
     * with the correct response SN value:
     *
     *    <SN=received AN><CTL=RST>
     *
     * Return to the current state without any further processing.
     */
    if (msg->control & RATP_CONTROL_ACK) {
        ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
        send_tx_buffer (self);
        return;
    }

    /*
     * We assume now that neither the RST nor the ACK flags were set.
     * Check the packet for a SYN flag.  If it is set then an attempt
     * is being made to open a connection.  Create a TCB for this
     * connection.  The sender has placed its MDL in the LENGTH field,
     * also specified is the sender's initial SN value.  Retrieve and
     * place them into the TCB.  Note that the presence of the SO flag
     * is ignored since it has no meaning when either of the SYN, RST,
     * or FIN flags are set.
     *
     * Send a SYN packet which acknowledges the SYN received.  Choose
     * the initial SN value and the MDL for this end of the
     * connection:
     *
     *    <SN=0><AN=received SN+1 modulo 2><CTL=SYN, ACK><LENGTH=MDL>
     *
     * and go to the SYN-RECEIVED state without any further
     * processing.
     */
    if (msg->control & RATP_CONTROL_SYN) {
        self->peer_mdl = msg->data_length;
        self->sn_received = ratp_message_get_sn (msg);

        link_update_state_locked (self, RATP_LINK_STATE_SYN_RECEIVED, RATP_STATUS_OK);

        ratp_message_header_init (rsp, RATP_CONTROL_SYN | RATP_CONTROL_ACK, self->local_mdl);
        ratp_message_set_next_an (rsp, self->sn_received);
        send_tx_buffer (self);
        return;
    }

    /* Any packet not satisfying the above tests is discarded and
     * ignored.  Return to the current state without any further
     * processing.
     */
}

static bool send_next_data_locked (ratp_link_t *self);

static void
dispatch_message_behavior_b (ratp_link_t                 *self,
                             const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior B]");

    /*
     * This procedure represents the behavior of the SYN-SENT state
     * and is entered when this end of the connection decides to
     * execute an active OPEN.
     *
     * First, check the packet for the ACK flag.  If the ACK flag is
     * set then check to see if the AN value was as expected.  If it
     * was continue below.  Otherwise the AN value was unexpected.  If
     * the RST flag was set then discard the packet and return to the
     * current state without any further processing, else send a
     * reset:
     *
     * <SN=received AN><CTL=RST>
     *
     * Discard the packet and return to the current state without any
     * further processing.
     */
    if (msg->control & RATP_CONTROL_ACK) {
        if (!ratp_message_validate_an (msg, self->sn_sent)) {
            stop_retransmission_logic (self, false);
            if (!(msg->control & RATP_CONTROL_RST)) {
                ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
                ratp_message_set_sn (rsp, ratp_message_get_an (msg));
                send_tx_buffer (self);
            }
            return;
        }
        stop_retransmission_logic (self, true);
    }

    /*
     * At this point either the ACK flag was set and the AN value was
     * as expected or ACK was not set.  Second, check the RST flag.
     * If the RST flag is set there are two cases:
     *
     * 1. If the ACK flag is set then discard the packet, flush the
     * retransmission queue, inform the user "Error: Connection
     * refused", delete the TCB, and go to the CLOSED state without
     * any further processing.
     *
     * 2. If the ACK flag was not set then discard the packet and
     * return to this state without any further processing.
     */
    if (msg->control & RATP_CONTROL_RST) {
        if (msg->control & RATP_CONTROL_ACK) {
            flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_REFUSED);
            stop_retransmission_logic (self, false);
            reset_tcb (self);
            link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_REFUSED);
        }
        return;
    }

    /*
     * At this point we assume the packet contained an ACK which was
     * Ok, or there was no ACK, and there was no RST.  Now check the
     * packet for the SYN flag.
     */
    if (msg->control & RATP_CONTROL_SYN) {
        /*
         * If the ACK flag was set then our SYN has been acknowledged.
         * Store MDL received in the TCB.  At this point we are technically
         * in the ESTABLISHED state.  Send an acknowledgment packet and any
         * initial data which is queued to send:
         *
         * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK><DATA>
         *
         * Go to the ESTABLISHED state without any further processing.
         */
        if (msg->control & RATP_CONTROL_ACK) {
            self->peer_mdl    = msg->data_length;
            self->sn_received = ratp_message_get_sn (msg);

            link_update_state_locked (self, RATP_LINK_STATE_ESTABLISHED, RATP_STATUS_OK);

            if (!self->outgoing_data_queue_first || !send_next_data_locked (self))
                send_ack_no_data (self, msg);
            return;
        }

        /*
         * If the SYN flag was set but the ACK was not set then the other
         * end of the connection has executed an active open also.
         * Acknowledge the SYN, choose your MDL, and send:
         *
         * <SN=0><AN=received SN+1 modulo 2><CTL=SYN, ACK><LENGTH=MDL>
         *
         * Go to the SYN-RECEIVED state without any further processing.
         */
        self->peer_mdl    = msg->data_length;
        self->sn_received = ratp_message_get_sn (msg);

        stop_retransmission_logic (self, false);

        link_update_state_locked (self, RATP_LINK_STATE_SYN_RECEIVED, RATP_STATUS_OK);

        ratp_message_header_init (rsp, RATP_CONTROL_SYN | RATP_CONTROL_ACK, self->local_mdl);
        ratp_message_set_next_an (rsp, self->sn_received);
        send_tx_buffer (self);
        return;
    }

    /*
     * Any packet not satisfying the above tests is discarded and
     * ignored.  Return to the current state without any further
     * processing.
     */
}

static bool
dispatch_message_behavior_c1 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior C1]");

    /*
     * Examine the received SN field value.  If the SN value was
     * expected then return and continue the processing associated
     * with this state.
     */
    if (ratp_message_validate_sn (msg, self->sn_received))
        return true;

    /*
     * We now assume the SN value was not what was expected.
     *
     * If either RST or FIN were set discard the packet and return to
     * the current state without any further processing.
     */

    if ((msg->control & RATP_CONTROL_RST) || (msg->control & RATP_CONTROL_FIN))
        return false;

    /*
     * If neither RST nor FIN flags were set it is assumed that this
     * packet is a duplicate of one already received.  Send an ACK
     * back:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     *
     * Discard the duplicate packet and return to the current state
     * without any further processing.
     */
    send_ack_no_data (self, msg);
    return false;
}

static bool
dispatch_message_behavior_c2 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior C2]");

    /*
     * Examine the received SN field value.  If the SN value was
     * expected then return and continue the processing associated
     * with this state.
     */
    if (ratp_message_validate_sn (msg, self->sn_received))
        return true;

    /*
     * We now assume the SN value was not what was expected.
     *
     * If either RST or FIN were set discard the packet and return to
     * the current state without any further processing.
     */
    if ((msg->control & RATP_CONTROL_RST) || (msg->control & RATP_CONTROL_FIN))
        return false;

    /*
     * If SYN was set we assume that the other end crashed and has
     * attempted to open a new connection.  We respond by sending a
     * legal reset:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=RST, ACK>
     *
     * This will cause the other end, currently in the SYN-SENT state,
     * to close.  Flush the retransmission queue, inform the user
     * "Error: Connection reset", discard the packet, delete the TCB,
     * and go to the CLOSED state without any further processing.
     */
    if (msg->control & RATP_CONTROL_SYN) {
        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
        stop_retransmission_logic (self, false);

        ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
        ratp_message_set_next_an (rsp, ratp_message_get_sn (msg));
        send_tx_buffer (self);

        reset_tcb (self);
        link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_RESET);
        return false;
    }

    /*
     * If neither RST, FIN, nor SYN flags were set it is assumed that
     * this packet is a duplicate of one already received.  Send an
     * ACK back:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     *
     * Discard the duplicate packet and return to the current state
     * without any further processing.
     */
    send_ack_no_data (self, msg);
    return false;
}

static bool
dispatch_message_behavior_d1 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior D1]");

    /*
     * The packet is examined for a RST flag.  If RST is not set then
     * return and continue the processing associated with this state.
     */
    if (!(msg->control & RATP_CONTROL_RST))
        return true;

    /*
     * RST is now assumed to have been set.  If the connection was
     * originally initiated from the LISTEN state (it was passively
     * opened) then flush the retransmission queue, discard the
     * packet, and go to the LISTEN state without any further
     * processing.
     */
    if (!self->active) {
        stop_retransmission_logic (self, false);
        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
        link_update_state_locked (self, RATP_LINK_STATE_LISTEN, RATP_STATUS_OK);
        return false;
    }

    /*
     * If instead the connection was initiated actively (came from the
     * SYN-SENT state) then flush the retransmission queue, inform the
     * user "Error: Connection refused", discard the packet, delete
     * the TCB, and go to the CLOSED state without any further
     * processing.
     */
    stop_retransmission_logic (self, false);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_REFUSED);
    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_REFUSED);
    return false;
}

static bool
dispatch_message_behavior_d2 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior D2]");

    /*
     * The packet is examined for a RST flag.  If RST is not set then
     * return and continue the processing associated with this state.
     */
    if (!(msg->control & RATP_CONTROL_RST))
        return true;

    /*
     * RST is now assumed to have been set.  Any data remaining to be
     * sent is flushed.  The retransmission queue is flushed, the user
     * is informed "Error: Connection reset.", discard the packet,
     * delete the TCB, and go to the CLOSED state without any further
     * processing.
     */
    stop_retransmission_logic (self, false);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_RESET);
    return false;
}

static bool
dispatch_message_behavior_d3 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior D3]");

    /*
     * The packet is examined for a RST flag.  If RST is not set then
     * return and continue the processing associated with this state.
     */
    if (!(msg->control & RATP_CONTROL_RST))
        return true;

     /*
     * RST is now assumed to have been set.  Discard the packet,
     * delete the TCB, and go to the CLOSED state without any further
     * processing.
     */
    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_OK);
    return false;
}

static bool
dispatch_message_behavior_e (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior E]");

    /*
     * Check the presence of the SYN flag.  If the SYN flag is not set
     * then return and continue the processing associated with this
     * state.
     */
    if (!(msg->control & RATP_CONTROL_SYN))
        return true;

    /*
     * We now assume that the SYN flag was set.  The presence of a SYN
     * here is an error.  Flush the retransmission queue, send a legal
     * RST packet.
     *
     * If the ACK flag was set then send:
     *
     * <SN=received AN><CTL=RST>
     *
     * If the ACK flag was not set then send:
     *
     * <SN=0><CTL=RST>
     *
     * The user should receive the message "Error: Connection reset.",
     * then delete the TCB and go to the CLOSED state without any
     * further processing.
     */
    stop_retransmission_logic (self, false);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);

    ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
    if (msg->control & RATP_CONTROL_ACK)
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
    send_tx_buffer (self);

    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_RESET);
    return false;
}

static bool
dispatch_message_behavior_f1 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior F1]");

    /*
     * Check the presence of the ACK flag.  If ACK is not set then
     * discard the packet and return without any further processing.
     */
    if (!(msg->control & RATP_CONTROL_ACK))
        return false;

    /*
     * We now assume that the ACK flag was set.  If the AN field value
     * was as expected then return and continue the processing
     * associated with this state.
     */

    if (ratp_message_validate_an (msg, self->sn_sent)) {
        stop_retransmission_logic (self, true);
        return true;
    }

    /*
     * We now assume that the ACK flag was set and that the AN field
     * value was unexpected.  If the connection was originally
     * initiated from the LISTEN state (it was passively opened) then
     * flush the retransmission queue, discard the packet, and send a
     * legal RST packet:
     *
     * <SN=received AN><CTL=RST>
     *
     * Then delete the TCB and go to the LISTEN state without any
     * further processing.
     */
    if (!self->active) {
        stop_retransmission_logic (self, false);
        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
        link_update_state_locked (self, RATP_LINK_STATE_LISTEN, RATP_STATUS_OK);

        ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
        send_tx_buffer (self);
        return false;
    }

    /*
     * Otherwise the connection was initiated actively (came from the
     * SYN-SENT state) then inform the user "Error: Connection
     * refused", flush the retransmission queue, discard the packet,
     * and send a legal RST packet:
     *
     * <SN=received AN><CTL=RST>
     *
     * Then delete the TCB and go to the CLOSED state without any
     * further processing.
     */
    stop_retransmission_logic (self, false);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_REFUSED);

    ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
    ratp_message_set_sn (rsp, ratp_message_get_an (msg));
    send_tx_buffer (self);

    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_REFUSED);
    return false;
}

static bool
dispatch_message_behavior_f2 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior F2]");

    /*
     * Check the presence of the ACK flag.  If ACK is not set then
     * discard the packet and return without any further processing.
     */
    if (!(msg->control & RATP_CONTROL_ACK))
        return false;

    /* We now assume that the ACK flag was set.  If the AN field value
     * was as expected then flush the retransmission queue and inform
     * the user with an "Ok" if a buffer has been entirely
     * acknowledged.  Another packet containing data may now be sent.
     * Return and continue the processing associated with this state.
     */
    if (ratp_message_validate_an (msg, self->sn_sent)) {
        struct ratp_data_s *current;

        stop_retransmission_logic (self, true);

        /* Notify data sent successfully */
        current = self->outgoing_data_queue_first;
        if (current && (current->buffer_size == current->buffer_sent)) {
            remove_from_outgoing_data_queue_locked (self, current);
            send_complete (self, current, RATP_STATUS_OK);
        }
        return true;
    }

    /*
     * We now assume that the ACK flag was set and that the AN field
     * value was unexpected.  This is assumed to indicate a duplicate
     * acknowledgment.  It is ignored, return and continue the
     * processing associated with this state.
     */
    return true;
}

static bool
dispatch_message_behavior_f3 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior F3]");

    /*
     * Check the presence of the ACK flag.  If ACK is not set then
     * discard the packet and return without any further processing.
     */
    if (!(msg->control & RATP_CONTROL_ACK))
        return false;

    /*
     * We now assume that the ACK flag was set.  If the AN field value
     * was as expected then continue the processing associated with
     * this state.
     */
    if (ratp_message_validate_an (msg, self->sn_sent)) {
        stop_retransmission_logic (self, true);
        return true;
    }

    /*
     * We now assume that the ACK flag was set and that the AN field
     * value was unexpected.  This is ignored, return and continue
     * with the processing associated with this state.
     */
    return true;
}

static void
dispatch_message_behavior_g (ratp_link_t                 *self,
                             const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior G]");

    /*
     * This procedure represents the behavior of the CLOSED state of a
     * connection.  All incoming packets are discarded.  If the packet
     * had the RST flag set take no action.
     */
    if (msg->control & RATP_CONTROL_RST)
        return;

    /* Otherwise it is necessary
     * to build a RST packet.  Since this end is closed the other end
     * of the connection has incorrect data about the state of the
     * connection and should be so informed.
     *
     * If the ACK flag was set then send:
     *
     * <SN=received AN><CTL=RST>
     *
     * If the ACK flag was not set then send:
     *
     * <SN=0><AN=received SN+1 modulo 2><CTL=RST, ACK>
     *
     * After sending the reset packet return to the current state
     * without any further processing.
     */
    stop_retransmission_logic (self, false);
    if (msg->control & RATP_CONTROL_ACK) {
        ratp_message_header_init (rsp, RATP_CONTROL_RST, 0);
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
    } else {
        ratp_message_header_init (rsp, RATP_CONTROL_RST | RATP_CONTROL_ACK, 0);
        ratp_message_set_next_an (rsp, ratp_message_get_sn (msg));
    }
    send_tx_buffer (self);
}

static void common_behavior_i1 (ratp_link_t                 *self,
                                const struct ratp_message_s *msg);

static void
dispatch_message_behavior_h1 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior H1]");

    /*
     * Our SYN has been acknowledged.  At this point we are
     * technically in the ESTABLISHED state.  Send any initial data
     * which is queued to send:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK><DATA>
     *
     * Go to the ESTABLISHED state and execute procedure I1 to process
     * any data which might be in this packet.
     */

    link_update_state_locked (self, RATP_LINK_STATE_ESTABLISHED, RATP_STATUS_OK);

    /* If the input message has data (i.e. it is not just an ACK without data) then
     * we need to send back an ACK ourselves, or even data if we have it pending.
     * This is the same procedure done in i1, so just run it. */
    common_behavior_i1 (self, msg);

    /*
     * Any packet not satisfying the above tests is discarded and
     * ignored.  Return to the current state without any further
     * processing.
     */
}

static bool
dispatch_message_behavior_h2 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    ratp_debug ("[behavior H2]");

    /*
     * Check the presence of the FIN flag.  If FIN is not set then
     * continue the processing associated with this state.
     */
    if (!(msg->control & RATP_CONTROL_FIN))
        return true;

    /*
     * We now assume that the FIN flag was set.  This means the other
     * end has decided to close the connection.  Flush the
     * retransmission queue.  If any data remains to be sent then
     * inform the user "Warning: Data left unsent."  The user must
     * also be informed "Connection closing."  An acknowledgment for
     * the FIN must be sent which also indicates this end is closing:
     *
     * <SN=received AN><AN=received SN + 1 modulo 2><CTL=FIN, ACK>
     */
    stop_retransmission_logic (self, false);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_DATA_LEFT_UNSENT);

    ratp_message_header_init (rsp, RATP_CONTROL_FIN | RATP_CONTROL_ACK, 0);
    ratp_message_set_sn (rsp, ratp_message_get_an (msg));
    ratp_message_set_next_an (rsp, self->sn_received);
    send_tx_buffer (self);

    /*
     * Go to the LAST-ACK state without any further processing.
     */
    link_update_state_locked (self, RATP_LINK_STATE_LAST_ACK, RATP_STATUS_CONNECTION_CLOSING);
    return false;
}

static bool
dispatch_message_behavior_h3 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior H3]");

    /*
     * This state represents the final behavior of the FIN-WAIT state.
     *
     * If the packet did not contain a FIN we assume this packet is a
     * duplicate and that the other end of the connection has not seen
     * the FIN packet we sent earlier.  Rely upon retransmission of
     * our earlier FIN packet to inform the other end of our desire to
     * close.  Discard the packet and return without any further
     * processing.
     */
    if (!(msg->control & RATP_CONTROL_FIN))
        return false;

    /*
     * At this point we have a packet which should contain a FIN.  By
     * the rules of this protocol an ACK of a FIN requires a FIN, ACK
     * in response and no data.  If the packet contains data we have
     * detected an illegal condition.  Send a reset:
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=RST, ACK>
     *
     * Discard the packet, flush the retransmission queue, inform the
     * ser "Error: Connection reset.", delete the TCB, and go to the
     * CLOSED state without any further processing.
     */
    if (ratp_message_has_data (msg)) {
        struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

        stop_retransmission_logic (self, false);
        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);

        ratp_message_header_init (rsp, RATP_CONTROL_RST | RATP_CONTROL_ACK, 0);
        ratp_message_set_sn (rsp, ratp_message_get_an (msg));
        ratp_message_set_next_an (rsp, self->sn_received);
        send_tx_buffer (self);

        reset_tcb (self);
        link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_RESET);
        return false;
    }

    /*
     * We now assume that the FIN flag was set and no data was
     * contained in the packet.  If the AN field value was expected
     * then this packet acknowledges a previously sent FIN packet.
     * The other end of the connection is then also assumed to be
     * closing and expects an acknowledgment.  Send an acknowledgment
     * of the FIN:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     *
     * Start the 2*SRTT timer associated with the TIME-WAIT state,
     * discard the packet, and go to the TIME-WAIT state without any
     * further processing.
     */
    if (ratp_message_validate_an (msg, self->sn_sent)) {
        send_ack_no_data (self, msg);
        start_time_wait_logic (self);
        link_update_state_locked (self, RATP_LINK_STATE_TIME_WAIT, RATP_STATUS_OK);
        return false;
    }

    /*
     * Otherwise the AN field value was unexpected.  This indicates a
     * simultaneous closing by both sides of the connection.  Send an
     * acknowledgment of the FIN:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     *
     * Discard the packet, and go to the CLOSING state without any
     * further processing.
     */
    send_ack_no_data (self, msg);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSING, RATP_STATUS_OK);
    return false;
}

static bool
dispatch_message_behavior_h4 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior H4]");

    /*
     * This state represents the final behavior of the LAST-ACK state.
     *
     * If the AN field value is expected then this ACK is in response
     * to the FIN, ACK packet recently sent.  This is the final
     * acknowledging message indicating both side's agreement to close
     * the connection.  Discard the packet, flush all queues, delete
     * the TCB, and go to the CLOSED state without any further
     * processing.
     */
    if (ratp_message_validate_an (msg, self->sn_sent)) {
        stop_retransmission_logic (self, false);
        flush_outgoing_data_queue_locked (self, RATP_STATUS_DATA_LEFT_UNSENT);
        reset_tcb (self);
        link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_OK);
        return false;
    }

    /*
     * Otherwise the AN field value was unexpected.  Discard the
     * packet and remain in the current state without any further
     * processing.
     */
    return false;
}

static bool
dispatch_message_behavior_h5 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior H5]");

    /*
     * This state represents the final behavior of the CLOSING state.
     *
     * If the AN field value was expected then this packet
     * acknowledges the FIN packet recently sent.  This is the final
     * acknowledging message indicating both side's agreement to close
     * the connection.  Start the 2*SRTT timer associated with the
     * TIME-WAIT state, discard the packet, and go to the TIME-WAIT
     * state without any further processing.
     */
    if (ratp_message_validate_an (msg, self->sn_sent)) {
        start_time_wait_logic (self);
        link_update_state_locked (self, RATP_LINK_STATE_TIME_WAIT, RATP_STATUS_OK);
        return false;
    }

    /*
     * Otherwise the AN field value was unexpected.  Discard the
     * packet and remain in the current state without any further
     * processing.
     */
    return false;
}

static void
dispatch_message_behavior_h6 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior H6]");

    /*
     * This state represents the behavior of the TIME-WAIT state.
     * Check the presence of the ACK flag.  If ACK is not set then
     * discard the packet and return without any further processing.
     */
    if (!(msg->control & RATP_CONTROL_ACK))
        return;

    /*
     * Check the presence of the FIN flag.  If FIN is not set then
     * discard the packet and return without any further processing.
     */
    if (!(msg->control & RATP_CONTROL_FIN))
        return;

    /*
     * We now assume that the FIN flag was set.  This situation
     * indicates that the last acknowledgment of the FIN packet sent
     * by the other end of the connection did not arrive.  Resend the
     * acknowledgment:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     */
    send_ack_no_data (self, msg);

    /*
     * Restart the 2*SRTT timer, discard the packet, and remain in the
     * current state without any further processing.
     */
    stop_time_wait_logic (self);
    start_time_wait_logic (self);
}

static void
common_behavior_i1 (ratp_link_t                 *self,
                    const struct ratp_message_s *msg)
{
    /*
     * This represents that stage of processing in the ESTABLISHED
     * state in which all the flag bits have been processed and only
     * data may remain.  The packet is examined to see if it contains
     * data.  If not the packet is now discarded, return to the
     * current state without any further processing.
     */
    if (!ratp_message_has_data (msg))
        return;

    /* Packets with just an ACK aren't considered for SN */
    self->sn_received = ratp_message_get_sn (msg);

    /*
     * We assume the packet contained data, that either the SO flag
     * was set or LENGTH is positive.  That data is placed into the
     * user's receive buffers.  As these become full the user should
     * be informed "Receive buffer full."
     */
    common_process_incoming_data (self, msg);

    /* An acknowledgment is sent:
     *
     * <SN=received AN><AN=received SN+1 modulo 2><CTL=ACK>
     *
     * If data is queued to send then it is most efficient to
     * 'piggyback' this acknowledgment on that data packet.
     */

    if (!self->outgoing_data_queue_first || !send_next_data_locked (self))
        send_ack_no_data (self, msg);
}

static void
dispatch_message_behavior_i1 (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    ratp_debug ("[behavior I1]");
    common_behavior_i1 (self, msg);
}

static void
dispatch_message_listen (ratp_link_t                 *self,
                         const struct ratp_message_s *msg)
{
    dispatch_message_behavior_a (self, msg);
}

static void
dispatch_message_syn_sent (ratp_link_t                 *self,
                           const struct ratp_message_s *msg)
{
    dispatch_message_behavior_b (self, msg);
}

static void
dispatch_message_syn_received (ratp_link_t                 *self,
                               const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_c1 (self, msg))
        return;
    if (!dispatch_message_behavior_d1 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f1 (self, msg))
        return;
    dispatch_message_behavior_h1 (self, msg);
}

static void
dispatch_message_established (ratp_link_t                 *self,
                              const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_c2 (self, msg))
        return;
    if (!dispatch_message_behavior_d2 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f2 (self, msg))
        return;
    if (!dispatch_message_behavior_h2 (self, msg))
        return;
    dispatch_message_behavior_i1 (self, msg);
}

static void
dispatch_message_fin_wait (ratp_link_t                 *self,
                           const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_c2 (self, msg))
        return;
    if (!dispatch_message_behavior_d2 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f3 (self, msg))
        return;
    dispatch_message_behavior_h3 (self, msg);
}

static void
dispatch_message_last_ack (ratp_link_t                 *self,
                           const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_c2 (self, msg))
        return;
    if (!dispatch_message_behavior_d3 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f3 (self, msg))
        return;
    dispatch_message_behavior_h4 (self, msg);
}

static void
dispatch_message_closing (ratp_link_t                 *self,
                          const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_c2 (self, msg))
        return;
    if (!dispatch_message_behavior_d3 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f3 (self, msg))
        return;
    dispatch_message_behavior_h5 (self, msg);
}

static void
dispatch_message_time_wait (ratp_link_t                 *self,
                            const struct ratp_message_s *msg)
{
    if (!dispatch_message_behavior_d3 (self, msg))
        return;
    if (!dispatch_message_behavior_e (self, msg))
        return;
    if (!dispatch_message_behavior_f3 (self, msg))
        return;
    dispatch_message_behavior_h6 (self, msg);
}

static void
dispatch_message_closed (ratp_link_t                 *self,
                         const struct ratp_message_s *msg)
{
    dispatch_message_behavior_g (self, msg);
}

typedef void (* dispatch_message_func) (ratp_link_t                 *self,
                                        const struct ratp_message_s *msg);

static const dispatch_message_func dispatch_message_funcs[] = {
    [RATP_LINK_STATE_LISTEN]       = dispatch_message_listen,
    [RATP_LINK_STATE_SYN_SENT]     = dispatch_message_syn_sent,
    [RATP_LINK_STATE_SYN_RECEIVED] = dispatch_message_syn_received,
    [RATP_LINK_STATE_ESTABLISHED]  = dispatch_message_established,
    [RATP_LINK_STATE_FIN_WAIT]     = dispatch_message_fin_wait,
    [RATP_LINK_STATE_LAST_ACK]     = dispatch_message_last_ack,
    [RATP_LINK_STATE_CLOSING]      = dispatch_message_closing,
    [RATP_LINK_STATE_TIME_WAIT]    = dispatch_message_time_wait,
    [RATP_LINK_STATE_CLOSED]       = dispatch_message_closed,
};

/******************************************************************************/

void
ratp_link_free (ratp_link_t *self)
{
    if (self) {
        ratp_link_shutdown (self);
        pthread_mutex_destroy (&self->mutex);
        self->backend->free (self);
    }
}

static void
cleanup_event (struct event **pevent, short events)
{
    assert (pevent);
    if (*pevent) {
        if (event_pending (*pevent, events, NULL))
            event_del (*pevent);
        event_free (*pevent);
        *pevent = NULL;
    }
}

static void
internal_shutdown (ratp_link_t *self)
{
    stop_retransmission_logic (self, false);

    cleanup_event (&self->input,     EV_READ);
    cleanup_event (&self->keepalive, EV_TIMEOUT);

    if (self->base) {
        event_base_free (self->base);
        self->base = NULL;
    }

    self->backend->shutdown (self);
}

static void
keepalive_timeout (evutil_socket_t  fd,
                   short            events,
                   ratp_link_t     *self)
{
    pthread_mutex_lock (&self->mutex);
    ratp_debug ("ratp running:");
    ratp_debug ("  link state '%s'", ratp_link_state_str (self->state));
    pthread_mutex_unlock (&self->mutex);
}

static ssize_t
process_input_message (ratp_link_t           *self,
                       struct ratp_message_s *msg,
                       size_t                 msg_size)
{
    size_t full_expected_message_size;

    /* Got full header? */
    if (msg_size < sizeof (struct ratp_message_s))
        return 0;

    /* Validate header checksum */
    if (!ratp_message_header_validate (msg)) {
        ratp_error ("message header cksum failed");
        return sizeof (struct ratp_message_s);
    }

    /* Got full message? */
    full_expected_message_size = ratp_message_get_size (msg);
    if (msg_size < full_expected_message_size)
        return 0;

    /* Validate data checksum */
    if (!ratp_message_data_validate (msg)) {
        ratp_error ("message data cksum failed");
        return full_expected_message_size;
    }

    /* Inject received message in state machine */
    ratp_message_debug (msg, "message received");

    pthread_mutex_lock (&self->mutex);
    dispatch_message_funcs[self->state] (self, msg);
    pthread_mutex_unlock (&self->mutex);

    return full_expected_message_size;
}

static void
input_ready (evutil_socket_t  fd,
             short            events,
             ratp_link_t     *self)
{
    while (1) {
        ssize_t r;

        r = read (fd, (void *) &self->rx_buffer[self->rx_buffer_n], 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            /* Other errors, fatal, discard everything */
            self->rx_buffer_n = 0;
            ratp_error ("couldn't read from input: %s", strerror (errno));
            return;
        }

        if (r == 0)
            break;

        assert (r == 1);

        /* If waiting for the SYNCH byte, discard data until we get it */
        if ((self->rx_buffer_n == 0) && (self->rx_buffer[0] != RATP_SYNCH_LEADER)) {
            ratp_debug ("discarded byte: 0x%02x (expecting SYNCH)", self->rx_buffer[0]);
            continue;
        }

        self->rx_buffer_n += r;

        if ((r = process_input_message (self, (struct ratp_message_s *) self->rx_buffer, self->rx_buffer_n)) > 0) {
            /* If we got closed while processing this message, rx_buffer_n will be reset already */
            if (self->rx_buffer_n > 0) {
                if (r == self->rx_buffer_n) {
                    self->rx_buffer_n = 0;
                } else if (r < self->rx_buffer_n) {
                    self->rx_buffer_n = self->rx_buffer_n - r;
                    memmove (&self->rx_buffer[0], &self->rx_buffer[r], self->rx_buffer_n);
                } else
                    assert (r <= self->rx_buffer_n);
            }
        }
    }
}

static void *
thread_start (void *user_data)
{
    ratp_link_t *self = (ratp_link_t *) user_data;

    ratp_debug ("private thread started");
    event_base_dispatch (self->base);
    ratp_debug ("private thread finished");

    self->tid = 0;
    return NULL;
}

ratp_status_t
ratp_link_initialize (ratp_link_t *self)
{
    static const struct timeval keepalive_time = {
        .tv_sec  = DEFAULT_KEEPALIVE_TIMEOUT_S,
        .tv_usec = 0,
    };

    ratp_status_t st;

    pthread_mutex_lock (&self->mutex);

    ratp_debug ("initialization requested");

    if (self->tid) {
        st = RATP_STATUS_INVALID_TRANSITION;
        goto out;
    }

    if ((st = self->backend->initialize (self)) != RATP_STATUS_OK)
        goto out;

    self->base = event_base_new ();
    if (!self->base) {
        st = RATP_STATUS_ERROR_NO_MEMORY;
        goto out;
    }

    self->keepalive = event_new (self->base, -1, EV_TIMEOUT | EV_PERSIST, (event_callback_fn) keepalive_timeout, self);
    if (!self->keepalive) {
        st = RATP_STATUS_ERROR_NO_MEMORY;
        goto out;
    }

    if (event_add (self->keepalive, &keepalive_time) < 0) {
        st = RATP_STATUS_ERROR;
        goto out;
    }

    self->input = event_new (self->base, self->backend->get_in_fd (self), EV_READ | EV_PERSIST, (event_callback_fn) input_ready, self);
    if (!self->input) {
        st = RATP_STATUS_ERROR_NO_MEMORY;
        goto out;
    }

    if (event_add (self->input, NULL) < 0) {
        st = RATP_STATUS_ERROR;
        goto out;
    }

    if (pthread_create (&self->tid, NULL, thread_start, self) != 0) {
        st = RATP_STATUS_ERROR;
        goto out;
    }

    st = RATP_STATUS_OK;

out:
    if (st != RATP_STATUS_OK)
        internal_shutdown (self);

    pthread_mutex_unlock (&self->mutex);
    return st;
}

void
ratp_link_shutdown (ratp_link_t *self)
{
    pthread_t stop_tid = 0;

    ratp_debug ("shutdown requested");

    pthread_mutex_lock (&self->mutex);
    {
        if (self->tid) {
            stop_tid = self->tid;
            event_base_loopexit (self->base, NULL);
        }
    }
    pthread_mutex_unlock (&self->mutex);

    if (stop_tid) {
        pthread_join (stop_tid, NULL);
        internal_shutdown (self);
    }
}

/******************************************************************************/

ratp_link_state_t
ratp_link_get_state (ratp_link_t *self)
{
    ratp_link_state_t st;

    pthread_mutex_lock (&self->mutex);
    st = self->state;
    pthread_mutex_unlock (&self->mutex);

    return st;
}

void
ratp_link_set_state_update_callback (ratp_link_t                 *self,
                                     ratp_link_state_update_func  callback,
                                     void                        *user_data)
{
    pthread_mutex_lock (&self->mutex);
    self->state_update_callback           = callback;
    self->state_update_callback_user_data = user_data;
    pthread_mutex_unlock (&self->mutex);
}

/******************************************************************************/

ratp_status_t
ratp_link_active_open (ratp_link_t *self)
{
    struct ratp_message_s req;

    ratp_debug ("link active open requested...");

    pthread_mutex_lock (&self->mutex);
    if (self->state != RATP_LINK_STATE_CLOSED) {
        pthread_mutex_unlock (&self->mutex);
        ratp_warning ("link must be closed for active open to succeed");
        return RATP_STATUS_INVALID_TRANSITION;
    }

    ratp_debug ("sending SYN...");
    ratp_message_header_init (&req, RATP_CONTROL_SYN, self->local_mdl);
    send_message (self, &req);

    /* Flag as having initiated an active open */
    self->active = true;

    link_update_state_locked (self, RATP_LINK_STATE_SYN_SENT, RATP_STATUS_OK);
    pthread_mutex_unlock (&self->mutex);
    return RATP_STATUS_OK;
}

ratp_status_t
ratp_link_passive_open (ratp_link_t *self)
{
    ratp_debug ("link passive open requested...");

    pthread_mutex_lock (&self->mutex);
    if (self->state != RATP_LINK_STATE_CLOSED) {
        pthread_mutex_unlock (&self->mutex);
        ratp_warning ("link must be closed for passive open to succeed");
        return RATP_STATUS_INVALID_TRANSITION;
    }

    link_update_state_locked (self, RATP_LINK_STATE_LISTEN, RATP_STATUS_OK);
    pthread_mutex_unlock (&self->mutex);
    return RATP_STATUS_OK;
}

/******************************************************************************/

void
ratp_link_close (ratp_link_t *self)
{
    struct ratp_message_s *rsp = (struct ratp_message_s *)(&self->tx_buffer[0]);

    pthread_mutex_lock (&self->mutex);

    /*
     * The user is allowed to request CLOSE operations in several states:
     *
     *  - In the LISTEN state, the state would go directly to CLOSED.
     *  - In the SYN-SENT state, the state would go directly to CLOSED.
     *  - In the SYN-RECEIVED state, a FIN packet is sent and the state goes to FIN-WAIT.
     *  - In the ESTABLISHED state, a FIN packet is sent and the state goes to FIN-WAIT.
     */

    if (self->state == RATP_LINK_STATE_ESTABLISHED || self->state == RATP_LINK_STATE_SYN_RECEIVED) {
        stop_retransmission_logic (self, false);
        /* NOTE: instead of sending a packet with a single FIN flag set, we piggyback the last
         * ACK we have sent to the peer. This must be done so that the F2 behavior in the peer
         * doesn't filter out the packet completely before H2 behavior is run, which is where
         * the FIN flag is checked. The spec isn't very clear about this, and it even shows
         * packet flows with FIN only set, which wouldn't go through the F2->H2 steps. */
        ratp_message_header_init (rsp, RATP_CONTROL_FIN | RATP_CONTROL_ACK, 0);
        ratp_message_set_next_sn (rsp, self->sn_sent);
        ratp_message_set_next_an (rsp, self->sn_received);
        send_tx_buffer (self);

        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
        link_update_state_locked (self, RATP_LINK_STATE_FIN_WAIT, RATP_STATUS_OK);
    } else if (self->state == RATP_LINK_STATE_LISTEN || self->state == RATP_LINK_STATE_SYN_SENT) {
        stop_retransmission_logic (self, false);
        flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_RESET);
        link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_OK);
    }

    pthread_mutex_unlock (&self->mutex);
}

/******************************************************************************/

static void
send_complete (ratp_link_t        *self,
               struct ratp_data_s *data,
               ratp_status_t       status)
{
    assert (!data->next);
    assert (!data->prev);

    ratp_debug ("send operation completed: %s", ratp_status_str (status));

    if (data->ready_func)
        data->ready_func (self, status, data->user_data);

    if (data->user_timeout) {
        event_del (data->user_timeout);
        event_free (data->user_timeout);
    }
    free (data);
}

static void
remove_from_outgoing_data_queue_locked (ratp_link_t        *self,
                                        struct ratp_data_s *data)
{
    if (data == self->outgoing_data_queue_first)
        self->outgoing_data_queue_first = data->next;
    else
        data->prev->next = data->next;

    if (data == self->outgoing_data_queue_last)
        self->outgoing_data_queue_last = data->prev;
    else
        data->next->prev = data->prev;

    data->next = NULL;
    data->prev = NULL;
}

static void
remove_from_outgoing_data_queue (ratp_link_t        *self,
                                 struct ratp_data_s *data)
{
    pthread_mutex_lock (&self->mutex);
    remove_from_outgoing_data_queue_locked (self, data);
    pthread_mutex_unlock (&self->mutex);
}

static void
flush_outgoing_data_queue_locked (ratp_link_t   *self,
                                  ratp_status_t  reason)
{
    struct ratp_data_s *current;

    while ((current = self->outgoing_data_queue_first) != NULL) {
        remove_from_outgoing_data_queue_locked (self, current);
        send_complete (self, current, reason);
    }
    assert (!self->outgoing_data_queue_first);
    assert (!self->outgoing_data_queue_last);
}

static bool
send_next_data_locked (ratp_link_t *self)
{
    struct ratp_message_s *msg;
    struct ratp_data_s    *current;
    uint8_t                control;
    size_t                 data_chunk_size;

    /* No data transmission if not yet established */
    if (self->state != RATP_LINK_STATE_ESTABLISHED)
        return false;

    /* An ongoing packet is already being sent */
    if (self->tx_retransmission_timeout)
        return false;

    /* The peer may have said no data is welcome */
    if (!self->peer_mdl) {
        flush_outgoing_data_queue_locked (self, RATP_STATUS_DATA_REFUSED);
        return false;
    }

    /* Data scheduled? */
    current = self->outgoing_data_queue_first;
    if (!current)
        return false;

    /* Place the data message directly in the TX buffer, to avoid unnecessary copies */
    msg = (struct ratp_message_s *)(&self->tx_buffer[0]);

    data_chunk_size = MIN (self->peer_mdl, (current->buffer_size - current->buffer_sent));
    assert (data_chunk_size > 0 && data_chunk_size <= 255);

    /* NOTE: we may already have ACK-ed independently the last received packet, but we
     * force here piggybacking the last ACK we have sent to the peer. This must be done
     * so that the F2 behavior in the peer doesn't filter out the packet completely
     * before I1 behavior is run, which is where the data is processed. The spec isn't
     * very clear about this. */
    control = RATP_CONTROL_ACK;

    /* Add EOR automatically for the last chunk of the current data */
    if ((current->buffer_sent + data_chunk_size) == current->buffer_size)
        control |= RATP_CONTROL_EOR;

    if (data_chunk_size == 1) {
        /* Automatically switch to SO if single byte being sent */
        control |= RATP_CONTROL_SO;
        ratp_message_header_init (msg, control, current->buffer[current->buffer_sent]);
    } else {
        ratp_message_header_init (msg, control, data_chunk_size);
        memcpy (msg->data, &current->buffer[current->buffer_sent], data_chunk_size);
    }

    current->buffer_sent += data_chunk_size;
    ratp_debug ("sending %zu bytes of data (total: %zu/%zu)",
                data_chunk_size, current->buffer_sent, current->buffer_size);

    ratp_message_set_next_sn (msg, self->sn_sent);
    ratp_message_set_next_an (msg, self->sn_received);

    send_tx_buffer (self);
    return true;
}

static void
send_next_data (ratp_link_t *self)
{
    pthread_mutex_lock (&self->mutex);
    send_next_data_locked (self);
    pthread_mutex_unlock (&self->mutex);
}

static void
append_to_outgoing_data_queue (ratp_link_t        *self,
                               struct ratp_data_s *data)
{
    assert (!data->next);
    assert (!data->prev);

    pthread_mutex_lock (&self->mutex);
    {
        if (self->outgoing_data_queue_last) {
            data->prev = self->outgoing_data_queue_last;
            self->outgoing_data_queue_last->next = data;
            self->outgoing_data_queue_last = data;
        } else {
            assert (!self->outgoing_data_queue_first);
            self->outgoing_data_queue_first = self->outgoing_data_queue_last = data;
        }
    }
    pthread_mutex_unlock (&self->mutex);
}

static void
send_user_timeout (evutil_socket_t     fd,
                   short               events,
                   struct ratp_data_s *data)
{
    ratp_link_t *self;

    self = data->self;

    remove_from_outgoing_data_queue (self, data);
    ratp_warning ("data user timeout expired: %zu/%zu bytes sent", data->buffer_sent, data->buffer_size);
    send_complete (self, data, RATP_STATUS_CONNECTION_ABORTED_USER_TIMEOUT);

    pthread_mutex_lock (&self->mutex);
    flush_outgoing_data_queue_locked (self, RATP_STATUS_CONNECTION_ABORTED_USER_TIMEOUT);
    stop_retransmission_logic (self, false);
    reset_tcb (self);
    link_update_state_locked (self, RATP_LINK_STATE_CLOSED, RATP_STATUS_CONNECTION_ABORTED_USER_TIMEOUT);
    pthread_mutex_unlock (&self->mutex);
}

ratp_status_t
ratp_link_send (ratp_link_t               *self,
                unsigned long              timeout_ms,
                const uint8_t             *buffer,
                size_t                     buffer_size,
                ratp_link_send_ready_func  callback,
                void                      *user_data)
{
    struct ratp_data_s *data;
    bool                allow_send = false;

    /* We only allow queueing data to send IF we're not closed or in the process
     * of being closed. Note that this also means that we're allowed to queue
     * data as soon as we have requested an active/passive OPEN. */
    pthread_mutex_lock (&self->mutex);
    allow_send = (self->state <= RATP_LINK_STATE_ESTABLISHED);
    pthread_mutex_unlock (&self->mutex);

    if (!allow_send)
        return RATP_STATUS_INVALID_TRANSITION;

    /* Allocate data structure */
    data = calloc (sizeof (struct ratp_data_s) + buffer_size, 1);
    if (!data)
        return RATP_STATUS_ERROR_NO_MEMORY;

    data->self        = self;
    data->buffer_size = buffer_size;
    data->ready_func  = callback;
    data->user_data   = user_data;

    memcpy (data->buffer, buffer, buffer_size);

    /* Schedule user timeout */
    if (timeout_ms) {
        struct timeval timeout;

        data->user_timeout = event_new (self->base, -1, EV_TIMEOUT, (event_callback_fn) send_user_timeout, data);
        if (!data->user_timeout) {
            free (data);
            return RATP_STATUS_ERROR_NO_MEMORY;
        }

        timeout.tv_sec  = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        if (event_add (data->user_timeout, &timeout) < 0) {
            event_free (data->user_timeout);
            free (data);
            return RATP_STATUS_ERROR;
        }
    }

    append_to_outgoing_data_queue (self, data);
    ratp_debug ("data scheduled to be sent: %zu bytes", buffer_size);

    send_next_data (self);

    return RATP_STATUS_OK;
}
