/* $Id$ */

/*
 * Copyright (c) 2011 George Nachman <tmux@georgester.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Helper command for control mode. The possible subcommands are:
 *
 * set-client-size client-size: Set client size, value is like "80,25".
 */

enum cmd_retval cmd_control_helper_exec(struct cmd *, struct cmd_q *);

const struct cmd_entry cmd_control_helper_entry = {
       "control-helper", "control",
       "", 2, 2,
       "set-client-size width,height",
       0,
       NULL,
       NULL,
       cmd_control_helper_exec
};

#define CMD_CONTROL_HELPER_MAX_WIDTH 20000
#define CMD_CONTROL_HELPER_MAX_HEIGHT 20000

enum cmd_retval        cmd_control_helper_set_client_size(struct cmd_q *,
                   const char *);

enum cmd_retval
cmd_control_helper_set_client_size(struct cmd_q *ctx, const char *value)
{
       struct client   *c;
       u_int            w, h;

       if (value == NULL) {
               cmdq_error(ctx, "no value given");
               return (CMD_RETURN_ERROR);
       }

       c = cmd_find_client(ctx, NULL, 1);
       if (c == NULL)
               return (CMD_RETURN_ERROR);

       if (sscanf(value, "%u,%u", &w, &h) != 2) {
               cmdq_error(ctx, "bad size argument");
               return (CMD_RETURN_ERROR);
       }

       if (w > CMD_CONTROL_HELPER_MAX_WIDTH ||
           h > CMD_CONTROL_HELPER_MAX_HEIGHT) {
               cmdq_error(ctx, "client too big");
               return (CMD_RETURN_ERROR);
       }
       if (tty_set_size(&c->tty, w, h) > 0)
               recalculate_sizes();

       return (CMD_RETURN_NORMAL);
}

int
cmd_control_helper_exec(struct cmd *self, struct cmd_q *ctx)
{
       struct args     *args = self->args;
       const char      *value;

       if (strcmp(args->argv[0], "set-client-size") == 0) {
               if (args->argc != 2) {
                       cmdq_error(ctx, "not enough arguments");
                       return (CMD_RETURN_ERROR);
               }
               value = args->argv[1];
               return (cmd_control_helper_set_client_size(ctx, value));
       }

       return (CMD_RETURN_ERROR);
}
