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

/* Space needed to store the hex representation of a UTF-8 cell. */
#define CONTROL_HISTORY_UTF8_BUFFER_SIZE ((UTF8_SIZE) * 2 + 1)

/* Number of bytes needed to encode a cell's metadata. */
#define CONTROL_HISTORY_CONTEXT_SIZE 4

/* Max size of a control client's screen. Prevents a broken client
 * from crashing the tmux server. */
#define MAX_CONTROL_CLIENT_HEIGHT 20000
#define MAX_CONTROL_CLIENT_WIDTH 20000

int cmd_control_exec(struct cmd *, struct cmd_ctx *);

/* The subcommands are:
 * get-emulator: Output emulator state. -t gives pane.
 * get-history: Output history. -t gives pane. -l gives lines.
 *   -a means alternate screen.
 * get-value key: Output value from key-value store.
 * set-value key=value: Set "key" to "value" in key-value store.
 * set-client-size client-size: Set client size, value is like "80x25".
 * set-ready: Mark client ready for spontaneous messages.
 */
const struct cmd_entry cmd_control_entry = {
	"control", "control",
	"al:t:", 1, 2,
	"[-a] [-l lines] [-t target-pane] "
	"get-emulator|get-history|get-value|"
	"set-value|set-client-size|set-ready [client-size|key|key=value]",
	0,
	NULL,
	NULL,
	cmd_control_exec
};

static void
control_print_bool(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%u", name, value ? 1 : 0);
}

static void
control_print_uint(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%u", name, value);
}

static void
control_print_int(struct cmd_ctx *ctx, unsigned int value, const char *name)
{
	ctx->print(ctx, "%s=%d", name, value);
}

static void
control_print_bits(struct cmd_ctx *ctx, bitstr_t *value, int length,
		const char *name)
{
	struct evbuffer	*buffer;
	int		 separator = 0;
	char		*temp;

	buffer = evbuffer_new();
	for (int i = 0; i < length; i++) {
		if (bit_test(value, i)) {
			if (separator) {
				evbuffer_add(buffer, ",", 1);
			} else {
				separator = 1;
			}
			evbuffer_add_printf(buffer, "%d", i);
		}
	}
	temp = xmalloc(evbuffer_get_length(buffer) + 1);
	evbuffer_copyout(buffer, temp, evbuffer_get_length(buffer));
	temp[evbuffer_get_length(buffer)] = '\0';

	ctx->print(ctx, "%s=%s", name, temp);
	xfree(temp);
	evbuffer_free(buffer);
}

static void
control_print_string(struct cmd_ctx *ctx, char *str, const char *name)
{
	ctx->print(ctx, "%s=%s", name, str);
}

static void
control_print_hex(
    struct cmd_ctx *ctx, const char *bytes, size_t length, const char *name)
{
	struct evbuffer	*buffer = evbuffer_new();
	char		*temp;

	for (size_t i = 0; i < length; i++)
		evbuffer_add_printf(buffer, "%02x", ((int) bytes[i]) % 0xff);

	temp = xmalloc(evbuffer_get_length(buffer) + 1);
	evbuffer_copyout(buffer, temp, evbuffer_get_length(buffer));
	temp[evbuffer_get_length(buffer)] = '\0';

	ctx->print(ctx, "%s=%s", name, temp);

	xfree(temp);
	evbuffer_free(buffer);
}

/* Return a hex-encoded version of utf8data. */
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
control_history_output_last_char(struct evbuffer *last_char,
				 struct evbuffer *output, int *repeats)
{
	if (evbuffer_get_length(last_char) > 0) {
		evbuffer_add_buffer(output, last_char);
		if (*repeats == 2 && evbuffer_get_length(last_char) <= 3)
			/* If an ASCII code repeats once then it's shorter to
			 * print it twice than to use the run-length encoding.
			 */
			evbuffer_add_buffer(output, last_char);
		else if (*repeats > 1)
			/* Output "*<n> " to indicate that the last character
			 * repeats <n> times. For instance, "AAA" is
			 * represented as "61*3". */
			evbuffer_add_printf(output, "*%d ", *repeats);
		evbuffer_drain(last_char, evbuffer_get_length(last_char));
	}
}

static int
control_evbuffer_strcmp(struct evbuffer *a, struct evbuffer *b)
{
	char	*temp_a;
	char	*temp_b;
	temp_a = xmalloc(evbuffer_get_length(a) + 1);
	temp_b = xmalloc(evbuffer_get_length(b) + 1);
	evbuffer_copyout(a, temp_a, evbuffer_get_length(a));
	evbuffer_copyout(b, temp_b, evbuffer_get_length(b));
	temp_a[evbuffer_get_length(a)] = 0;
	temp_b[evbuffer_get_length(b)] = 0;
	int rc = strcmp(temp_a, temp_b);
	xfree(temp_a);
	xfree(temp_b);
	return rc;
}

static void
control_history_append_char(struct grid_cell *celldata,
			       struct grid_utf8 *utf8data,
			       struct evbuffer *last_char, int *repeats,
			       struct evbuffer *output)
{
	struct evbuffer	*buffer = evbuffer_new();

	if (celldata->flags & GRID_FLAG_UTF8) {
		char temp[CONTROL_HISTORY_UTF8_BUFFER_SIZE + 3];
		control_history_encode_utf8(utf8data, temp);
		evbuffer_add_printf(
		    buffer, "[%s]", control_history_encode_utf8(utf8data, temp));
	} else
		evbuffer_add_printf(buffer, "%x", ((int) celldata->data) & 0xff);
	if (evbuffer_get_length(last_char) > 0 && !control_evbuffer_strcmp(buffer, last_char)) {
		/* Last character repeated */
		(*repeats)++;
	} else {
		/* Not a repeat */
		control_history_output_last_char(last_char, output, repeats);
		evbuffer_add_buffer(last_char, buffer);
		*repeats = 1;
	}
	evbuffer_free(buffer);
}

static void
control_history_cell(struct evbuffer *output, struct grid_cell *celldata,
		     struct grid_utf8 *utf8data, int *dump_context,
		     struct evbuffer *last_char, int *repeats)
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
		evbuffer_add_printf(output, ":%x,%x,%x,%x,", celldata->attr,
			   celldata->flags, celldata->fg, celldata->bg);
	}
	control_history_append_char(celldata, utf8data, last_char, repeats,
				       output);
}

static void
control_history_line(struct cmd_ctx *ctx, struct grid_line *linedata,
		     int *dump_context)
{
	unsigned int	 i;
	struct evbuffer	*last_char;
	struct evbuffer	*output;
	int		 repeats = 0;
        char		*temp;

	output = evbuffer_new();
	last_char = evbuffer_new();
	for (i = 0; i < linedata->cellsize; i++)
	    control_history_cell(
		output, linedata->celldata + i, linedata->utf8data + i,
		dump_context, last_char, &repeats);
	control_history_output_last_char(last_char, output, &repeats);
	if (linedata->flags & GRID_LINE_WRAPPED)
	    evbuffer_add(output, "+", 1);
        temp = xmalloc(evbuffer_get_length(output) + 1);
        temp[evbuffer_get_length(output)] = '\0';
        evbuffer_copyout(output, temp, evbuffer_get_length(output));
	ctx->print(ctx, "%s", temp);
        xfree(temp);
	evbuffer_free(output);
	evbuffer_free(last_char);
}

/* This command prints the contents of the screen plus its history.
 * The encoding includes not just the text but also the per-cell
 * context, such as colors, bold flags, etc. To encode this efficiently,
 * a runlength encoding scheme is used.
 * Each row is output on one line, terminated with a newline.
 * The context is output as four comma-separated hex values preceded by
 * a colon and terminated with a comma. Output begins with context and
 * is followed by characters. New context may be output at any time after
 * the end of a character.
 * The cells' characters are encoded as either two-digit hex values
 * (for example, 61 for 'A') or, for UTF-8 cells, a sequence of concatenated
 * two-digit hex values inside square brackets (for example, [65cc81] for
 * LATIN SMALL LETTER E followed by COMBINING ACUTE ACCENT).
 *
 * Example:
 *   :0,0,8,8,6120[c3a9]:1,0,8,8,67
 * Interpretation:
 *   First comes a context with normal foreground and background and no
 *   character attributes (:0,0,8,8,). Then characters:
 *   LATIN SMALL LETTER A (61)
 *   SPACE (20)
 *   LATIN SMALL LETTER E WITH ACUTE ([c3a9])
 *   Then a new context with the bold flag on (:1,0,8,8,), followed by the
 *     character:
 *   LATIN SMALL LETTER G (67)
 */
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
	max_lines = temp;  /* assign to unsigned int to do comparisons later */

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
        char			*temp;

	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	control_print_int(ctx, wp->saved_grid ? 1 : 0, "in_alternate_screen");
	/* This is the saved cursor position from when the alternate screen was
	 * entered. */
	control_print_uint(ctx, wp->saved_cx, "base_cursor_x");
	control_print_uint(ctx, wp->saved_cy, "base_cursor_y");
	control_print_uint(ctx, wp->base.cx, "cursor_x");
	control_print_uint(ctx, wp->base.cy, "cursor_y");
	control_print_uint(ctx, wp->base.rupper, "scroll_region_upper");
	control_print_uint(ctx, wp->base.rlower, "scroll_region_lower");
	control_print_bits(ctx, wp->base.tabs, wp->base.grid->sx, "tabstops");
	control_print_string(ctx, wp->window->name, "title");
	control_print_bool(ctx, !!(wp->base.mode & MODE_CURSOR), "cursor_mode");
	control_print_bool(ctx, !!(wp->base.mode & MODE_INSERT), "insert_mode");
	control_print_bool(
	    ctx, !!(wp->base.mode & MODE_KCURSOR), "kcursor_mode");
	control_print_bool(
	    ctx, !!(wp->base.mode & MODE_KKEYPAD), "kkeypad_mode");
	control_print_bool(ctx, !!(wp->base.mode & MODE_WRAP), "wrap_mode");
	control_print_bool(
	    ctx, !!(wp->base.mode & MODE_MOUSE_STANDARD),
	    "mouse_standard_mode");
	control_print_bool(
	   ctx, !!(wp->base.mode & MODE_MOUSE_BUTTON), "mouse_button_mode");
	control_print_bool(
	    ctx, !!(wp->base.mode & MODE_MOUSE_ANY), "mouse_any_mode");
	control_print_bool(
	    ctx, !!(wp->base.mode & MODE_MOUSE_UTF8), "mouse_utf8_mode");

	/* This is the saved cursor position from CSI DECSC. */
	control_print_int(ctx, wp->ictx.old_cx, "decsc_cursor_x");
	control_print_int(ctx, wp->ictx.old_cy, "decsc_cursor_y");
	if (evbuffer_get_length(wp->ictx.input_since_ground)) {
		temp = xmalloc(evbuffer_get_length(wp->ictx.input_since_ground) + 1);
		evbuffer_copyout(wp->ictx.input_since_ground,
				 temp,
				 evbuffer_get_length(wp->ictx.input_since_ground));
		temp[evbuffer_get_length(wp->ictx.input_since_ground)] = '\0';
		control_print_hex(ctx,
			temp,
			evbuffer_get_length(wp->ictx.input_since_ground),
			"pending_output");
		xfree(temp);
	}
	return (0);
}

static int
control_kvp_command(
    unused struct cmd *self, struct cmd_ctx *ctx, const char *name)
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
control_parse_size(const char *size, u_int *w, u_int *h)
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
control_set_client_size(struct client *c, u_int w, u_int h, struct cmd_ctx *ctx)
{
	if (tty_set_size(&c->tty, w, h) > 0)
		recalculate_sizes();
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
	if (control_parse_size(value, &w, &h))
		return (-1);
	/* Prevent a broken client from making us use crazy amounts of
	 * memory */
	if (w > MAX_CONTROL_CLIENT_WIDTH ||
	    h > MAX_CONTROL_CLIENT_HEIGHT)
		return (-1);
	control_set_client_size(c, w, h, ctx);
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
