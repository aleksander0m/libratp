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

#ifndef RATP_MESSAGE_H
#define RATP_MESSAGE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define RATP_MAX_MSG_SIZE 261

#define RATP_SYNCH_LEADER 0x01

enum {
    RATP_CONTROL_SO  = 1 << 0,
    RATP_CONTROL_EOR = 1 << 1,
    RATP_CONTROL_AN  = 1 << 2,
    RATP_CONTROL_SN  = 1 << 3,
    RATP_CONTROL_RST = 1 << 4,
    RATP_CONTROL_FIN = 1 << 5,
    RATP_CONTROL_ACK = 1 << 6,
    RATP_CONTROL_SYN = 1 << 7,
};

struct ratp_message_s {
    uint8_t  synch;
    uint8_t  control;
    uint8_t  data_length;
    uint8_t  cksum;
    uint8_t  data[];
} __attribute__((packed));

#define ratp_message_get_sn(msg)                  (((msg)->control & RATP_CONTROL_SN) ? 1 : 0)
#define ratp_message_validate_sn(msg,sn_received) (ratp_message_get_sn (msg) == ((sn_received + 1) % 2))
#define ratp_message_get_an(msg)                  (((msg)->control & RATP_CONTROL_AN) ? 1 : 0)
#define ratp_message_validate_an(msg,sn_sent)     (ratp_message_get_an (msg) == ((sn_sent + 1) % 2))
#define ratp_message_set_sn(msg,sn)               (msg)->control |= (sn ? RATP_CONTROL_SN : 0)
#define ratp_message_set_an(msg,an)               (msg)->control |= (an ? RATP_CONTROL_AN : 0)
#define ratp_message_set_next_sn(msg,sn)          (msg)->control |= (((sn + 1) % 2) ? RATP_CONTROL_SN : 0)
#define ratp_message_set_next_an(msg,an)          (msg)->control |= (((an + 1) % 2) ? RATP_CONTROL_AN : 0)

size_t ratp_message_get_size        (const struct ratp_message_s *msg);
void   ratp_message_header_init     (struct ratp_message_s       *msg,
                                     uint8_t                      control,
                                     uint8_t                      data_length);
void   ratp_message_header_complete (struct ratp_message_s       *msg);
bool   ratp_message_header_validate (const struct ratp_message_s *msg);
bool   ratp_message_has_data        (const struct ratp_message_s *msg);
void   ratp_message_data_complete   (struct ratp_message_s       *msg);
bool   ratp_message_data_validate   (const struct ratp_message_s *msg);
void   ratp_message_debug           (const struct ratp_message_s *msg,
                                     const char                  *direction);

#endif /* RATP_MESSAGE_H */
