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

#include <event2/event.h>
#include <event2/thread.h>

#include "ratp.h"
#include "ratp-log.h"

/******************************************************************************/

static const char *status_str[] = {
    [RATP_STATUS_OK]                 = "ok",
    [RATP_STATUS_ERROR]              = "error",
    [RATP_STATUS_ERROR_NO_MEMORY]    = "no memory",
    [RATP_STATUS_INVALID_TRANSITION] = "invalid transition",
    [RATP_STATUS_WRITE_ERROR]        = "write error",
    [RATP_STATUS_READ_ERROR]         = "read error",
    [RATP_STATUS_TIMEOUT]            = "timeout",
    [RATP_STATUS_INVALID_DATA]       = "invalid data",
    [RATP_STATUS_DATA_REFUSED]       = "data refused",
    [RATP_STATUS_DATA_LEFT_UNSENT]   = "data left unsent",
    [RATP_STATUS_CONNECTION_REFUSED] = "connection refused",
    [RATP_STATUS_CONNECTION_RESET]   = "connection reset",
    [RATP_STATUS_CONNECTION_CLOSING] = "connection closing",
    [RATP_STATUS_CONNECTION_ABORTED_USER_TIMEOUT]           = "connection aborted: user timeout",
    [RATP_STATUS_CONNECTION_ABORTED_RETRANSMISSION_FAILURE] = "connection aborted: retransmission failure",
};

const char *
ratp_status_str (ratp_status_t status)
{
    return (status < (sizeof (status_str) / sizeof (status_str[0])) ? status_str[status] : "unknown");
}

/******************************************************************************/

static const char *link_state_str[] = {
    [RATP_LINK_STATE_LISTEN]       = "listen",
    [RATP_LINK_STATE_SYN_SENT]     = "syn-sent",
    [RATP_LINK_STATE_SYN_RECEIVED] = "syn-received",
    [RATP_LINK_STATE_ESTABLISHED]  = "established",
    [RATP_LINK_STATE_FIN_WAIT]     = "fin-wait",
    [RATP_LINK_STATE_LAST_ACK]     = "last-ack",
    [RATP_LINK_STATE_CLOSING]      = "closing",
    [RATP_LINK_STATE_TIME_WAIT]    = "time-wait",
    [RATP_LINK_STATE_CLOSED]       = "closed",
};

const char *
ratp_link_state_str (ratp_link_state_t state)
{
    return (state < (sizeof (link_state_str) / sizeof (link_state_str[0])) ? link_state_str[state] : "unknown");
}

/******************************************************************************/

ratp_status_t
ratp_init (void)
{
    /* Require pthreads locking in libevent */
    if (evthread_use_pthreads () < 0) {
        ratp_error ("cannot enable thread locking in libevent");
        return RATP_STATUS_ERROR;
    }

    return RATP_STATUS_OK;
}

/******************************************************************************/
/* Library version info */

unsigned int
ratp_get_major_version (void)
{
    return RATP_MAJOR_VERSION;
}

unsigned int
ratp_get_minor_version (void)
{
    return RATP_MINOR_VERSION;
}

unsigned int
ratp_get_micro_version (void)
{
    return RATP_MICRO_VERSION;
}
