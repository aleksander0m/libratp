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

#include <malloc.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "ratp.h"
#include "ratp-log.h"
#include "ratp-link.h"

/******************************************************************************/

struct ratp_link_fifo_s {
    struct ratp_link_s  parent;
    char               *path_in;
    char               *path_out;
    int                 fd_in;
    int                 fd_out;
};

static void
backend_free (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    free (self->path_in);
    free (self->path_out);
    free (self);
}

static ratp_status_t
backend_initialize (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    if ((self->fd_in != -1) || (self->fd_out != -1))
        return RATP_STATUS_ERROR;

    if ((self->fd_in = open (self->path_in, O_RDWR | O_NONBLOCK)) < 0) {
        ratp_error ("%s: couldn't open input fifo: %s", self->path_in, strerror (errno));
        return RATP_STATUS_ERROR;
    }
    ratp_debug ("%s: input fifo open", self->path_in);

    if ((self->fd_out = open (self->path_out, O_WRONLY)) < 0) {
        ratp_error ("%s: couldn't open output fifo: %s", self->path_out, strerror (errno));
        return RATP_STATUS_ERROR;
    }
    ratp_debug ("%s: output fifo open", self->path_out);

    return RATP_STATUS_OK;
}

static void
backend_shutdown (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    if (self->fd_in >= 0) {
        close (self->fd_in);
        self->fd_in = -1;
        ratp_debug ("%s: closed input fifo", self->path_in);
    }

    if (self->fd_out >= 0) {
        close (self->fd_out);
        self->fd_out = -1;
        ratp_debug ("%s: closed output fifo", self->path_out);
    }
}

static int
backend_get_in_fd (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    return self->fd_in;
}

static int
backend_get_out_fd (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    return self->fd_out;
}

static void
backend_flush (ratp_link_t *_self)
{
    struct ratp_link_fifo_s *self = (struct ratp_link_fifo_s *)_self;

    fsync (self->fd_out);
}

static const struct ratp_backend_s fifo_backend = {
    .free       = backend_free,
    .initialize = backend_initialize,
    .shutdown   = backend_shutdown,
    .get_in_fd  = backend_get_in_fd,
    .get_out_fd = backend_get_out_fd,
    .flush      = backend_flush,
};

/******************************************************************************/

ratp_link_t *
ratp_link_new_fifo (const char *path_in,
                    const char *path_out,
                    uint8_t     mdl)
{
    struct ratp_link_fifo_s *self;

    self = calloc (sizeof (struct ratp_link_fifo_s), 1);
    if (!self)
        return NULL;

    ratp_link_common_init ((ratp_link_t *) self, &fifo_backend, mdl);

    self->path_in  = strdup (path_in);
    self->path_out = strdup (path_out);
    self->fd_in    = -1;
    self->fd_out   = -1;

    if (!self->path_in || !self->path_out) {
        free (self->path_in);
        free (self->path_out);
        free (self);
        return NULL;
    }

    return (ratp_link_t *) self;
}
