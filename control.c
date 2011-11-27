/* $Id$ */

/*
 * Copyright (c) 2010 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <event.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"


/*
 * XXX TODO
 * TODO(georgen): What other uses might there be? Any immediate concerns?
 * Prevent other uses of stdin/stdout fds.
 */

void   control_read_callback(struct bufferevent *, void *);
void   control_error_callback(struct bufferevent *, short, void *);
void printflike2 control_msg_error(struct cmd_ctx *ctx, const char *fmt, ...);
void printflike2 control_msg_print(struct cmd_ctx *ctx, const char *fmt, ...);
void printflike2 control_msg_info(unused struct cmd_ctx *ctx,
    unused const char *fmt, ...);
void   control_read_callback(unused struct bufferevent *bufev, void *data);
void   control_error_callback(unused struct bufferevent *bufev,
    unused short what, void *data);

void printflike2
control_msg_error(struct cmd_ctx *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    evbuffer_add_vprintf(ctx->cmdclient->stdout_event->output, fmt, ap);
    va_end(ap);

    bufferevent_write(ctx->cmdclient->stdout_event, "\n", 1);
}

void printflike2
control_msg_print(struct cmd_ctx *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    evbuffer_add_vprintf(ctx->cmdclient->stdout_event->output, fmt, ap);
    va_end(ap);

    bufferevent_write(ctx->cmdclient->stdout_event, "\n", 1);
}

void printflike2
control_msg_info(unused struct cmd_ctx *ctx, unused const char *fmt, ...)
{
}

/* Control input callback. */
void
control_read_callback(unused struct bufferevent *bufev, void *data)
{
    struct client      *c = data;
    struct bufferevent *out = c->stdout_event;
    char               *line;
    struct cmd_ctx      ctx;
    struct cmd_list    *cmdlist;
    char               *cause;

    /* Read input line. */
    line = evbuffer_readln(c->stdin_event->input, NULL, EVBUFFER_EOL_CRLF);
    if (line == NULL)
        return;

    /* Parse command. */
    ctx.msgdata = NULL;
    ctx.curclient = NULL;

    ctx.error = control_msg_error;
    ctx.print = control_msg_print;
    ctx.info = control_msg_info;

    ctx.cmdclient = c;

    if (cmd_string_parse(line, &cmdlist, &cause) != 0) {
        /* Error */
        if (cause) {
            /* cause should always be set if there's an error. */
            evbuffer_add_printf(out->output, "%s", cause);
            bufferevent_write(out, "\n", 1);
            xfree(cause);
        }
    } else {
        /* Parsed ok. Run command. */
        cmd_list_exec(cmdlist, &ctx);
        cmd_list_free(cmdlist);
    }

    xfree(line);
}

/* Control error callback. */
/* ARGSUSED */
void
control_error_callback(
    unused struct bufferevent *bufev, unused short what, void *data)
{
    struct client   *c = data;
    //TODO(georgen): implement this
}

/* Initialise as a control client. */
void
control_start(struct client *c)
{
    /* Enable reading from stdin. */
    if (c->stdin_event != NULL)
        bufferevent_free(c->stdin_event);
    c->stdin_event = bufferevent_new(c->stdin_fd,
                                     control_read_callback,
                                     NULL,
                                     control_error_callback, c);
    if (c->stdin_event == NULL)
        fatalx("failed to create stdin event");
    bufferevent_enable(c->stdin_event, EV_READ);

    /* Write the protocol identifier and version. */
    bufferevent_enable(c->stdout_event, EV_WRITE);
    control_write_str(c, "\033_tmux1\033\\%%noop tmux ready\n");
}

void
control_write_str(struct client *c, const char *str)
{
    control_write(c, str, strlen(str));
}

void
control_write(struct client *c, const char *buf, int len)
{
    evbuffer_add(c->stdout_event->output, buf, len);
}

void
control_write_printf(struct client *c, const char *format, ...)
{
    va_list argp;
    va_start(argp, format);

    evbuffer_add_vprintf(c->stdout_event->output, format, argp);

    va_end(argp);
}

void
control_write_window_pane(struct client *c, struct window_pane *wp)
{
    u_int i = -1;
    window_index(wp->window, &i);
    control_write_printf(c, "%u", i);
    control_write_str(c, ".");
    control_write_printf(c, "%u", wp->id);
}

void
control_write_input(struct client *c, struct window_pane *wp,
                    const u_char *buf, int len)
{
    control_write_str(c, "%output ");
    control_write_window_pane(c, wp);
    control_write_printf(c, " %d ", len);
    control_write(c, buf, len);
    control_write_str(c, "\n");
}

void
control_broadcast_input(struct window_pane *wp, const u_char *buf, size_t len)
{
    for (int i = 0; i < (int) ARRAY_LENGTH(&clients); i++) {
        struct client *c = ARRAY_ITEM(&clients, i);
        if (c && c->flags & CLIENT_CONTROL) {
            if (c->flags & CLIENT_SUSPENDED) {
                continue;
            }
            control_write_input(c, wp, buf, len);
        }
    }
}
