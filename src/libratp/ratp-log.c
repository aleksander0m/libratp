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
#include <stdarg.h>
#include <stdint.h>

#include "ratp.h"
#include "ratp-log.h"

static ratp_log_handler_t default_handler = NULL;
static ratp_log_level_t   default_level   = RATP_LOG_LEVEL_ERROR;

static const char *level_str[] = {
    "error",
    "warn ",
    "info ",
    "debug"
};

const char *
ratp_log_level_str (ratp_log_level_t level)
{
    return level_str [level];
}

void
ratp_log_set_level (ratp_log_level_t level)
{
    default_level = level;
}

ratp_log_level_t
ratp_log_get_level (void)
{
    return default_level;
}

void
ratp_log_set_handler (ratp_log_handler_t handler)
{
    default_handler = handler;
}

void
ratp_log (ratp_log_level_t  level,
          unsigned long     tid,
          const char       *fmt,
          ...)
{
    char *message;
    va_list args;

    /* Only keep on if the log level allows us */
    if (level > ratp_log_get_level () || !default_handler)
      return;

    va_start (args, fmt);
    if (vasprintf (&message, fmt, args) == -1)
        return;
    va_end (args);

    default_handler (level, tid, message);
    free (message);
}
