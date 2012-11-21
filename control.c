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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Version number history:
 * There may be some binaries in the world with 0.1, 0.2, 0.3, and
 * 0.4. These were pre-release test versions.
 * 0.5: First July release of iTerm for Mountain Lion, plus tmux.
 */
#define CURRENT_TMUX_CONTROL_PROTOCOL_VERSION "0.5"

typedef void control_write_cb(struct client *c, void *user_data);

/* Global key-value pairs. */
struct options	 control_options;

/* Stored text to send control clients. */
struct control_input_ctx {
	struct window_pane *wp;
	const u_char *buf;
	size_t len;
};

void	control_error_callback(struct bufferevent *, short, void *);
void printflike2 control_msg_error(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_print(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_info(struct cmd_ctx *, const char *, ...);
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
	options_init(&control_options, NULL);
}

/* Write a buffer, adding a terminal newline. Empties buffer. */
void
control_write_buffer(struct client *c, struct evbuffer *buffer)
{
	evbuffer_add_buffer(c->stdout_data, buffer);
	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
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
			free(cause);
		} else {
			cmd_list_exec(cmdlist, &ctx);
			cmd_list_free(cmdlist);
		}

		free(line);
	}
}
