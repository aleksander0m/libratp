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

struct ratp_link_tty_s {
    struct ratp_link_s  parent;
    char               *path;
    int                 fd;
    speed_t             baudrate;
};

static void
backend_free (ratp_link_t *_self)
{
    struct ratp_link_tty_s *self = (struct ratp_link_tty_s *)_self;

    free (self->path);
    free (self);
}

static ratp_status_t
backend_initialize (ratp_link_t *_self)
{
    struct ratp_link_tty_s *self = (struct ratp_link_tty_s *)_self;
    struct termios          stbuf;

    if (self->fd != -1)
        return RATP_STATUS_ERROR;

    ratp_debug ("%s: opening tty...", self->path);

    /* Open for RW; we skip setting non-blocking mode here as we're using
     * VMIN/VTIME for the same purpose */
    if ((self->fd = open (self->path, O_RDWR | O_NOCTTY)) < 0) {
        ratp_error ("%s: couldn't open tty: %s", self->path, strerror (errno));
        return RATP_STATUS_ERROR;
    }

    /* Get port attributes */
    memset (&stbuf, 0, sizeof (struct termios));
    if (tcgetattr (self->fd, &stbuf) != 0) {
        ratp_error ("%s: couldn't get serial device attributes: %s", self->path, strerror (errno));
        return RATP_STATUS_ERROR;
    }

    /* No flow control of any kind */
    stbuf.c_iflag &= ~(IGNCR | ICRNL | IUCLC | INPCK | IXON | IXOFF | IXANY );

    /* Set into raw, no echo mode */
    stbuf.c_lflag = 0;
    stbuf.c_oflag = 0;

    /* Set up port speed and serial attributes; also ignore modem control
     * lines since most drivers don't implement RTS/CTS anyway.
     */
    stbuf.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | PARENB | CRTSCTS);
    stbuf.c_cflag |= (CS8 | CREAD | CLOCAL);

    /* Set read() behavior.
     * VMIN = 0 and VTIME = 0: a completely non-blocking read; the call is
     * satisfied immediately directly from the driver's input queue. */
    stbuf.c_cc[VMIN]  = 0;
    stbuf.c_cc[VTIME] = 0;
    stbuf.c_cc[VEOF]  = 1;

    /* Use the speed specified in the input */
    cfsetospeed (&stbuf, self->baudrate);
    cfsetispeed (&stbuf, self->baudrate);

    /* Set new port attributes */
    if (tcsetattr (self->fd, TCSANOW, &stbuf) < 0) {
        ratp_error ("%s: couldn't set serial device attributes: %s", self->path, strerror (errno));
        return RATP_STATUS_ERROR;
    }

    /* Flush any waiting IO */
    tcflush (self->fd, TCIOFLUSH);

    ratp_debug ("%s: tty open", self->path);
    return RATP_STATUS_OK;
}

static void
backend_shutdown (ratp_link_t *_self)
{
    struct ratp_link_tty_s *self = (struct ratp_link_tty_s *)_self;

    if (self->fd >= 0) {
        close (self->fd);
        self->fd = -1;
        ratp_debug ("[%s] closed tty", self->path);
    }
}

static int
backend_get_fd (ratp_link_t *_self)
{
    struct ratp_link_tty_s *self = (struct ratp_link_tty_s *)_self;

    return self->fd;
}

static void
backend_flush (ratp_link_t *_self)
{
    struct ratp_link_tty_s *self = (struct ratp_link_tty_s *)_self;

    tcflush (self->fd, TCOFLUSH);
}

static const struct ratp_backend_s tty_backend = {
    .free       = backend_free,
    .initialize = backend_initialize,
    .shutdown   = backend_shutdown,
    .get_in_fd  = backend_get_fd,
    .get_out_fd = backend_get_fd,
    .flush      = backend_flush,
};

/******************************************************************************/

ratp_link_t *
ratp_link_new_tty (const char *path,
                   speed_t     baudrate,
                   uint8_t     mdl)
{
    struct ratp_link_tty_s *self;

    self = calloc (sizeof (struct ratp_link_tty_s), 1);
    if (!self)
        return NULL;

    ratp_link_common_init ((ratp_link_t *) self, &tty_backend, mdl);

    self->path = strdup (path);
    if (!self->path) {
        free (self);
        return NULL;
    }

    self->fd       = -1;
    self->baudrate = baudrate;

    return (ratp_link_t *) self;
}
