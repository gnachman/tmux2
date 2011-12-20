/* $Id$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicm@users.sourceforge.net>
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
 * Set a control client attribute.
 */

int cmd_set_control_client_attr_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_set_control_client_attr_entry = {
    "set-control-client-attr", "setctlattr",
    "", 1, 2,
    "name value",
    0,
    NULL,
    NULL,
    cmd_set_control_client_attr_exec
};

/* "size" should be formatted as "int,int". If it is well formed, then *w and
 * *h will be populated with the first and second ints, respectively and 0 is
 * returned. If an error is encountered, -1 is returned. */
static int
parse_size(const char *size, u_int *w, u_int *h)
{
    char       *endptr, *temp;
    *w = strtoul(size, &endptr, 10);
    if (*endptr != ',')
        return (-1);
    temp = endptr + 1;
    *h = strtoul(temp, &endptr, 10);
    if (*endptr != '\0' || temp == endptr)
        return (-1);
    return (0);
}

/* Change the size of the client. If any change was made, outputs a list of
 * lines of window indexes and their layouts. */
static void
set_client_size(struct client *c, u_int w, u_int h, struct cmd_ctx *ctx)
{
    if (tty_set_size(&c->tty, w, h) > 0) {
        recalculate_sizes();
    }
    control_print_session_layouts(c->session, ctx);
}

int
cmd_set_control_client_attr_exec(struct cmd *self, struct cmd_ctx *ctx)
{
    struct args     *args = self->args;
    const char      *name, *value;
    struct client   *c;
    char            *temp;
    char            *eq;

    c = cmd_find_client(ctx, NULL);
    if (!c)
        return (-1);

    name = args->argv[0];
    if (*name == '\0') {
        ctx->error(ctx, "empty variable name");
        return (-1);
    }
    if (args->argc < 2)
        value = NULL;
    else
        value = args->argv[1];

    if (!strcmp(name, "client-size")) {
        if (!value) {
            ctx->error(ctx, "no value given");
            return (-1);
        }
        u_int w, h;
        if (parse_size(value, &w, &h))
            return (-1);

        set_client_size(c, w, h, ctx);
    } else if (!strcmp(name, "ready")) {
        c = cmd_find_client(ctx, NULL);
        if (c)
            c->flags |= CLIENT_CONTROL_READY;
    } else if (!strcmp(name, "set")) {
        if (!value) {
            ctx->error(ctx, "no value given");
            return (-1);
        }
        temp = xstrdup(value);
        eq = strchr(temp, '=');
        if (!eq) {
            ctx->error(ctx, "no '=' found");
            xfree(temp);
            return (-1);
        }
        *eq = 0;
        control_set_kvp(temp, eq + 1);
        xfree(temp);
    } else
        return (-1);

    return (0);
}
