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

#ifndef RATP_LOG_H
#define RATP_LOG_H

#include <pthread.h>

#include "ratp.h"

#define ratp_error(...)   ratp_log (RATP_LOG_LEVEL_ERROR,   (unsigned long) pthread_self (), ## __VA_ARGS__ )
#define ratp_warning(...) ratp_log (RATP_LOG_LEVEL_WARNING, (unsigned long) pthread_self (), ## __VA_ARGS__ )
#define ratp_info(...)    ratp_log (RATP_LOG_LEVEL_INFO,    (unsigned long) pthread_self (), ## __VA_ARGS__ )
#define ratp_debug(...)   ratp_log (RATP_LOG_LEVEL_DEBUG,   (unsigned long) pthread_self (), ## __VA_ARGS__ )

void ratp_log (ratp_log_level_t  level,
               unsigned long     tid,
               const char       *fmt,
               ...)
    __attribute__ ((__format__ (__printf__, 3, 4)))
    __attribute__ ((visibility ("hidden")));

#endif /* RATP_LOG_H */
