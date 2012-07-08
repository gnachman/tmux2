/* $Id$ */

/*
 * Copyright (c) 2012 Nicholas Marriott <nicm@users.sourceforge.net>
 * Copyright (c) 2012 George Nachman <tmux@georgester.com>
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
 * Version number history:
 * There may be some binaries in the world with 0.1, 0.2, 0.3, and
 * 0.4. These were pre-release test versions.
 * 1.0: First complete integration.
 */
#define CURRENT_TMUX_CONTROL_PROTOCOL_VERSION "1.0"

typedef void control_write_cb(struct client *c, void *user_data);

/* A pending change related to a window's state. */
struct window_change {
	u_int window_id;
	enum {
		WINDOW_CREATED, WINDOW_RENAMED, WINDOW_CLOSED
	} action;
	TAILQ_ENTRY(window_change) entry;
};
TAILQ_HEAD(, window_change) window_changes;

/* Global key-value pairs. */
struct options	 control_options;

/* Stored text to send control clients. */
struct control_input_ctx {
	struct window_pane *wp;
	const u_char *buf;
	size_t len;
};


static void	control_notify_windows_changed(void);

static struct window	**layouts_changed;
static int	    	  num_layouts_changed;
static int	    	  spontaneous_message_allowed;
/* Flag values for session_changed_flags. */
#define SESSION_CHANGE_ADDREMOVE	0x1
#define SESSION_CHANGE_ATTACHMENT	0x2
#define SESSION_CHANGE_RENAME		0x4
/* A bitmask storing which kinds of session changes clients need to be notified
 * of. */
static int	    	  session_changed_flags;

void	control_error_callback(struct bufferevent *, short, void *);
void printflike2 control_msg_error(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_print(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_info(struct cmd_ctx *, const char *, ...);
void printflike2 control_write(struct client *, const char *, ...);
void	control_callback(struct client *c, int closed, unused void *data);
void	control_error_callback(unused struct bufferevent *bufev,
		unused short what, void *data);
void	control_hex_encode_buffer(const char *buf, int len,
		struct evbuffer *output);

/* Command error callback. */
void printflike2
control_msg_error(struct cmd_ctx *ctx, const char *fmt, ...)
{
	struct client	*c = ctx->curclient;
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/* Command print callback. */
void printflike2
control_msg_print(struct cmd_ctx *ctx, const char *fmt, ...)
{
	struct client	*c = ctx->curclient;
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/* Command info callback. */
void printflike2
control_msg_info(unused struct cmd_ctx *ctx, unused const char *fmt, ...)
{
}

/* Control input callback. */

void
control_error_callback(
    unused struct bufferevent *bufev, unused short what, unused void *data)
{
	struct client	*c = data;
	server_client_lost(c);
}

void
control_hex_encode_buffer(const char *buf, int len, struct evbuffer *output)
{
	for (int i = 0; i < len; i++) {
	    	evbuffer_add_printf(output, "%02x", ((int) buf[i]) & 0xff);
	}
}

void
control_force_write_str(struct client *c, const char *str)
{
	evbuffer_add(c->stdout_data, str, strlen(str));
}

/* Write a line. */
void printflike2
control_write(struct client *c, const char *fmt, ...)
{
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

void
control_write_input(struct client *c, struct window_pane *wp,
			const u_char *buf, int len)
{
    	struct evbuffer	*hex_output;

	if (!c->session)
	    return;
	/* Only write input if the window pane is linked to a window belonging
	 * to the client's session. */
	if (winlink_find_by_window(&c->session->windows, wp->window)) {
	    	hex_output = evbuffer_new();
	    	control_hex_encode_buffer(buf, len, hex_output);
		evbuffer_add(hex_output, "\0", 1);  /* Null terminate */
		control_write(c, "%%output %%%u %s", wp->id,
			      EVBUFFER_DATA(hex_output));
		evbuffer_free(hex_output);
	}
}

static void
control_foreach_client(control_write_cb *cb, void *user_data)
{
	for (int i = 0; i < (int) ARRAY_LENGTH(&clients); i++) {
		struct client *c = ARRAY_ITEM(&clients, i);
		if (c &&
                    (c->flags & CLIENT_CONTROL) &&
		    !(c->flags & CLIENT_SUSPENDED))
			cb(c, user_data);
	}
}

static void
control_write_input_cb(struct client *c, void *user_data)
{
	struct control_input_ctx	*ctx = user_data;
	if (c->flags & CLIENT_CONTROL_READY)
	    control_write_input(c, ctx->wp, ctx->buf, ctx->len);
}

void
control_broadcast_input(struct window_pane *wp, const u_char *buf, size_t len)
{
	struct control_input_ctx	ctx;
	ctx.wp = wp;
	ctx.buf = buf;
	ctx.len = len;
	control_foreach_client(control_write_input_cb, &ctx);
}

static void
control_write_attached_session_change_cb(
    struct client *c, unused void *user_data)
{
	if (c->session && (c->flags & CLIENT_SESSION_CHANGED)) {
		control_write(c, "%%session-changed %d %s",
			      c->session->idx, c->session->name);
		c->flags &= ~CLIENT_SESSION_CHANGED;
	}
	if (session_changed_flags &
	    (SESSION_CHANGE_ADDREMOVE | SESSION_CHANGE_RENAME)) {
		control_write(c, "%%sessions-changed");
	}
	if ((session_changed_flags & SESSION_CHANGE_RENAME) &&
	    c->session &&
	    (c->session->flags & SESSION_RENAMED)) {
		control_write(c, "%%session-renamed %s", c->session->name);
	}
}

static void
control_write_layout_change_cb(struct client *c, unused void *user_data)
{
	struct format_tree	*ft;
	struct winlink		*wl;

	if (!(c->flags & CLIENT_CONTROL_READY)) {
		/* Don't issue spontaneous commands until the remote client has
		 * finished its initalization. It's ok because the remote
		 * client should fetch all window and layout info at the same
		 * time as it's marked ready. */
		return;
	}

	for (int i = 0; i < num_layouts_changed; i++) {
		struct window	*w = layouts_changed[i];
		if (w &&
		    c->session &&
		    winlink_find_by_window_id(&c->session->windows, w->id)) {
			/* When the last pane in a window is closed it won't
			 * have a layout root and we don't need to inform the
			 * client about its layout change because the whole
			 * window will go away soon. */
			if (w && w->layout_root) {
				const char *template =
				    "%layout-change #{window_id} "
				    "#{window_layout}";
				ft = format_create();
				wl = winlink_find_by_window(
				    &c->session->windows, w);
				if (wl) {
					format_winlink(ft, c->session, wl);
					control_write(c, "%s",
						      format_expand(ft, template));
				}
			}
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

	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

void
control_notify_window_removed(struct window *w)
{
	struct window_change	*change;
	int			 found_window;
        int			 is_create;

	for (int i = 0; i < num_layouts_changed; i++) {
		if (layouts_changed[i] == w) {
			layouts_changed[i] = NULL;
			break;
		}
	}
        /* If there is an existing change to the same window, remove it and
         * return.  Repeatedly search the list of window changes for any
         * relating to this window ID and remove them. Break out of the outer
         * loop when none exist. */
	found_window = 0;
        is_create = 0;
	do {
		found_window = 0;
		TAILQ_FOREACH(change, &window_changes, entry) {
			if (change->window_id == w->id) {
				TAILQ_REMOVE(&window_changes, change, entry);
				found_window = 1;
				if (change->action == WINDOW_CREATED)
				    is_create = 1;
				xfree(change);
				break;
			}
		}
	} while (found_window);

	if (is_create) {
          	/* We removed a queued WINDOW_CREATE so there's no need to queue a
                 * WINDOW_CLOSED. */
        	return;
        }

	/* Enqueue a WINDOW_CLOSED change. */
	change = xmalloc(sizeof(struct window_change));
	change->window_id = w->id;
	change->action = WINDOW_CLOSED;
	TAILQ_INSERT_TAIL(&window_changes, change, entry);

	control_notify_windows_changed();
}

void
control_notify_window_added(struct window *w)
{
	struct window_change	*change = xmalloc(sizeof(struct window_change));

	change->window_id = w->id;

	change->action = WINDOW_CREATED;
	TAILQ_INSERT_TAIL(&window_changes, change, entry);

	control_notify_windows_changed();
}

void
control_notify_window_renamed(struct window *w)
{
	struct window_change	*change;

	change = xmalloc(sizeof(struct window_change));
	change->window_id = w->id;
	change->action = WINDOW_RENAMED;
	TAILQ_INSERT_TAIL(&window_changes, change, entry);

	control_notify_windows_changed();
}

/* The currently attached session for this client changed. */
void
control_notify_attached_session_changed(struct client *c)
{
	if (c->flags & CLIENT_SESSION_CHANGED)
	    return;
	c->flags |= CLIENT_SESSION_CHANGED;
	session_changed_flags |= SESSION_CHANGE_ATTACHMENT;
	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

void
control_notify_session_renamed(struct session *s)
{
	session_changed_flags |= SESSION_CHANGE_RENAME;
	s->flags |= SESSION_RENAMED;
	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

void
control_notify_session_created(unused struct session *s)
{
	session_changed_flags |= SESSION_CHANGE_ADDREMOVE;
	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

void
control_notify_session_closed(unused struct session *s)
{
	session_changed_flags |= SESSION_CHANGE_ADDREMOVE;
	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

static void
control_notify_windows_changed(void)
{
	if (spontaneous_message_allowed)
	    control_broadcast_queue();
}

static void
control_write_windows_change_cb(struct client *c, unused void *user_data)
{
	struct window_change	*change;
	const char		*prefix;
	struct window		*w;
	struct winlink		*wl;

	if (!(c->flags & CLIENT_CONTROL_READY)) {
		/* Don't issue spontaneous commands until the remote client has
		 * finished its initalization. It's ok because the remote
		 * client should fetch all window and layout info at the same
		 * time as it's marked ready. */
		return;
	}
	if (!c->session)
	    return;

	TAILQ_FOREACH(change, &window_changes, entry) {
                /* A notification for a window not linked to the client's
                 * session gets a special notification (prefixed with
                 * "unlinked-") because clients are likely to do less in
                 * response to those, but at this point only the server knows
                 * which windows are linked to the client's session. */
		if (winlink_find_by_window_id(&c->session->windows,
					      change->window_id))
		    prefix = "";
		else
		    prefix = "unlinked-";
		switch (change->action) {
			case WINDOW_CREATED:
				control_write(c, "%%%swindow-add %u",
					      prefix, change->window_id);
				break;

			case WINDOW_CLOSED:
				control_write(c, "%%window-close %u",
					      change->window_id);
				break;

			case WINDOW_RENAMED:
				wl = winlink_find_by_window_id(
				    &c->session->windows, change->window_id);
				if (wl) {
					w = wl->window;
					control_write(
					     c, "%%window-renamed %u %s",
					     change->window_id, w->name);
				}
				break;

		}
	}
}

void
control_broadcast_queue(void)
{
	struct session	*s;

	if (session_changed_flags) {
		control_foreach_client(control_write_attached_session_change_cb,
				       NULL);
		session_changed_flags = 0;
		RB_FOREACH(s, sessions, &sessions) {
			s->flags &= ~SESSION_RENAMED;
		}
	}
	if (num_layouts_changed) {
		control_foreach_client(control_write_layout_change_cb, NULL);
		num_layouts_changed = 0;
		xfree(layouts_changed);
		layouts_changed = NULL;
	}
	if (!TAILQ_EMPTY(&window_changes)) {
		struct window_change *change;
		control_foreach_client(control_write_windows_change_cb, NULL);
		while ((change = TAILQ_FIRST(&window_changes))) {
			TAILQ_REMOVE(&window_changes, change, entry);
			xfree(change);
		}
	}
}

void
control_set_spontaneous_messages_allowed(int allowed)
{
	if (allowed && !spontaneous_message_allowed)
	    control_broadcast_queue();
	spontaneous_message_allowed = allowed;
}

void
control_handshake(struct client *c)
{
	if ((c->flags & CLIENT_SESSION_NEEDS_HANDSHAKE)) {
		/* If additional capabilities are added to tmux that do not
		 * break backward compatibility, they can be advertised
		 * after the protocol version. A semicolon should separate
		 * the version number from any optional parameters that follow.
		 * Parameters should themselves be semicolon delimited.
		 * Example:
		 *   _tmux1.0;foo;bar
		 * A 1.0-compatible client should work with such a version
		 * string, even if it does not know about the "foo" and "bar"
		 * features. The client may, at its discretion, use the foo and
		 * bar features when they are advertised this way. Future
		 * implementers should document or link to client requirements
		 * for such features here.
		 */
		control_write(
		    c, "%s", "\033_tmux" CURRENT_TMUX_CONTROL_PROTOCOL_VERSION
		    "\033\\%noop If you can see this message, "
		    "your terminal emulator does not support tmux mode "
		    "version " CURRENT_TMUX_CONTROL_PROTOCOL_VERSION ". Press "
		    "enter to return to your shell.");
		c->flags &= ~CLIENT_SESSION_NEEDS_HANDSHAKE;
	}
}

/* Print one line for each window in the session with the window number and the
 * layout. */
void
control_print_session_layouts(struct session *session, struct cmd_ctx *ctx)
{
	struct format_tree	*ft;
	struct winlink		*wl;
	struct winlinks		*wwl;

	wwl = &session->windows;
	RB_FOREACH(wl, winlinks, wwl) {
		const char *template = "#{window_id} #{window_layout}";
		ft = format_create();
		format_winlink(ft, session, wl);
		ctx->print(ctx, "%s", format_expand(ft, template));
	}
}

void
control_set_kvp(const char *name, const char *value)
{
	options_set_string(&control_options, name, "%s", value);
}

char *
control_get_kvp_value(const char *name)
{
    	struct options_entry	*o;

	o = options_find(&control_options, name);
	if (o == NULL || o->type != OPTIONS_STRING)
	    	return NULL;

	return o->str;
}

void control_init(void)
{
	TAILQ_INIT(&window_changes);
	options_init(&control_options, NULL);
}

/* Control input callback. Read lines and fire commands. */
void
control_callback(struct client *c, int closed, unused void *data)
{
	char		*line, *cause;
	struct cmd_ctx	 ctx;
	struct cmd_list	*cmdlist;

	if (closed)
		c->flags |= CLIENT_EXIT;

	for (;;) {
		line = evbuffer_readln(c->stdin_data, NULL, EVBUFFER_EOL_LF);
		if (line == NULL)
			break;
		if (*line == '\0') { /* empty line exit */
			c->flags |= CLIENT_EXIT;
			break;
		}

		ctx.msgdata = NULL;
		ctx.cmdclient = NULL;
		ctx.curclient = c;

		ctx.error = control_msg_error;
		ctx.print = control_msg_print;
		ctx.info = control_msg_info;

		if (cmd_string_parse(line, &cmdlist, &cause) != 0) {
			control_write(c, "%%error in line \"%s\": %s", line,
			    cause);
			xfree(cause);
		} else {
			cmd_list_exec(cmdlist, &ctx);
			cmd_list_free(cmdlist);
		}

		xfree(line);
	}
}
