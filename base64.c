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

#include <resolv.h>
#include <string.h>
#include "tmux.h"

unsigned char *base64_xencode(const unsigned char *src, size_t len,
                  size_t *out_len)
{
    size_t targetsize = 4 * ((len + 2) / 3) + 1;
    unsigned char *target = xmalloc(targetsize + 1);
    int rc = b64_ntop(src, len, (char *)target, targetsize);
    if (rc < 0) {
        xfree(target);
        if (out_len)
            *out_len = 0;
        return 0;
    } else {
        if (out_len)
            *out_len = rc;
        return target;
    }
}


unsigned char *base64_xdecode(const unsigned char *src, size_t *out_len)
{
    size_t target_size = b64_pton(src, 0, 0);
    unsigned char *target = xmalloc(target_size + 1);
    int rc = b64_pton(src, target, target_size);
    if (rc < 0) {
        xfree(target);
        if (out_len)
            *out_len = 0;
        return 0;
    } else {
        if (out_len)
            *out_len = strlen(target);
        return target;
    }
}
