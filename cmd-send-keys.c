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

/*
 * Send keys to client.
 */

int cmd_send_keys_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_send_keys_entry = {
    "send-keys", "send",
    "6t:", 0, -1,
    "[-t target-pane] key ...",
    0,
    NULL,
    NULL,
    cmd_send_keys_exec
};

static int
hextoint(char hex)
{
    if (hex >= '0' && hex <= '9') {
        return hex - '0';
    }
    if (hex >= 'a' && hex <= 'f') {
        return hex - 'a' + 10;
    }
    if (hex >= 'A' && hex <= 'F') {
        return hex - 'A' + 10;
    }
    return 0;
}

static int
hexdecode(char *hex)
{
    return hextoint(hex[0]) * 16 + hextoint(hex[1]);
}

int
cmd_send_keys_exec(struct cmd *self, struct cmd_ctx *ctx)
{
    struct args     *args = self->args;
    struct window_pane  *wp;
    struct session      *s;
    const char      *str;
    int          i, key;
    int          b64;

    b64 = args_has(args, '6');  /* Key is formatted base base-64 data */
    if (cmd_find_pane(ctx, args_get(args, 't'), &s, &wp) == NULL)
        return (-1);

    for (i = 0; i < args->argc; i++) {
        str = args->argv[i];

        if (b64) {
	    unsigned char *bytes = base64_xdecode(args->argv[i],
						  strlen(args->argv[i]),
						  NULL);
	    if (bytes) {
		for (int j = 0; bytes[j]; j++)
		    window_pane_key(wp, s, bytes[j]);
		xfree(bytes);
	    }
        } else if ((key = key_string_lookup_string(str)) != KEYC_NONE) {
                window_pane_key(wp, s, key);
        } else {
            for (; *str != '\0'; str++)
                window_pane_key(wp, s, *str);
        }
    }

    return (0);
}
