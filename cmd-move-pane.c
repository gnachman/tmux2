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
#include <unistd.h>

#include "tmux.h"

/*
 * Move a pane by splitting another pane and moving it into the new split. Just
 * like join-pane but without the different-windows requirement.
 */

void	cmd_move_pane_key_binding(struct cmd *, int);
int	cmd_move_pane_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_move_pane_entry = {
	"move-pane", "movep",
	"bdhvp:l:s:t:", 0, 0,
	"[-bdhv] [-p percentage|-l size] [-s src-pane] [-t dst-pane]",
	0,
	NULL,
	NULL,
	cmd_move_pane_exec
};

int
cmd_move_pane_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	return join_pane(self, ctx, 0);
}
