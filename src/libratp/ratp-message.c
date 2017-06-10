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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <endian.h>
#include <malloc.h>

#include <common.h>

#include "ratp-message.h"
#include "ratp-log.h"

/******************************************************************************/
/*
 * eCos, the Embedded Configurable Operating System.
 * Copyright (C) 1998, 1999, 2000, 2001, 2002 Red Hat, Inc.
 * Copyright (C) 2002 Gary Thomas
 */

/* Table of CRC constants - implements x^16+x^12+x^5+1 */
static const uint16_t crc16_tab[] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

static uint16_t
cyg_crc16 (const unsigned char *buf,
           int                  len)
{
    int i;
    uint16_t cksum;

    cksum = 0;
    for (i = 0;  i < len;  i++)
        cksum = crc16_tab[((cksum>>8) ^ *buf++) & 0xFF] ^ (cksum << 8);
    return cksum;
}

/******************************************************************************/

bool
ratp_message_has_data (const struct ratp_message_s *msg)
{
    if (msg->control & RATP_CONTROL_SO)
        return true;

    if (!(msg->control & (RATP_CONTROL_RST | RATP_CONTROL_SYN | RATP_CONTROL_FIN)) && msg->data_length)
        return true;

    return false;
}

size_t
ratp_message_get_size (const struct ratp_message_s *msg)
{
    size_t message_size;

    message_size = sizeof (struct ratp_message_s);
    if (!(msg->control & (RATP_CONTROL_SO | RATP_CONTROL_RST | RATP_CONTROL_SYN | RATP_CONTROL_FIN)) && msg->data_length)
        message_size += msg->data_length + 2;
    return message_size;
}

void
ratp_message_header_init (struct ratp_message_s *msg,
                          uint8_t                control,
                          uint8_t                data_length)
{
    msg->synch       = RATP_SYNCH_LEADER;
    msg->control     = control;
    msg->data_length = data_length;
    msg->cksum       = (control + data_length) ^ 0xff;
}

void
ratp_message_header_complete (struct ratp_message_s *msg)
{
    msg->cksum = (msg->control + msg->data_length) ^ 0xff;
}

bool
ratp_message_header_validate (const struct ratp_message_s *msg)
{
    uint8_t cksum;

    cksum  = msg->control;
    cksum += msg->data_length;
    cksum += msg->cksum;
    return (cksum == 0xff);
}

void
ratp_message_data_complete (struct ratp_message_s *msg)
{
    uint16_t crc_be;

    /* If not a message with data contents, do nothing */
    if (msg->control & (RATP_CONTROL_SO | RATP_CONTROL_RST | RATP_CONTROL_SYN | RATP_CONTROL_FIN))
        return;

    if (msg->data_length) {
        crc_be = htobe16 (cyg_crc16 (msg->data, msg->data_length));
        memcpy (&msg->data[msg->data_length], &crc_be, 2);
    }
}

bool
ratp_message_data_validate (const struct ratp_message_s *msg)
{
    uint16_t crc_expect, crc_read_be;

    /* If not a message with data contents, do nothing */
    if (msg->control & (RATP_CONTROL_SO | RATP_CONTROL_RST | RATP_CONTROL_SYN | RATP_CONTROL_FIN))
        return true;

    if (!msg->data_length)
        return true;

    memcpy (&crc_read_be, &msg->data[msg->data_length], 2);
    crc_expect = cyg_crc16 (msg->data, msg->data_length);
    return (be16toh (crc_read_be) == crc_expect);
}

void
ratp_message_debug (const struct ratp_message_s *msg,
                    const char                  *direction)
{
    char *str;

    if (ratp_log_get_level () != RATP_LOG_LEVEL_DEBUG)
        return;

    str = common_strhex (msg, ratp_message_get_size (msg), ":");
    ratp_debug ("%s: [%s %s %s %s %s %s %s %s] len: %-3d: %s",
                direction,
                msg->control & RATP_CONTROL_SO  ? "so"  : "--",
                msg->control & RATP_CONTROL_EOR ? "eor" : "---",
                msg->control & RATP_CONTROL_AN  ? "an"  : "--",
                msg->control & RATP_CONTROL_SN  ? "sn"  : "--",
                msg->control & RATP_CONTROL_RST ? "rst" : "---",
                msg->control & RATP_CONTROL_FIN ? "fin" : "---",
                msg->control & RATP_CONTROL_ACK ? "ack" : "---",
                msg->control & RATP_CONTROL_SYN ? "syn" : "---",
                msg->data_length,
                str);
    free (str);
}
