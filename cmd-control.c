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

struct control_history_cell {
	int attr, flags, fg, bg;  /* Context */
	/* Holds a 2-byte latin1 char or a UTF-8 char inside square brackets. */
	char encoded[CONTROL_HISTORY_UTF8_BUFFER_SIZE + 3];
	int repeats;
	int begins_new_context;
};

int cmd_control_exec(struct cmd *, struct cmd_ctx *);

/* The subcommands are:
 * get-emulatorstate: Output emulator state. -t gives pane.
 * get-history: Output history. -t gives pane. -l gives lines.
 *   -a means alternate screen.
 * get-value key: Output value from key-value store.
 * set-value key=value: Set "key" to "value" in key-value store.
 * set-client-size client-size: Set client size, value is like "80x25".
 * set-ready: Mark client ready for notifications.
 */
const struct cmd_entry cmd_control_entry = {
	"control", "control",
	"al:t:", 1, 2,
	"[-a] [-l lines] [-t target-pane] "
	"get-emulatorstate|get-history|get-value|"
	"set-value|set-client-size|set-ready [client-size|key|key=value]",
	0,
	NULL,
	NULL,
	cmd_control_exec
};

static struct evbuffer *
control_format_tabstops_string(bitstr_t *value, int length)
{
	struct evbuffer	 	*buffer;

	buffer = evbuffer_new();
	for (int i = 0; i < length; i++) {
		if (bit_test(value, i)) {
			if (EVBUFFER_LENGTH(buffer) > 0) {
				evbuffer_add(buffer, ",", 1);
			}
			evbuffer_add_printf(buffer, "%d", i);
		}
	}
	return buffer;
}

static struct evbuffer *
control_format_hex_string(const char *s, size_t length)
{
	struct evbuffer	 	*buffer;

	buffer = evbuffer_new();
	for (size_t i = 0; i < length; i++)
		evbuffer_add_printf(buffer, "%02x", ((int) s[i]) % 0xff);
	return buffer;
}

/* Return a hex-encoded version of utf8data. */
static char *
control_concat_utf8(struct grid_utf8 *utf8data, char *buffer)
{
	int		o;
	unsigned int	i;
	unsigned char	c;

	o = 0;
	buffer[o++] = '[';
	size_t size = grid_utf8_size(utf8data);
	for (i = 0; i < size; i++) {
		c = utf8data->data[i];
		sprintf(buffer + o, "%02x", (int)c);
		o += 2;
	}
	buffer[o++] = ']';
	buffer[o++] = '\0';
	return buffer;
}

static int
control_history_cells_equal(struct control_history_cell *a,
			    struct control_history_cell *b)
{
	return (a->attr == b->attr &&
		a->flags == b->flags &&
		a->fg == b->fg &&
		a->bg == b->bg);
}

static int
control_history_compact_cells(struct control_history_cell *cells, int count)
{
	int		 i, o = 0;

	for (i = 0; i < count; i++) {
	       	cells[i].begins_new_context =
		    (o == 0 ||
		     !control_history_cells_equal(&cells[i], &cells[o - 1]));
		if (o > 0 &&
		    !cells[i].begins_new_context &&
		    !strcmp(cells[i].encoded, cells[o - 1].encoded)) {
			cells[o - 1].repeats++;
		} else {
			cells[o++] = cells[i];
		}
	}
	return o;
}

/* Print a single line of history to the client's stdout. */
static void
control_history_print_line(struct cmd_ctx *ctx, struct grid_line *linedata)
{
	struct control_history_cell	*cells;
	size_t				 bytes;
	unsigned int	 		 i, j, n, iter, len;
	int				 flag_mask;
	int				 cell;

	bytes = sizeof(struct control_history_cell) * linedata->cellsize;
	cells = (struct control_history_cell *)malloc(bytes);
	flag_mask = (GRID_FLAG_FG256 | GRID_FLAG_BG256 | GRID_FLAG_PADDING |
		     GRID_FLAG_UTF8);
	/* Construct an array of control_history_cell's. */
	cell = 0;
	for (i = 0; i < linedata->cellsize; i++) {
		cells[i].attr = linedata->celldata[i].attr;
		cells[i].flags = (linedata->celldata[i].flags & flag_mask);
		if (cells[i].flags & GRID_FLAG_UTF8)
		    control_concat_utf8(linedata->utf8data + i,
					cells[i].encoded + 1);
		else
		    sprintf(cells[i].encoded, "%02x",
			    (linedata->celldata[i].data & 0xff));
		cells[i].fg = linedata->celldata[i].fg;
		cells[i].bg = linedata->celldata[i].bg;
		cells[i].repeats = 1;
	}

	/* Compact cells, combining identical adjacent cells and incrementing
	 * the repeats counts, and setting the begins_new_context flag
	 * appropriately. */
	n = control_history_compact_cells(cells, linedata->cellsize);

        /* Write output. */
	for (i = 0; i < n; i++) {
		if (cells[i].begins_new_context) {
			evbuffer_add_printf(ctx->curclient->stdout_data,
					    "<%x,%x,%x,%x>",
					    cells[i].attr,
					    cells[i].flags,
					    cells[i].fg,
					    cells[i].bg);
		}
		len = strlen(cells[i].encoded);
		iter = cells[i].repeats > 2 ? 1 : cells[i].repeats;
		for (j = 0; j < iter; j++) {
			evbuffer_add(ctx->curclient->stdout_data,
				     cells[i].encoded, len);
		}
		if (cells[i].repeats > 2) {
			evbuffer_add_printf(ctx->curclient->stdout_data, "*%d ",
					    cells[i].repeats);
		}
	}

	if (linedata->flags & GRID_LINE_WRAPPED)
	    evbuffer_add(ctx->curclient->stdout_data, "+", 1);
	evbuffer_add(ctx->curclient->stdout_data, "\n", 1);
	server_push_stdout(ctx->curclient);
	free(cells);
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
 * Repeated characters are suffixed with "*n " where n is a decimal value giving
 * the number of repeats. If a line is terminated with a '+', then it is a
 * "soft" newline (i.e., a long line that happened to wrap at the right edge).
 *
 * Example:
 *   <0,0,8,8>6120[c3a9]<1,0,8,8>67*3 +
 *
 *   Text       Interpretation
 *   <0,0,8,8>  Set foreground and background to default colors, reset attrs.
 *   61		'a'
 *   20		' '
 *   [c3a9]     Lowercase 'e' with an acute accent.
 *   <1,0,8,8>  Turn on bold
 *   67*3       'ggg'
 *   +          The next line is a continuation of this line.
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
		control_history_print_line(ctx, grid->linedata + i);
	return (0);
}

static int
control_emulatorstate_command(struct cmd *self, struct cmd_ctx *ctx)
{
	struct format_tree	*ft;
	struct args		*args = self->args;
	struct window_pane	*wp;
	struct session		*s;
	struct evbuffer		*tabstops, *pending;
	const char *template = "saved_grid=#{saved_grid}\n"
			       "saved_cx=#{saved_cx}\n"
			       "saved_cy=#{saved_cy}\n"
			       "cursor_x=#{cursor_x}\n"
			       "cursor_y=#{cursor_y}\n"
			       "scroll_region_upper=#{scroll_region_upper}\n"
			       "scroll_region_lower=#{scroll_region_lower}\n"
			       "tabstops=#{tabstops}\n"
			       "title=#{title}\n"
			       "cursor_mode=#{cursor_mode}\n"
			       "insert_mode=#{insert_mode}\n"
			       "kcursor_mode=#{kcursor_mode}\n"
			       "kkeypad_mode=#{kkeypad_mode}\n"
			       "wrap_mode=#{wrap_mode}\n"
			       "mouse_standard_mode=#{mouse_standard_mode}\n"
			       "mouse_button_mode=#{mouse_button_mode}\n"
			       "mouse_any_mode=#{mouse_any_mode}\n"
			       "mouse_utf8_mode=#{mouse_utf8_mode}\n"
			       "decsc_cursor_x=#{decsc_cursor_x}\n"
			       "decsc_cursor_y=#{decsc_cursor_y}\n"
			       "pending_output=#{pending_output}";
	if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
		return (-1);

	ft = format_create();
	format_add(ft, "saved_grid", "%d", wp->saved_grid ? 1 : 0);
	format_add(ft, "saved_cx", "%d", wp->saved_cx);
	format_add(ft, "saved_cy", "%d", wp->saved_cy);
	format_add(ft, "cursor_x", "%d", wp->base.cx);
	format_add(ft, "cursor_y", "%d", wp->base.cy);
	format_add(ft, "scroll_region_upper", "%d", wp->base.rupper);
	format_add(ft, "scroll_region_lower", "%d", wp->base.rlower);
	tabstops = control_format_tabstops_string(wp->base.tabs,
						  wp->base.grid->sx);
	format_add(ft, "tabstops", "%.*s",
		   (int) EVBUFFER_LENGTH(tabstops), EVBUFFER_DATA(tabstops));
	format_add(ft, "window_name", "%s", wp->window->name);
	format_add(ft, "cursor_mode", "%d",
		   (int) !!(wp->base.mode & MODE_CURSOR));
	format_add(ft, "insert_mode", "%d",
		   (int) !!(wp->base.mode & MODE_INSERT));
	format_add(ft, "kcursor_mode", "%d",
		   (int) !!(wp->base.mode & MODE_KCURSOR));
	format_add(ft, "kkeypad_mode", "%d",
		   (int) !!(wp->base.mode & MODE_KKEYPAD));
	format_add(ft, "wrap_mode", "%d", (int) !!(wp->base.mode & MODE_WRAP));
	format_add(ft, "mouse_standard_mode", "%d",
		   (int) !!(wp->base.mode & MODE_MOUSE_STANDARD));
	format_add(ft, "mouse_button_mode", "%d",
		   (int) !!(wp->base.mode & MODE_MOUSE_BUTTON));
	format_add(ft, "mouse_any_mode", "%d",
		   (int) !!(wp->base.mode & MODE_MOUSE_ANY));
	format_add(ft, "mouse_utf8_mode", "%d",
		   (int) !!(wp->base.mode & MODE_MOUSE_UTF8));

	/* This is the saved cursor position from CSI DECSC. */
	format_add(ft, "decsc_cursor_x", "%d", wp->ictx.old_cx);
	format_add(ft, "decsc_cursor_y", "%d", wp->ictx.old_cy);

	pending = control_format_hex_string(
		EVBUFFER_DATA(wp->ictx.since_ground),
		EVBUFFER_LENGTH(wp->ictx.since_ground));
	format_add(ft, "pending_output", "%.*s",
		   (int) EVBUFFER_LENGTH(pending),
		   EVBUFFER_DATA(pending));

	ctx->print(ctx, "%s", format_expand(ft, template));

	evbuffer_free(tabstops);
	evbuffer_free(pending);

	return (0);
}

static struct options *
control_get_options(void)
{
	static int		initialized;
        static struct options	control_options;

        if (!initialized) {
		options_init(&control_options, NULL);
                initialized = 1;
        }
        return &control_options;
}

static int
control_get_kvp_command(
    unused struct cmd *self, struct cmd_ctx *ctx, const char *name)
{
	char		*value;
    	struct options_entry	*o;

	o = options_find(control_get_options(), name);
	if (o == NULL || o->type != OPTIONS_STRING)
	    return (-1);

	value = o->str;

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
	u_int		 w, h;

	if (!value) {
		ctx->error(ctx, "no value given");
		return (-1);
	}
	c = cmd_find_client(ctx, NULL);
	if (!c)
		return (-1);
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
		free(temp);
		return (-1);
	}
        *eq = '\0';
	options_set_string(control_get_options(), temp, "%s", eq + 1);
	free(temp);
	return (0);
}

int
cmd_control_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args	*args = self->args;
	const char	*subcommand;
	const char	*value;

	subcommand = args->argv[0];

	if (!strcmp(subcommand, "get-emulatorstate"))
	    return control_emulatorstate_command(self, ctx);
	else if (!strcmp(subcommand, "get-history"))
	    return control_history_command(self, ctx);
	else if (!strcmp(subcommand, "get-value")) {
	    if (args->argc != 2)
		return (-1);
	    value = args->argv[1];
	    return control_get_kvp_command(self, ctx, value);
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
