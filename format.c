/* $Id$ */

/*
 * Copyright (c) 2011 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <netdb.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Build a list of key-value pairs and use them to expand #{key} entries in a
 * string.
 */

int	format_replace(struct format_tree *,
	    const char *, size_t, char **, size_t *, size_t *);

/* Format key-value replacement entry. */
RB_GENERATE(format_tree, format_entry, entry, format_cmp);

/* Format tree comparison function. */
int
format_cmp(struct format_entry *fe1, struct format_entry *fe2)
{
	return (strcmp(fe1->key, fe2->key));
}

/* Single-character aliases. */
const char *format_aliases[26] = {
	NULL,		/* A */
	NULL,		/* B */
	NULL,		/* C */
	"pane_id",	/* D */
	NULL,		/* E */
	"window_flags",	/* F */
	NULL,		/* G */
	"host",		/* H */
	"window_index",	/* I */
	NULL,		/* J */
	NULL,		/* K */
	NULL,		/* L */
	NULL,		/* M */
	NULL,		/* N */
	NULL,		/* O */
	"pane_index",	/* P */
	NULL,		/* Q */
	NULL,		/* R */
	"session_name",	/* S */
	"pane_title",	/* T */
	NULL,		/* U */
	NULL,		/* V */
	"window_name",	/* W */
	NULL,		/* X */
	NULL,		/* Y */
	NULL 		/* Z */
};

/* Create a new tree. */
struct format_tree *
format_create(void)
{
	struct format_tree	*ft;
	char			 host[MAXHOSTNAMELEN];

	ft = xmalloc(sizeof *ft);
	RB_INIT(ft);

	if (gethostname(host, sizeof host) == 0)
		format_add(ft, "host", "%s", host);

	return (ft);
}

/* Free a tree. */
void
format_free(struct format_tree *ft)
{
	struct format_entry	*fe, *fe_next;

	fe_next = RB_MIN(format_tree, ft);
	while (fe_next != NULL) {
		fe = fe_next;
		fe_next = RB_NEXT(format_tree, ft, fe);

		RB_REMOVE(format_tree, ft, fe);
		xfree(fe->value);
		xfree(fe->key);
		xfree(fe);
	}

	xfree (ft);
}

/* Add a key-value pair. */
void
format_add(struct format_tree *ft, const char *key, const char *fmt, ...)
{
	struct format_entry	*fe;
	va_list			 ap;

	fe = xmalloc(sizeof *fe);
	fe->key = xstrdup(key);

	va_start(ap, fmt);
	xvasprintf(&fe->value, fmt, ap);
	va_end(ap);

	RB_INSERT(format_tree, ft, fe);
}

/* Find a format entry. */
const char *
format_find(struct format_tree *ft, const char *key)
{
	struct format_entry	*fe, fe_find;

	fe_find.key = (char *) key;
	fe = RB_FIND(format_tree, ft, &fe_find);
	if (fe == NULL)
		return (NULL);
	return (fe->value);
}

/*
 * Replace a key/value pair in buffer. #{blah} is expanded directly,
 * #{?blah,a,b} is replace with a if blah exists and is nonzero else b.
 */
int
format_replace(struct format_tree *ft,
    const char *key, size_t keylen, char **buf, size_t *len, size_t *off)
{
	char		*copy, *ptr;
	const char	*value;
	size_t		 valuelen;

	/* Make a copy of the key. */
	copy = xmalloc(keylen + 1);
	memcpy(copy, key, keylen);
	copy[keylen] = '\0';

	/*
	 * Is this a conditional? If so, check it exists and extract either the
	 * first or second element. If not, look up the key directly.
	 */
	if (*copy == '?') {
		ptr = strchr(copy, ',');
		if (ptr == NULL)
			goto fail;
		*ptr = '\0';

		value = format_find(ft, copy + 1);
		if (value != NULL && (value[0] != '0' || value[1] != '\0')) {
			value = ptr + 1;
			ptr = strchr(value, ',');
			if (ptr == NULL)
				goto fail;
			*ptr = '\0';
		} else {
			ptr = strchr(ptr + 1, ',');
			if (ptr == NULL)
				goto fail;
			value = ptr + 1;
		}
	} else {
		value = format_find(ft, copy);
		if (value == NULL)
			value = "";
	}
	valuelen = strlen(value);

	/* Expand the buffer and copy in the value. */
	while (*len - *off < valuelen + 1) {
		*buf = xrealloc(*buf, 2, *len);
		*len *= 2;
	}
	memcpy(*buf + *off, value, valuelen);
	*off += valuelen;

	xfree(copy);
	return (0);

fail:
	xfree(copy);
	return (-1);
}

/* Expand keys in a template. */
char *
format_expand(struct format_tree *ft, const char *fmt)
{
	char		*buf, *ptr;
	const char	*s;
	size_t		 off, len, n;
	int     	 ch;

	len = 64;
	buf = xmalloc(len);
	off = 0;

	while (*fmt != '\0') {
		if (*fmt != '#') {
			while (len - off < 2) {
				buf = xrealloc(buf, 2, len);
				len *= 2;
			}
			buf[off++] = *fmt++;
			continue;
		}
		fmt++;

		ch = (u_char) *fmt++;
		switch (ch) {
		case '{':
			ptr = strchr(fmt, '}');
			if (ptr == NULL)
				break;
			n = ptr - fmt;

			if (format_replace(ft, fmt, n, &buf, &len, &off) != 0)
				break;
			fmt += n + 1;
			continue;
		default:
			if (ch >= 'A' && ch <= 'Z') {
				s = format_aliases[ch - 'A'];
				if (s != NULL) {
					n = strlen(s);
					if (format_replace (
					    ft, s, n, &buf, &len, &off) != 0)
						break;
					continue;
				}
			}
			while (len - off < 2) {
				buf = xrealloc(buf, 2, len);
				len *= 2;
			}
			buf[off++] = ch;
			continue;
		}

		break;
	}
	buf[off] = '\0';

	return (buf);
}

/* Set default format keys for a session. */
void
format_session(struct format_tree *ft, struct session *s)
{
	struct session_group	*sg;
	char			*tim;
	time_t			 t;

	format_add(ft, "session_name", "%s", s->name);
	format_add(ft, "session_windows", "%u", winlink_count(&s->windows));
	format_add(ft, "session_width", "%u", s->sx);
	format_add(ft, "session_height", "%u", s->sy);

	sg = session_group_find(s);
	format_add(ft, "session_grouped", "%d", sg != NULL);
	if (sg != NULL)
		format_add(ft, "session_group", "%u", session_group_index(sg));

	t = s->creation_time.tv_sec;
	format_add(ft, "session_created", "%ld", (long) t);
	tim = ctime(&t);
	*strchr(tim, '\n') = '\0';
	format_add(ft, "session_created_string", "%s", tim);

	if (s->flags & SESSION_UNATTACHED)
		format_add(ft, "session_attached", "%d", 0);
	else
		format_add(ft, "session_attached", "%d", 1);
}

/* Set default format keys for a client. */
void
format_client(struct format_tree *ft, struct client *c)
{
	char	*tim;
	time_t	 t;

	format_add(ft, "client_cwd", "%s", c->cwd);
	format_add(ft, "client_height", "%u", c->tty.sy);
	format_add(ft, "client_width", "%u", c->tty.sx);
	format_add(ft, "client_tty", "%s", c->tty.path);
	format_add(ft, "client_termname", "%s", c->tty.termname);

	t = c->creation_time.tv_sec;
	format_add(ft, "client_created", "%ld", (long) t);
	tim = ctime(&t);
	*strchr(tim, '\n') = '\0';
	format_add(ft, "client_created_string", "%s", tim);

	t = c->activity_time.tv_sec;
	format_add(ft, "client_activity", "%ld", (long) t);
	tim = ctime(&t);
	*strchr(tim, '\n') = '\0';
	format_add(ft, "client_activity_string", "%s", tim);

	if (c->tty.flags & TTY_UTF8)
		format_add(ft, "client_utf8", "%d", 1);
	else
		format_add(ft, "client_utf8", "%d", 0);

	if (c->flags & CLIENT_READONLY)
		format_add(ft, "client_readonly", "%d", 1);
	else
		format_add(ft, "client_readonly", "%d", 0);
}

/* Set default format keys for a winlink. */
void
format_winlink(struct format_tree *ft, struct session *s, struct winlink *wl)
{
	struct window	*w = wl->window;
	char		*layout, *flags;

	layout = layout_dump(w);
	flags = window_printable_flags(s, wl);

	format_add(ft, "window_id", "@%u", w->id);
	format_add(ft, "window_index", "%d", wl->idx);
	format_add(ft, "window_name", "%s", w->name);
	format_add(ft, "window_width", "%u", w->sx);
	format_add(ft, "window_height", "%u", w->sy);
	format_add(ft, "window_flags", "%s", flags);
	format_add(ft, "window_layout", "%s", layout);
	format_add(ft, "window_active", "%d", wl == s->curw);
	format_add(ft, "window_panes", "%u", window_count_panes(w));

	xfree(flags);
	xfree(layout);
}

/* Set default format keys for a window pane. */
void
format_window_pane(struct format_tree *ft, struct window_pane *wp)
{
	struct grid		*gd = wp->base.grid;
	struct grid_line	*gl;
	unsigned long long	 size;
	u_int			 i;
	u_int			 idx;

	size = 0;
	for (i = 0; i < gd->hsize; i++) {
		gl = &gd->linedata[i];
		size += gl->cellsize * sizeof *gl->celldata;
		size += gl->utf8size * sizeof *gl->utf8data;
	}
	size += gd->hsize * sizeof *gd->linedata;

	if (window_pane_index(wp, &idx) != 0)
		fatalx("index not found");

	format_add(ft, "pane_width", "%u", wp->sx);
	format_add(ft, "pane_height", "%u", wp->sy);
	format_add(ft, "pane_title", "%s", wp->base.title);
	format_add(ft, "pane_index", "%u", idx);
	format_add(ft, "history_size", "%u", gd->hsize);
	format_add(ft, "history_limit", "%u", gd->hlimit);
	format_add(ft, "history_bytes", "%llu", size);
	format_add(ft, "pane_id", "%%%u", wp->id);
	format_add(ft, "pane_active", "%d", wp == wp->window->active);
	format_add(ft, "pane_dead", "%d", wp->fd == -1);
	if (wp->cmd != NULL)
		format_add(ft, "pane_start_command", "%s", wp->cmd);
	if (wp->cwd != NULL)
		format_add(ft, "pane_start_path", "%s", wp->cwd);
	format_add(ft, "pane_current_path", "%s", osdep_get_cwd(wp->pid));
	format_add(ft, "pane_pid", "%ld", (long) wp->pid);
	format_add(ft, "pane_tty", "%s", wp->tty);
}

void
format_paste_buffer(struct format_tree *ft, struct paste_buffer *pb)
{
	char	*pb_print = paste_print(pb, 50);

	format_add(ft, "buffer_size", "%zu", pb->size);
	format_add(ft, "buffer_sample", "%s", pb_print);

	xfree(pb_print);
}
