/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * ratp-cli - Minimal libratp tester
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA.
 *
 * Copyright (C) 2017 Zodiac Inflight Innovations
 * Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>

#include <libratp.h>
#include <common.h>

#define PROGRAM_NAME    "ratp-cli"
#define PROGRAM_VERSION PACKAGE_VERSION

/******************************************************************************/

static unsigned long main_tid;

static void
log_handler (ratp_log_level_t  level,
             unsigned long     tid,
             const char       *message)
{
    if (tid == main_tid)
        printf ("[ratp] %s: %s\n", ratp_log_level_str (level), message);
    else
        printf ("[ratp %lu] %s: %s\n", tid, ratp_log_level_str (level), message);
}

/******************************************************************************/

static bool volatile quit_requested;
static bool volatile close_requested;

static void
sig_handler (int signo)
{
    if (!close_requested) {
        close_requested = true;
        return;
    }
    quit_requested = true;
}

/******************************************************************************/

struct baudrate_num_s {
    speed_t       baudrate;
    unsigned long num;
};

static const struct baudrate_num_s baudrate_num[] = {
    { B9600,     9600 },
    { B19200,   19200 },
    { B38400,   38400 },
    { B57600,   57600 },
    { B115200, 115200 },
    { B230400, 230400 },
    { B460800, 460800 },
};

static speed_t
baudrate_from_num (unsigned long num)
{
    int i;

    for (i = 0; i < (sizeof (baudrate_num) / sizeof (baudrate_num[0])); i++) {
        if (baudrate_num[i].num == num)
            return baudrate_num[i].baudrate;
    }

    return B0;
}

/******************************************************************************/

static void
state_update_ready (ratp_link_t       *self,
                    ratp_link_state_t  old_state,
                    ratp_link_state_t  new_state,
                    ratp_status_t      status_reason,
                    void              *user_data)
{
    if (new_state == RATP_LINK_STATE_CLOSED) {
        printf ("link closed\n");
        quit_requested = true;
    }
}

static void
message_send_ready (ratp_link_t   *self,
                    ratp_status_t  status_reason,
                    void          *unused)
{
    if (status_reason == RATP_STATUS_OK)
        printf ("message successfully sent\n");
    else
        fprintf (stderr, "error: couldn't send message: %s\n", ratp_status_str (status_reason));
}

static void
message_receive_ready (ratp_link_t   *self,
                       const uint8_t *buffer,
                       size_t         buffer_size,
                       void          *user_data)
{
    printf ("message received: %s\n", (const char *) buffer);
}

static int
run_active (ratp_link_t *ratp)
{
    ratp_status_t      st;
    static const char *message = "hello world active->passive!";

    if ((st = ratp_link_active_open (ratp)) != RATP_STATUS_OK) {
        fprintf (stderr, "error: couldn't actively open link: %s\n", ratp_status_str (st));
        return -1;
    }

    printf ("active open requested\n");

    while (!quit_requested) {
        if ((st = ratp_link_send (ratp,
                                  10000,
                                  (const uint8_t *) message,
                                  strlen (message) + 1,
                                  (ratp_link_send_ready_func) message_send_ready,
                                  NULL)) != RATP_STATUS_OK) {
            fprintf (stderr, "error: couldn't schedule message to send: %s\n", ratp_status_str (st));
            return -1;
        }
        if (close_requested) {
            printf ("requesting close...\n");
            ratp_link_close (ratp);
        }
        sleep (1);
    }
    return 0;
}

static int
run_passive (ratp_link_t *ratp)
{
    ratp_status_t      st;
    static const char *message = "hello world passive->active!";

    if ((st = ratp_link_passive_open (ratp)) != RATP_STATUS_OK) {
        fprintf (stderr, "error: couldn't passively open link: %s\n", ratp_status_str (st));
        return -1;
    }

    printf ("passive open requested\n");

    while (!quit_requested) {
        if (close_requested) {
            printf ("requesting close...\n");
            ratp_link_close (ratp);
            sleep (5);
            continue;
        }

        if ((st = ratp_link_send (ratp,
                                  10000,
                                  (const uint8_t *) message,
                                  strlen (message) + 1,
                                  (ratp_link_send_ready_func) message_send_ready,
                                  NULL)) != RATP_STATUS_OK) {
            fprintf (stderr, "error: couldn't schedule message to send: %s\n", ratp_status_str (st));
            return -1;
        }
        sleep (1);
    }
    return 0;
}

/******************************************************************************/

static void
print_help (void)
{
    printf ("\n"
            "Usage: " PROGRAM_NAME " <option>\n"
            "\n"
            "TTY link selection:\n"
            "  -t, --tty=[PATH]                TTY device file path\n"
            "  -b, --tty-baudrate=[BAUDRATE]   Serial port baudrate\n"
            "\n"
            "FIFO link selection:\n"
            "  -i, --fifo-in=[PATH]            FIFO input path.\n"
            "  -o, --fifo-out=[PATH]           FIFO output path.\n"
            "\n"
            "Link settings:\n"
            "  -m, --mdl=[MDL]                 Set the maximum data length.\n"
            "\n"
            "Actions:\n"
            "  -a, --active                    Active RATP endpoint.\n"
            "  -p, --passive                   Passive RATP endpoint.\n"
            "\n"
            "Common options:\n"
            "  -d, --debug                     Enable verbose logging.\n"
            "  -h, --help                      Show help.\n"
            "  -v, --version                   Show version.\n"
            "\n"
            "Notes:\n"
            " * [BAUDRATE] may be any of:\n"
            "     9600, 19200, 38400, 57600, 115200 (default), 230400, 460800.\n"
            " * [MDL] may be any value between 0 and 255 (default).\n"
            "\n");
}

static void
print_version (void)
{
    printf ("\n"
            PROGRAM_NAME " " PROGRAM_VERSION "\n"
            "Copyright (2017) Zodiac Inflight Innovations\n"
            "Copyright (2017) Aleksander Morgado\n"
            "\n");
}

int main (int argc, char **argv)
{
    int            idx, iarg = 0;
    char          *tty_path = NULL;
    speed_t        tty_baudrate = B0;
    char          *fifo_in_path = NULL;
    char          *fifo_out_path = NULL;
    long           mdl = -1;
    bool           action_active = false;
    bool           action_passive = false;
    bool           debug = false;
    unsigned int   n_actions;
    int            action_ret;
    ratp_link_t   *ratp;
    ratp_status_t  st;

    const struct option longopts[] = {
        { "fifo-in",      required_argument, 0, 'i' },
        { "fifo-out",     required_argument, 0, 'o' },
        { "tty",          required_argument, 0, 't' },
        { "tty-baudrate", required_argument, 0, 'b' },
        { "mdl",          required_argument, 0, 'm' },
        { "active",       no_argument,       0, 'a' },
        { "passive",      no_argument,       0, 'p' },
        { "debug",        no_argument,       0, 'd' },
        { "version",      no_argument,       0, 'v' },
        { "help",         no_argument,       0, 'h' },
        { 0,              0,                 0, 0   },
    };

    /* turn off getopt error message */
    opterr = 1;
    while (iarg != -1) {
        iarg = getopt_long (argc, argv, "i:o:t:b:m:apdvh", longopts, &idx);
        switch (iarg) {
        case 'i':
            if (fifo_in_path)
                fprintf (stderr, "warning: -i,--fifo-in given multiple times\n");
            else
                fifo_in_path = strdup (optarg);
            break;
        case 'o':
            if (fifo_out_path)
                fprintf (stderr, "warning: -o,--fifo-out given multiple times\n");
            else
                fifo_out_path = strdup (optarg);
            break;
        case 't':
            if (tty_path)
                fprintf (stderr, "warning: -t,--tty given multiple times\n");
            else
                tty_path = strdup (optarg);
            break;
        case 'b':
            if (tty_baudrate != B0)
                fprintf (stderr, "warning: -b,--tty-baudrate given multiple times\n");
            else {
                unsigned int aux;

                aux = strtoul (optarg, NULL, 10);
                tty_baudrate = baudrate_from_num (aux);
                if (tty_baudrate == B0) {
                    fprintf (stderr, "error: invalid [BAUDRATE] given: %s\n", optarg);
                    return -1;
                }
            }
            break;
        case 'm':
            if (mdl >= 0)
                fprintf (stderr, "warning: -m,--mdl given multiple times\n");
            else {
                errno = 0;
                mdl = strtol (optarg, NULL, 10);
                if ((mdl == 0 && errno) || mdl > 255) {
                    fprintf (stderr, "error: invalid [MDL] given: %s\n", optarg);
                    return -1;
                }
            }
            break;
        case 'a':
            action_active = true;
            break;
        case 'p':
            action_passive = true;
            break;
        case 'd':
            debug = true;
            break;
        case 'h':
            print_help ();
            return 0;
        case 'v':
            print_version ();
            return 0;
        }
    }

    /* Validate actions */
    n_actions = (action_active + action_passive);
    if (n_actions > 1) {
        fprintf (stderr, "error: too many actions requested\n");
        return -1;
    }
    if (n_actions == 0) {
        fprintf (stderr, "error: no actions requested\n");
        return -1;
    }

    /* Initialize RATP library */
    if ((st = ratp_init ()) != RATP_STATUS_OK) {
        fprintf (stderr, "error: couldn't initialize RATP library: %s\n", ratp_status_str (st));
        return -1;
    }

    /* Verbose library logging */
    if (debug) {
        main_tid = (unsigned long) pthread_self ();
        ratp_log_set_level (RATP_LOG_LEVEL_DEBUG);
        ratp_log_set_handler (log_handler);
    }

    /* Setup signals */
    signal (SIGHUP, sig_handler);
    signal (SIGINT, sig_handler);

    /* Validate input paths and create RATP link */
    if (fifo_in_path || fifo_out_path) {
        if (!fifo_in_path || !fifo_out_path) {
            fprintf (stderr, "error: FIFO based RATP link requires both input and output paths\n");
            return -2;
        }
        ratp = ratp_link_new_fifo (fifo_in_path, fifo_out_path, mdl >= 0 ? mdl : 255);
        if (!ratp) {
            fprintf (stderr, "error: couldn't create FIFO based RATP link\n");
            return -2;
        }
    } else if (tty_path) {
        ratp = ratp_link_new_tty (tty_path, tty_baudrate != B0 ? tty_baudrate : B115200, mdl >= 0 ? mdl : 255);
        if (!ratp) {
            fprintf (stderr, "error: couldn't create TTY based RATP link\n");
            return -2;
        }
    } else {
        fprintf (stderr, "error: no link selected\n");
        return -2;
    }

    /* Initialize RATP link */
    if ((st = ratp_link_initialize (ratp)) != RATP_STATUS_OK) {
        fprintf (stderr, "error: couldn't initialize RATP link: %s\n", ratp_status_str (st));
        return -3;
    }

    /* Configure callbacks */
    ratp_link_set_receive_callback (ratp, message_receive_ready, NULL);
    ratp_link_set_state_update_callback (ratp, state_update_ready, NULL);

    if (action_active)
        action_ret = run_active (ratp);
    else if (action_passive)
        action_ret = run_passive (ratp);
    else
        assert (0);

    ratp_link_shutdown (ratp);
    ratp_link_free (ratp);
    return action_ret;
}
