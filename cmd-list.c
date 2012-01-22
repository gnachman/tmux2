/* $Id$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <string.h>

#include "tmux.h"

struct cmd_list *
cmd_list_parse(int argc, char **argv, char **cause)
{
	struct cmd_list	*cmdlist;
	struct cmd	*cmd;
	int		 i, lastsplit;
	size_t		 arglen, new_argc;
	char	       **copy_argv, **new_argv;

	copy_argv = cmd_copy_argv(argc, argv);

	cmdlist = xmalloc(sizeof *cmdlist);
	cmdlist->references = 1;
	TAILQ_INIT(&cmdlist->list);

	lastsplit = 0;
	for (i = 0; i < argc; i++) {
		arglen = strlen(copy_argv[i]);
		if (arglen == 0 || copy_argv[i][arglen - 1] != ';')
			continue;
		copy_argv[i][arglen - 1] = '\0';

		if (arglen > 1 && copy_argv[i][arglen - 2] == '\\') {
			copy_argv[i][arglen - 2] = ';';
			continue;
		}

		new_argc = i - lastsplit;
		new_argv = copy_argv + lastsplit;
		if (arglen != 1)
			new_argc++;

		cmd = cmd_parse(new_argc, new_argv, cause);
		if (cmd == NULL)
			goto bad;
		TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);

		lastsplit = i + 1;
	}

	if (lastsplit != argc) {
		cmd = cmd_parse(argc - lastsplit, copy_argv + lastsplit, cause);
		if (cmd == NULL)
			goto bad;
		TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);
	}

	cmd_free_argv(argc, copy_argv);
	return (cmdlist);

bad:
	cmd_list_free(cmdlist);
	cmd_free_argv(argc, copy_argv);
	return (NULL);
}

int
cmd_list_exec(struct cmd_list *cmdlist, struct cmd_ctx *ctx)
{
	struct cmd	*cmd;
	int		 n, retval;
	struct client	*c;
	int		 print_guards;

	c = ctx->curclient;
	/* print %begin...%end guards around command output only if the client
	 * is a control client that has an attached session. The requirement
	 * for an attached session exists because the local client may issue an
	 * attach-session or new-session command on startup that the remote
	 * client is unaware of. Only after attaching to a session does the
	 * remote client take charge.*/
	print_guards = c && (c->flags & CLIENT_CONTROL) && c->session;
	retval = 0;
	control_set_spontaneous_messages_allowed(0);
	TAILQ_FOREACH(cmd, &cmdlist->list, qentry) {
		if (print_guards)
			ctx->print(ctx, "%%begin");
		if ((n = cmd_exec(cmd, ctx)) == -1) {
			if (print_guards)
				ctx->print(ctx, "%%end");
			return (-1);
		}
		if (print_guards)
			ctx->print(ctx, "%%end");

		/*
		 * A 1 return value means the command client is being attached
		 * (sent MSG_READY).
		 */
		if (n == 1) {
			retval = 1;

			/*
			 * The command client has been attached, so mangle the
			 * context to treat any following commands as if they
			 * were called from inside.
			 */
			if (ctx->curclient == NULL) {
				ctx->curclient = ctx->cmdclient;
				ctx->cmdclient = NULL;

				ctx->error = key_bindings_error;
				ctx->print = key_bindings_print;
				ctx->info = key_bindings_info;
			}
		}
	}
	control_set_spontaneous_messages_allowed(1);
	return (retval);
}

void
cmd_list_free(struct cmd_list *cmdlist)
{
	struct cmd	*cmd;

	if (--cmdlist->references != 0)
		return;

	while (!TAILQ_EMPTY(&cmdlist->list)) {
		cmd = TAILQ_FIRST(&cmdlist->list);
		TAILQ_REMOVE(&cmdlist->list, cmd, qentry);
		cmd_free(cmd);
	}
	xfree(cmdlist);
}

size_t
cmd_list_print(struct cmd_list *cmdlist, char *buf, size_t len)
{
	struct cmd	*cmd;
	size_t		 off;

	off = 0;
	TAILQ_FOREACH(cmd, &cmdlist->list, qentry) {
		if (off >= len)
			break;
		off += cmd_print(cmd, buf + off, len - off);
		if (off >= len)
			break;
		if (TAILQ_NEXT(cmd, qentry) != NULL)
			off += xsnprintf(buf + off, len - off, " ; ");
	}
	return (off);
}
