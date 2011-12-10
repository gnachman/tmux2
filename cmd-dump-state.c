/* $Id$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicm@users.sourceforge.net>
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
 * Print out a table with terminal state for a window pane.
 */

int cmd_dump_state_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_dump_state_entry = {
    "dump-state", "dumpstate",
    "t:", 0, 0,
    "",
    0,
    NULL,
    NULL,
    cmd_dump_state_exec
};

static void
dump_state_uint(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
    ctx->print(ctx, "%s=%u", name, value);
}

static void
dump_state_int(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
    ctx->print(ctx, "%s=%d", name, value);
}

static void
dump_state_bits(struct cmd_ctx *ctx, bitstr_t *value, int length, const char *name)
{
    struct dstring ds;
    int            separator = 0;

    ds_init(&ds);
    for (int i = 0; i < length; i++) {
        if (bit_test(value, i)) {
            if (separator) {
                ds_append(&ds, ",");
            } else {
                separator = 1;
            }
            ds_appendf(&ds, "%d", i);
        }
    }
    ctx->print(ctx, "%s=%s", name, ds.buffer);
    ds_free(&ds);
}

static void
dump_state_string(struct cmd_ctx *ctx, char *str, const char *name)
{
    struct dstring ds;
    ds_init(&ds);

    ctx->print(ctx, "%s=%s", name, str);
    ds_free(&ds);
}

int
cmd_dump_state_exec(struct cmd *self, struct cmd_ctx *ctx)
{
    struct args         *args = self->args;
    struct window_pane  *wp;
    struct session      *s;

    if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
        return (-1);

    dump_state_int(ctx, wp->saved_grid ? 1 : 0, "in_alternate_screen");
    /* This is the saved cursor position from when the alternate screen was
     * entered. */
    dump_state_uint(ctx, wp->saved_cx, "base_cursor_x");
    dump_state_uint(ctx, wp->saved_cy, "base_cursor_y");
    dump_state_uint(ctx, wp->base.cx, "cursor_x");
    dump_state_uint(ctx, wp->base.cy, "cursor_y");
    dump_state_uint(ctx, wp->base.rupper, "scroll_region_upper");
    dump_state_uint(ctx, wp->base.rlower, "scroll_region_lower");
    dump_state_bits(ctx, wp->base.tabs, wp->base.grid->sx, "tabstops");
    dump_state_int(ctx, wp->base.sel.flag, "has_selection");
    dump_state_int(ctx, wp->base.sel.rectflag, "has_rectangular_selection");
    dump_state_uint(ctx, wp->base.sel.sx, "selection_start_x");
    dump_state_uint(ctx, wp->base.sel.sy, "selection_start_y");
    dump_state_uint(ctx, wp->base.sel.ex, "selection_end_x");
    dump_state_uint(ctx, wp->base.sel.ey, "selection_end_y");
    dump_state_string(ctx, wp->base.title, "title");

    /* This is the saved cursor position from CSI DECSC. */
    dump_state_int(ctx, wp->ictx.old_cx, "decsc_cursor_x");
    dump_state_int(ctx, wp->ictx.old_cy, "decsc_cursor_y");

    return (0);
}
