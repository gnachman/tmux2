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

/*
 * Example usage:
 *
 * struct dstring mystring;
 * ds_init(&mystring);
 * ds_appendf(&mystring, "Hello %s", name);
 * ds_append(&mystring, "\n");
 * printf("The string value is: %s", mystring.buffer);
 * ds_free(&mystring);
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

void
ds_init(struct dstring *ds)
{
	ds->buffer = ds->staticbuffer;
	ds->used = 0;
	ds->available = DSTRING_STATIC_BUFFER_SIZE;
	ds->buffer[0] = '\0';
}

void
ds_free(struct dstring *ds)
{
	if (ds->buffer != ds->staticbuffer) {
		xfree(ds->buffer);
	}
}

void
ds_appendf(struct dstring *ds, const char *fmt, ...)
{
	char	*temp;
	va_list	 ap;

	va_start(ap, fmt);
	xvasprintf(&temp, fmt, ap);
	va_end(ap);

	ds_append(ds, temp);
	xfree(temp);
}

void
ds_append(struct dstring *ds, const char *str)
{
	int	len;

	len = strlen(str);
	ds_appendl(ds, str, len);
}

void
ds_appendl(struct dstring *ds, const char *str, int len)
{
	if (ds->used + len >= ds->available) {
		ds->available *= 2;
		if (ds->buffer == ds->staticbuffer) {
			ds->buffer = xmalloc(ds->available);
			memmove(ds->buffer, ds->staticbuffer, ds->used);
		} else {
			ds->buffer = xrealloc(ds->buffer, ds->available, 1);
		}
	}
	memmove(ds->buffer + ds->used, str, len);
	ds->used += len;
	ds->buffer[ds->used] = '\0';
}

void
ds_truncate(struct dstring *ds, int new_length)
{
	ds->used = new_length;
	ds->buffer[new_length] = '\0';

	/* We're a little conservative about freeing memory to avoid repeated
	 * realloc calls at the cost of using a bit more memory. We'll also
	 * never realloc below the static buffer's size because the cost of
	 * fragmentation exceeds the benefit of saving a few bytes. */
	if (ds->buffer != ds->staticbuffer &&
	    new_length > (DSTRING_STATIC_BUFFER_SIZE / 2) &&
	    new_length < ds->available / 4) {
		ds->buffer = xrealloc(ds->buffer, new_length * 2, 1);
	}
}
