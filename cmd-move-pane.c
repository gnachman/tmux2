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
#include <unistd.h>

#include "tmux.h"

/*
 * Move a pane by splitting another pane and moving it into the new split.
 */

void    cmd_move_pane_key_binding(struct cmd *, int);
int cmd_move_pane_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_move_pane_entry = {
    "move-pane", "movew",
    "bs:dl:hp:Pt:v", 0, 0,
    "[-bdhvP] [-p percentage|-l size] [-t target-pane] [-s source-pane]",
    0,
    NULL,
    NULL,
    cmd_move_pane_exec
};

int
cmd_move_pane_exec(struct cmd *self, struct cmd_ctx *ctx)
{
    struct args     *args = self->args;
    struct session      *s;
    struct winlink      *wl;
    struct winlink      *src_wl;
    struct window       *w;
    struct window_pane  *src_wp, *wp, *new_wp = NULL;
    char            *cause, *new_cause;
    const char      *shell;
    u_int            hlimit, paneidx;
    int          size, percentage;
    enum layout_type     type;
    struct layout_cell  *lc;

    if ((wl = cmd_find_pane(ctx, args_get(args, 't'), &s, &wp)) == NULL)
        return (-1);
    w = wl->window;

    if ((src_wl = cmd_find_pane(ctx, args_get(args, 's'), &s, &src_wp)) == NULL)
        return (-1);
    if (args->argc > 0)
        return (-1);

    type = LAYOUT_TOPBOTTOM;
    if (args_has(args, 'h'))
        type = LAYOUT_LEFTRIGHT;

    size = -1;
    if (args_has(args, 'l')) {
        size = args_strtonum(args, 'l', 0, INT_MAX, &cause);
        if (cause != NULL) {
            xasprintf(&new_cause, "size %s", cause);
            xfree(cause);
            cause = new_cause;
            goto error;
        }
    } else if (args_has(args, 'p')) {
        percentage = args_strtonum(args, 'p', 0, INT_MAX, &cause);
        if (cause != NULL) {
            xasprintf(&new_cause, "percentage %s", cause);
            xfree(cause);
            cause = new_cause;
            goto error;
        }
        if (type == LAYOUT_TOPBOTTOM)
            size = (wp->sy * percentage) / 100;
        else
            size = (wp->sx * percentage) / 100;
    }
    hlimit = options_get_number(&s->options, "history-limit");

    shell = options_get_string(&s->options, "default-shell");
    if (*shell == '\0' || areshell(shell))
        shell = _PATH_BSHELL;

    if ((lc = layout_split_pane(wp, type, size, (int) args_has(args, 'b'))) == NULL) {
        cause = xstrdup("pane too small");
        goto error;
    }
    /* Move an existing window pane */
    struct window *src_w;

    /* Close the source pane */
    src_w = src_wl->window;
    TAILQ_REMOVE(&src_w->panes, src_wp, entry);
    if (src_wp == src_w->active) {
        src_w->active = w->last;
        src_w->last = NULL;
        if (src_w->active == NULL) {
            src_w->active = TAILQ_PREV(wp, window_panes, entry);
            if (src_w->active == NULL)
                src_w->active = TAILQ_NEXT(wp, entry);
        }
    } else if (src_wp == src_w->last)
        src_w->last = NULL;
    layout_close_pane(src_wp);

    /* If this was the last pane in the source window, close the window. */
    if (window_count_panes(src_w) == 0)
        server_kill_window(src_w);

    /* Add the source pane to its new window */
    if (TAILQ_EMPTY(&w->panes))
        TAILQ_INSERT_HEAD(&w->panes, src_wp, entry);
    else
        TAILQ_INSERT_AFTER(&w->panes, w->active, src_wp, entry);
    new_wp = src_wp;

    layout_assign_pane(lc, new_wp);

    server_redraw_window(w);

    if (!args_has(args, 'd')) {
        window_set_active_pane(w, new_wp);
        session_select(s, wl->idx);
        server_redraw_session(s);
    } else
        server_status_session(s);

    if (args_has(args, 'P')) {
        if (window_pane_index(new_wp, &paneidx) != 0)
            fatalx("index not found");
        ctx->print(ctx, "%s:%u.%u", s->name, wl->idx, paneidx);
    }
    control_notify_layout_change(w);
    return (0);

error:
    if (new_wp != NULL)
        window_remove_pane(w, new_wp);
    ctx->error(ctx, "create pane failed: %s", cause);
    xfree(cause);
    return (-1);
}
