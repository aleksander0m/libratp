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

#include <malloc.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>

#include "common.h"

/******************************************************************************/

char *
common_strhex (const void *mem,
               size_t      size,
               const char *delimiter)
{
    const uint8_t *data = mem;
    size_t         i, j, new_str_length, delimiter_length;
    char          *new_str;

    assert (size > 0);

    /* Allow delimiters of arbitrary sizes, including 0 */
    delimiter_length = (delimiter ? strlen (delimiter) : 0);

    /* Get new string length. If input string has N bytes, we need:
     * - 1 byte for last NUL char
     * - 2N bytes for hexadecimal char representation of each byte...
     * - N-1 times the delimiter length
     * So... e.g. if delimiter is 1 byte,  a total of:
     *   (1+2N+N-1) = 3N bytes are needed...
     */
    new_str_length =  1 + (2 * size) + ((size - 1) * delimiter_length);

    /* Allocate memory for new array and initialize contents to NUL */
    new_str = calloc (new_str_length, 1);

    /* Print hexadecimal representation of each byte... */
    for (i = 0, j = 0; i < size; i++, j += (2 + delimiter_length)) {
        /* Print character in output string... */
        snprintf (&new_str[j], 3, "%02x", data[i]);
        /* And if needed, add separator */
        if (delimiter_length && i != (size - 1) )
            strncpy (&new_str[j + 2], delimiter, delimiter_length);
    }

    /* Set output string */
    return new_str;
}

/******************************************************************************/

void
common_timespec_diff (struct timespec *start,
                      struct timespec *stop,
                      struct timespec *result)
{
    if ((stop->tv_nsec - start->tv_nsec) < 0) {
        result->tv_sec = stop->tv_sec - start->tv_sec - 1;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
    } else {
        result->tv_sec = stop->tv_sec - start->tv_sec;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec;
    }
}
