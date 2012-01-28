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

#define CONTROL_HISTORY_UTF8_BUFFER_SIZE ((UTF8_SIZE) * 2 + 1)
#define CONTROL_HISTORY_CONTEXT_SIZE 4
#define MAX_CONTROL_CLIENT_HEIGHT 20000
#define MAX_CONTROL_CLIENT_WIDTH 20000

/*
 * Output information needed by control clients, including history, cursor
 * position, and miscellaneous VT100 state.
 */

int cmd_control_exec(struct cmd *, struct cmd_ctx *);

/*
 * get-emulator: Output emulator state. -t gives pane.
 * get-history: Output history. -t gives pane. -l gives lines. -a means alternate screen.
 * get-value key: Output value from key-value store.
 * set-value key=value: Set "key" to "value" in key-value store.
 * set-client-size client-size: Set client size, value is like "80x25".
 * set-ready: Mark client ready for spontaneous messages.
 */
const struct cmd_entry cmd_control_entry = {
	"control", "control",
	"at:l:", 1, 2,
	"[-a] [-t target-pane] [-l lines]"
	    "get-emulator|get-history|get-value|"
	    "set-value|set-client-size|set-ready [client-size|key|key=value]",
	0,
	NULL,
	NULL,
	cmd_control_exec
};

static void
control_uint(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%u", name, value);
}

static void
control_int(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%d", name, value);
}

static void
control_bits(struct cmd_ctx *ctx, bitstr_t *value, int length,
		const char *name)
{
	struct dstring	ds;
	int		separator = 0;

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
control_string(struct cmd_ctx *ctx, char *str, const char *name)
{
	struct dstring	ds;
	ds_init(&ds);

	ctx->print(ctx, "%s=%s", name, str);
	ds_free(&ds);
}

static void
control_hex(struct cmd_ctx *ctx, const char *bytes, size_t length,
	       const char *name)
{
	struct dstring	ds;
	ds_init(&ds);
	for (size_t i = 0; i < length; i++) {
		ds_appendf(&ds, "%02x", ((int) bytes[i]) % 0xff);
	}

	ctx->print(ctx, "%s=%s", name, ds.buffer);
	ds_free(&ds);
}

/* Return a hex encoded version of utf8data. */
static char *
control_history_encode_utf8(struct grid_utf8 *utf8data, char *buffer)
{
	int		o;
	unsigned int	i;
	unsigned char	c;

	o = 0;
	size_t size = grid_utf8_size(utf8data);
	for (i = 0; i < size; i++) {
		c = utf8data->data[i];
		sprintf(buffer + o, "%02x", (int)c);
		o += 2;
	}
	return buffer;
}

static void
control_history_output_last_char(struct dstring *last_char,
				    struct dstring *output, int *repeats)
{
	if (last_char->used > 0) {
		ds_append(output, last_char->buffer);
		if (*repeats == 2 && last_char->used <= 3) {
			/* If an ASCII code repeats once then it's shorter to
			 * print it twice than to use the run-length encoding.
			 * */
			ds_append(output, last_char->buffer);
		} else if (*repeats > 1) {
			/* Output "*<n> " to indicate that the last character
			 * repeats <n> times. For instance, "AAA" is
			 * represented as "61*3". */
			ds_appendf(output, "*%d ", *repeats);
		}
		ds_truncate(last_char, 0);
	}
}

static void
control_history_append_char(struct grid_cell *celldata,
			       struct grid_utf8 *utf8data,
			       struct dstring *last_char, int *repeats,
			       struct dstring *output)
{
	struct dstring	ds;
	ds_init(&ds);

	if (celldata->flags & GRID_FLAG_UTF8) {
		char temp[CONTROL_HISTORY_UTF8_BUFFER_SIZE + 3];
		control_history_encode_utf8(utf8data, temp);
		ds_appendf(&ds, "[%s]",
			   control_history_encode_utf8(utf8data, temp));
	} else {
		ds_appendf(&ds, "%x", ((int) celldata->data) & 0xff);
	}
	if (last_char->used > 0 && !strcmp(ds.buffer, last_char->buffer)) {
		/* Last character repeated */
		(*repeats)++;
	} else {
		/* Not a repeat */
		control_history_output_last_char(last_char, output, repeats);
		ds_append(last_char, ds.buffer);
		*repeats = 1;
	}
}

static void
control_history_cell(struct dstring *output, struct grid_cell *celldata,
			struct grid_utf8 *utf8data, int *dump_context,
			struct dstring *last_char, int *repeats)
{
	int	flags;

	/* Exclude the GRID_FLAG_UTF8 flag because it's wasteful to output when
	 * UTF-8 chars are already marked by being enclosed in square brackets.
	 */
	flags  = celldata->flags & (GRID_FLAG_FG256 | GRID_FLAG_BG256 |
				    GRID_FLAG_PADDING);
	if (celldata->attr != dump_context[0] ||
	    flags != dump_context[1] ||
	    celldata->fg != dump_context[2] ||
	    celldata->bg != dump_context[3]) {
		/* Context has changed since the last character. */
		dump_context[0] = celldata->attr;
		dump_context[1] = flags;
		dump_context[2] = celldata->fg;
		dump_context[3] = celldata->bg;

		control_history_output_last_char(last_char, output, repeats);
		ds_appendf(output, ":%x,%x,%x,%x,", celldata->attr,
			   celldata->flags, celldata->fg, celldata->bg);
	}
	control_history_append_char(celldata, utf8data, last_char, repeats,
				       output);
}

static void
control_history_line(struct cmd_ctx *ctx, struct grid_line *linedata,
			int *dump_context)
{
	unsigned int	i;
	struct dstring	last_char;
	struct dstring	output;

	ds_init(&output);
	ds_init(&last_char);
	int repeats = 0;
	for (i = 0; i < linedata->cellsize; i++) {
		control_history_cell(&output, linedata->celldata + i,
					linedata->utf8data + i, dump_context, &last_char,
					&repeats);
	}
	control_history_output_last_char(&last_char, &output, &repeats);
	if (linedata->flags & GRID_LINE_WRAPPED) {
		ds_appendf(&output, "+");
	}
	ctx->print(ctx, "%s", output.buffer);
	ds_free(&output);
}

static int
control_history_command(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args		*args = self->args;
	struct window_pane	*wp;
	struct session		*s;
	const char		*max_lines_str;
	unsigned		 max_lines;
	int			 temp;
	unsigned int		 i;
	unsigned int		 start, limit;
	struct grid		*grid;
	int			 dump_context[CONTROL_HISTORY_CONTEXT_SIZE] =
		{ -1, -1, -1, -1 };

	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	max_lines_str = args_get(args, 'l');
	if (!max_lines_str)
		return (-1);
	temp = atoi(max_lines_str);
	if (temp <= 0)
		return (-1);
	max_lines = temp;  /* assign to unsigned to do comparisons later */

	if (args_has(args, 'a')) {
		grid = wp->saved_grid;
		if (!grid)
			return (0);
	} else
		grid = wp->base.grid;
	limit = grid->hsize + grid->sy;
	if (limit >= max_lines)
		start = limit - max_lines;
	else
		start = 0;
	for (i = start; i < limit; i++)
		control_history_line(ctx, grid->linedata + i, dump_context);
	return (0);
}

static int
control_emulator_command(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args		*args = self->args;
	struct window_pane	*wp;
	struct session		*s;

	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	control_int(ctx, wp->saved_grid ? 1 : 0, "in_alternate_screen");
	/* This is the saved cursor position from when the alternate screen was
	 * entered. */
	control_uint(ctx, wp->saved_cx, "base_cursor_x");
	control_uint(ctx, wp->saved_cy, "base_cursor_y");
	control_uint(ctx, wp->base.cx, "cursor_x");
	control_uint(ctx, wp->base.cy, "cursor_y");
	control_uint(ctx, wp->base.rupper, "scroll_region_upper");
	control_uint(ctx, wp->base.rlower, "scroll_region_lower");
	control_bits(ctx, wp->base.tabs, wp->base.grid->sx, "tabstops");
	control_string(ctx, wp->base.title, "title");

	/* This is the saved cursor position from CSI DECSC. */
	control_int(ctx, wp->ictx.old_cx, "decsc_cursor_x");
	control_int(ctx, wp->ictx.old_cy, "decsc_cursor_y");
	if (wp->ictx.input_since_ground.used) {
		control_hex(ctx,
			wp->ictx.input_since_ground.buffer,
			wp->ictx.input_since_ground.used,
			"pending_output");
	}
	return (0);
}

static int
control_kvp_command(unused struct cmd *self, struct cmd_ctx *ctx, const char *name)
{
	char		*value;

	value = control_get_kvp_value(name);
	if (value)
		ctx->print(ctx, "%s", value);
	else
		ctx->print(ctx, "%s", "");

	return (0);
}

/* "size" should be formatted as "int,int". If it is well formed, then *w and
 * *h will be populated with the first and second ints, respectively and 0 is
 * returned. If an error is encountered, -1 is returned. */
static int
parse_size(const char *size, u_int *w, u_int *h)
{
	char	*endptr, *temp;

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

static int
control_set_client_size_command(struct cmd_ctx *ctx, const char *value)
{
	struct client   *c;

	if (!value) {
		ctx->error(ctx, "no value given");
		return (-1);
	}
	c = cmd_find_client(ctx, NULL);
	if (!c)
		return (-1);
	u_int	w, h;
	if (parse_size(value, &w, &h))
		return (-1);
	/* Prevent a broken client from making us use crazy amounts of
	 * memory */
	if (w > MAX_CONTROL_CLIENT_WIDTH ||
	    h > MAX_CONTROL_CLIENT_HEIGHT)
		return (-1);
	set_client_size(c, w, h, ctx);
	return (0);
}

static int
control_set_ready_command(struct cmd_ctx *ctx)
{
	struct client   *c;

	c = cmd_find_client(ctx, NULL);
	if (c)
		c->flags |= CLIENT_CONTROL_READY;
	return (0);
}

static int
control_set_kvp_command(struct cmd_ctx *ctx, const char *value)
{
	char		*temp;
	char		*eq;

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
	return (0);
}

int
cmd_control_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args	*args = self->args;
	const char	*subcommand;
	const char	*value;

	subcommand = args->argv[0];

	if (!strcmp(subcommand, "get-emulator"))
		return control_emulator_command(self, ctx);
	else if (!strcmp(subcommand, "get-history"))
		return control_history_command(self, ctx);
	else if (!strcmp(subcommand, "get-value")) {
		if (args->argc != 2)
		    return (-1);
		value = args->argv[1];
		return control_kvp_command(self, ctx, value);
	} else if (!strcmp(subcommand, "set-client-size")) {
		if (args->argc != 2)
		    return (-1);
		value = args->argv[1];
		return control_set_client_size_command(ctx, value);
	} else if (!strcmp(subcommand, "set-ready")) {
		return control_set_ready_command(ctx);
	} else if (!strcmp(subcommand, "set-value")) {
		value = args->argv[1];
		return control_set_kvp_command(ctx, value);
	} else
		    return (-1);
}
