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

typedef void control_write_cb(struct client *c, void *user_data);
static int windows_changed;
static struct window **layouts_changed;
static int num_layouts_changed;
static int spontaneous_message_allowed;

static void control_notify_windows_changed(void);

struct control_input_ctx {
    struct window_pane *wp;
    const u_char *buf;
    size_t len;
};

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
void control_write_b64(struct client *c, const char *buf, int len);

void printflike2
control_msg_error(struct cmd_ctx *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    evbuffer_add_vprintf(ctx->curclient->stdout_event->output, fmt, ap);
    va_end(ap);

    bufferevent_write(ctx->curclient->stdout_event, "\n", 1);
}

void printflike2
control_msg_print(struct cmd_ctx *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    evbuffer_add_vprintf(ctx->curclient->stdout_event->output, fmt, ap);
    va_end(ap);

    bufferevent_write(ctx->curclient->stdout_event, "\n", 1);
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
    ctx.cmdclient = NULL;
    ctx.curclient = c;

    ctx.error = control_msg_error;
    ctx.print = control_msg_print;
    ctx.info = control_msg_info;

    if (cmd_string_parse(line, &cmdlist, &cause) != 0) {
        /* Error */
        if (cause) {
            /* cause should always be set if there's an error. */
            evbuffer_add_printf(out->output, "%%error %s", cause);
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

void
control_error_callback(
    unused struct bufferevent *bufev, unused short what, unused void *data)
{
    struct client   *c = data;

    c->references--;
    if (c->flags & CLIENT_DEAD)
        return;

    bufferevent_disable(c->stdin_event, EV_READ|EV_WRITE);
    setblocking(c->stdin_fd, 1);
    close(c->stdin_fd);
    c->stdin_fd = -1;

    if (c->stdin_callback != NULL)
        c->stdin_callback(c, c->stdin_data);
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
}

void
control_write_str(struct client *c, const char *str)
{
    control_write(c, str, strlen(str));
}

void
control_write_b64(struct client *c, const char *buf, int len)
{
    // TODO: I don't have an internet connection atm so I'll just do something
    // hacky instead of base 64
    for (int i = 0; i < len; i++) {
        char temp[3];
        snprintf(temp, sizeof(temp), "%02x", (int)buf[i]);
        control_write(c, temp, 2);
    }
}

void
control_write(struct client *c, const char *buf, int len)
{
    if (c->session) {
        // Only write to control clients that have an attached session. This
        // indicates that the initial setup performed by the local client is
        // complete and the remote client is expecting to send and receive
        // commands.
        evbuffer_add(c->stdout_event->output, buf, len);
    }
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
control_write_window(struct client *c, struct window *w)
{
    u_int i = -1;
    window_index(w, &i);
    control_write_printf(c, "%u", i);
}

void
control_write_window_pane(struct client *c, struct window_pane *wp)
{
    u_int i = -1;
    window_index(wp->window, &i);
    control_write_printf(c, "%u", i);
    control_write_str(c, ".");
    u_int j = -1;
    window_pane_index(wp, &j);
    control_write_printf(c, "%u", j);
}

void
control_write_input(struct client *c, struct window_pane *wp,
                    const u_char *buf, int len)
{
    control_write_str(c, "%output ");
    control_write_window_pane(c, wp);
    control_write_str(c, " ");
    control_write_b64(c, buf, len);
    control_write_str(c, "\n");
}

static void
control_foreach_client(control_write_cb *cb, void *user_data)
{
    for (int i = 0; i < (int) ARRAY_LENGTH(&clients); i++) {
        struct client *c = ARRAY_ITEM(&clients, i);
        if (c && c->flags & CLIENT_CONTROL) {
            if (c->flags & CLIENT_SUSPENDED) {
                continue;
            }
            cb(c, user_data);
        }
    }
}

static void
control_write_input_cb(struct client *c, void *user_data)
{
    struct control_input_ctx *ctx = user_data;
    if (c->flags & CLIENT_CONTROL_READY) {
        /* The client has sent start-control and may receive output. */
        if (!(c->flags & CLIENT_CONTROL_UPTODATE)) {
            /* This is the first output the client has received. */
            c->flags |= CLIENT_CONTROL_UPTODATE;
            if (ctx->wp->ictx.input_since_ground.used) {
                /* There's a partial escape code in waiting. */
                control_write_input(c, ctx->wp, ctx->wp->ictx.input_since_ground.buffer,
                                    ctx->wp->ictx.input_since_ground.used);
            }
        }
        control_write_input(c, ctx->wp, ctx->buf, ctx->len);
    }
}

void
control_broadcast_input(struct window_pane *wp, const u_char *buf, size_t len)
{
    struct control_input_ctx ctx;
    ctx.wp = wp;
    ctx.buf = buf;
    ctx.len = len;
    control_foreach_client(control_write_input_cb, &ctx);
}

static void
control_write_layout_change_cb(struct client *c, unused void *user_data)
{
    struct format_tree  *ft;
    struct winlink      *wl;

    if (!(c->flags & CLIENT_CONTROL_READY)) {
        /* Don't issue spontaneous commands until the remote client has
         * finished its initalization. It's ok because the remote client should
         * fetch all window and layout info at the same time as it's marked
         * ready. */
        return;
    }

    for (int i = 0; i < num_layouts_changed; i++) {
        struct window *w = layouts_changed[i];
        if (w) {
            const char *template = "%layout-change #{window_index} "
                "#{window_layout_ex}\n";
            ft = format_create();
            wl = winlink_find_by_window(&c->session->windows, w);
            format_winlink(ft, c->session, wl);
            control_write_str(c, format_expand(ft, template));
        }
    }
}

void
control_notify_layout_change(struct window *w)
{
    for (int i = 0; i < num_layouts_changed; i++) {
        if (layouts_changed[i] == w) {
            // Don't add a duplicate
            return;
        }
    }

    ++num_layouts_changed;
    if (!layouts_changed) {
        layouts_changed = xmalloc(sizeof(struct window *));
    } else {
        layouts_changed = xrealloc(layouts_changed, num_layouts_changed,
                                   sizeof(struct window *));
    }
    layouts_changed[num_layouts_changed - 1] = w;

    if (spontaneous_message_allowed) {
        control_broadcast_queue();
    }
}

void
control_notify_window_removed(struct window *w)
{
    for (int i = 0; i < num_layouts_changed; i++) {
        if (layouts_changed[i] == w) {
            layouts_changed[i] = NULL;
            break;
        }
    }
    control_notify_windows_changed();
}

void
control_notify_window_added(void)
{
    control_notify_windows_changed();
}

static void
control_notify_windows_changed(void)
{
    windows_changed = 1;
    if (spontaneous_message_allowed) {
        control_broadcast_queue();
    }
}

static void
control_write_windows_change_cb(struct client *c, unused void *user_data)
{
    if (!(c->flags & CLIENT_CONTROL_READY)) {
        /* Don't issue spontaneous commands until the remote client has
         * finished its initalization. It's ok because the remote client should
         * fetch all window and layout info at the same time as it's marked
         * ready. */
        return;
    }

    control_write_str(c, "%windows-change\n");
}

void control_broadcast_queue(void)
{
    if (num_layouts_changed) {
        control_foreach_client(control_write_layout_change_cb, NULL);
        num_layouts_changed = 0;
        xfree(layouts_changed);
        layouts_changed = NULL;
    }
    if (windows_changed) {
        control_foreach_client(control_write_windows_change_cb, NULL);
        windows_changed = 0;
    }
}

void control_set_spontaneous_messages_allowed(int allowed)
{
    if (allowed && !spontaneous_message_allowed) {
        control_broadcast_queue();
    }
    spontaneous_message_allowed = allowed;
}

void control_handshake(struct client *c)
{
    control_write_str(c, "\033_tmux1\033\\%noop If you can see this message, "
                      "your terminal emulator does not support tmux mode. "
                      "Type \"detach\" and press the enter key to return to "
                      "your shell.\n");
}

/* Print one line for each window in the session with the window number and the
 * layout. */
void control_print_session_layouts(struct session *session, struct cmd_ctx *ctx)
{
    struct format_tree  *ft;
    struct winlink      *wl;
    struct winlinks     *wwl;

    wwl = &session->windows;
    RB_FOREACH(wl, winlinks, wwl) {
        const char *template = "#{window_index} #{window_layout_ex}";
        ft = format_create();
        format_winlink(ft, session, wl);
        ctx->print(ctx, "%s", format_expand(ft, template));
    }
}
