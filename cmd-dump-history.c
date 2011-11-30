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

#define DUMP_HISTORY_UTF8_BUFFER_SIZE ((UTF8_SIZE) * 2 + 1)
#define DUMP_HISTORY_CONTEXT_SIZE 4
#define DSTRING_STATIC_BUFFER_SIZE 1024

/*
 * Print out the last n lines of history plus screen contents.
 */

/* dstring is a dynamic string. It is null terminated. */
struct dstring {
    char *buffer;  /* always points at the current value */
    int used;
    int available;
    char staticbuffer[DSTRING_STATIC_BUFFER_SIZE];
};

int cmd_dump_history_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_dump_history_entry = {
    "dump-history", "dumphist",
    "al:t:", 0, 0,
    "",
    0,
    NULL,
    NULL,
    cmd_dump_history_exec
};

/* Return a hex encoded version of utf8data. */
static char *
dump_history_encode_utf8(struct grid_utf8 *utf8data, char *buffer)
{
    int           o;
    int           i;
    unsigned char c;

    o = 0;
    for (i = 0; i < utf8data->width; i++) {
        c = utf8data->data[i];
        sprintf(buffer + o, "[%02x]", (int)c);
        o += 2;
    }
    return buffer;
}

static void
ds_init(struct dstring *ds)
{
    ds->buffer = ds->staticbuffer;
    ds->used = 0;
    ds->available = DSTRING_STATIC_BUFFER_SIZE;
    ds->buffer[0] = '\0';
}

static void
ds_free(struct dstring *ds)
{
    if (ds->buffer != ds->staticbuffer) {
        xfree(ds->buffer);
    }
}

static void
ds_appendf(struct dstring *ds, const char *fmt, ...)
{
    char    *temp;
    va_list ap;
    int     len;

    va_start(ap, fmt);
    xvasprintf(&temp, fmt, ap);
    va_end(ap);

    len = strlen(temp);
    if (ds->used + len >= ds->available) {
        ds->available *= 2;
        if (ds->buffer == ds->staticbuffer) {
            ds->buffer = xmalloc(ds->available);
            memmove(ds->buffer, ds->staticbuffer, ds->used);
        } else {
            ds->buffer = xrealloc(ds->buffer, ds->available, 1);
        }
    }
    memmove(ds->buffer + ds->used, temp, len);
    xfree(temp);
    ds->used += len;
    ds->buffer[ds->used] = '\0';
}

static void
dump_history_cell(struct dstring *output, struct grid_cell *celldata,
                  struct grid_utf8 *utf8data, int *dump_context)
{
    if (celldata->attr == dump_context[0] &&
        celldata->flags == dump_context[1] &&
        celldata->fg == dump_context[2] &&
        celldata->bg == dump_context[3]) {
        /* Don't repeat context if it's the same as the last character. */
        if (celldata->flags & GRID_FLAG_UTF8) {
            char temp[DUMP_HISTORY_UTF8_BUFFER_SIZE];
            ds_appendf(output, "%s", dump_history_encode_utf8(utf8data, temp));
        } else {
            ds_appendf(output, "%x", celldata->data);
        }
    } else {
        dump_context[0] = celldata->attr;
        dump_context[1] = celldata->flags;
        dump_context[2] = celldata->fg;
        dump_context[3] = celldata->bg;

        if (celldata->flags & GRID_FLAG_UTF8) {
            char temp[DUMP_HISTORY_UTF8_BUFFER_SIZE];
            ds_appendf(output, ":%x,%x,%x,%x,%s", celldata->attr,
                       celldata->flags, celldata->fg, celldata->bg,
                       dump_history_encode_utf8(utf8data, temp));
        } else {
            ds_appendf(output, ":%x,%x,%x,%x,%x", celldata->attr,
                       celldata->flags, celldata->fg, celldata->bg, celldata->data);
        }
    }
}

static void
dump_history_line(struct cmd_ctx *ctx, struct grid_line *linedata,
                  int *dump_context)
{
    unsigned int   i;
    struct dstring output;
    ds_init(&output);
    for (i = 0; i < linedata->cellsize; i++) {
        dump_history_cell(&output, linedata->celldata + i,
                          linedata->utf8data + i, dump_context);
    }
    if (linedata->flags & GRID_LINE_WRAPPED) {
        ds_appendf(&output, "+");
    }
    ctx->print(ctx, "%s", output.buffer);
    ds_free(&output);
}

int
cmd_dump_history_exec(struct cmd *self, struct cmd_ctx *ctx)
{
    struct args         *args = self->args;
    struct window_pane  *wp;
    struct session      *s;
    const char          *max_lines_str;
    unsigned             max_lines;
    int                  temp;
    unsigned int         i;
    unsigned int         start, limit;
    struct grid         *grid;
    int dump_context[DUMP_HISTORY_CONTEXT_SIZE] = { -1, -1, -1, -1 };

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
        if (!grid) {
            return (0);
        }
    } else {
        grid = wp->base.grid;
    }
    limit = grid->hsize + grid->sy;
    if (limit >= max_lines) {
        start = limit - max_lines;
    } else {
        start = 0;
    }
    for (i = start; i < limit; i++) {
        dump_history_line(ctx, grid->linedata + i, dump_context);
    }

    return (0);
}
