/* $Id$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
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
#include <sys/stat.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#if defined(DEBUG) && defined(__OpenBSD__)
extern char	*malloc_options;
#endif

struct options	 global_options;	/* server options */
struct options	 global_s_options;	/* session options */
struct options	 global_w_options;	/* window options */
struct environ	 global_environ;

struct event_base *ev_base;

char		*cfg_file;
char		*shell_cmd;
int		 debug_level;
time_t		 start_time;
char		 socket_path[MAXPATHLEN];
int		 login_shell;
char		*environ_path;
pid_t		 environ_pid = -1;
int		 environ_idx = -1;

__dead void	 usage(void);
void	 	 parseenvironment(void);
char 		*makesocketpath(const char *);

#ifndef HAVE___PROGNAME
char      *__progname = (char *) "tmux";
#endif

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-28lquvV] [-c shell-command] [-f file] [-L socket-name]\n"
	    "            [-S socket-path] [command [flags]]\n",
	    __progname);
	exit(1);
}

void
logfile(const char *name)
{
	char	*path;

	if (debug_level > 0) {
		xasprintf(&path, "tmux-%s-%ld.log", name, (long) getpid());
		log_open(debug_level, path);
		free(path);
	}
}

const char *
getshell(void)
{
	struct passwd	*pw;
	const char	*shell;

	shell = getenv("SHELL");
	if (checkshell(shell))
		return (shell);

	pw = getpwuid(getuid());
	if (pw != NULL && checkshell(pw->pw_shell))
		return (pw->pw_shell);

	return (_PATH_BSHELL);
}

int
checkshell(const char *shell)
{
	if (shell == NULL || *shell == '\0' || *shell != '/')
		return (0);
	if (areshell(shell))
		return (0);
	if (access(shell, X_OK) != 0)
		return (0);
	return (1);
}

int
areshell(const char *shell)
{
	const char	*progname, *ptr;

	if ((ptr = strrchr(shell, '/')) != NULL)
		ptr++;
	else
		ptr = shell;
	progname = __progname;
	if (*progname == '-')
		progname++;
	if (strcmp(ptr, progname) == 0)
		return (1);
	return (0);
}

const char*
get_full_path(const char *wd, const char *path)
{
	static char	newpath[MAXPATHLEN];
	char		oldpath[MAXPATHLEN];

	if (getcwd(oldpath, sizeof oldpath) == NULL)
		return (NULL);
	if (chdir(wd) != 0)
		return (NULL);
	if (realpath(path, newpath) != 0)
		return (NULL);
	chdir(oldpath);
	return (newpath);
}

void
parseenvironment(void)
{
	char	*env, path[256];
	long	 pid;
	int	 idx;

	if ((env = getenv("TMUX")) == NULL)
		return;

	if (sscanf(env, "%255[^,],%ld,%d", path, &pid, &idx) != 3)
		return;
	environ_path = xstrdup(path);
	environ_pid = pid;
	environ_idx = idx;
}

char *
makesocketpath(const char *label)
{
	char		base[MAXPATHLEN], *path, *s;
	struct stat	sb;
	u_int		uid;

	uid = getuid();
	if ((s = getenv("TMPDIR")) == NULL || *s == '\0')
		xsnprintf(base, sizeof base, "%s/tmux-%u", _PATH_TMP, uid);
	else
		xsnprintf(base, sizeof base, "%s/tmux-%u", s, uid);

	if (mkdir(base, S_IRWXU) != 0 && errno != EEXIST)
		return (NULL);

	if (lstat(base, &sb) != 0)
		return (NULL);
	if (!S_ISDIR(sb.st_mode)) {
		errno = ENOTDIR;
		return (NULL);
	}
	if (sb.st_uid != uid || (sb.st_mode & (S_IRWXG|S_IRWXO)) != 0) {
		errno = EACCES;
		return (NULL);
	}

	xasprintf(&path, "%s/%s", base, label);
	return (path);
}

void
setblocking(int fd, int state)
{
	int mode;

	if ((mode = fcntl(fd, F_GETFL)) != -1) {
		if (!state)
			mode |= O_NONBLOCK;
		else
			mode &= ~O_NONBLOCK;
		fcntl(fd, F_SETFL, mode);
	}
}

__dead void
shell_exec(const char *shell, const char *shellcmd)
{
	const char	*shellname, *ptr;
	char		*argv0;

	ptr = strrchr(shell, '/');
	if (ptr != NULL && *(ptr + 1) != '\0')
		shellname = ptr + 1;
	else
		shellname = shell;
	if (login_shell)
		xasprintf(&argv0, "-%s", shellname);
	else
		xasprintf(&argv0, "%s", shellname);
	setenv("SHELL", shell, 1);

	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);
	closefrom(STDERR_FILENO + 1);

	execl(shell, argv0, "-c", shellcmd, (char *) NULL);
	fatal("execl failed");
}

int
main(int argc, char **argv)
{
	struct passwd	*pw;
	char		*s, *path, *label, *home, **var;
	int	 	 opt, flags, quiet, keys;

#if defined(DEBUG) && defined(__OpenBSD__)
	malloc_options = (char *) "AFGJPX";
#endif

	quiet = flags = 0;
	label = path = NULL;
	login_shell = (**argv == '-');
	while ((opt = getopt(argc, argv, "28c:Cdf:lL:qS:uUvV")) != -1) {
		switch (opt) {
		case '2':
			flags |= IDENTIFY_256COLOURS;
			flags &= ~IDENTIFY_88COLOURS;
			break;
		case '8':
			flags |= IDENTIFY_88COLOURS;
			flags &= ~IDENTIFY_256COLOURS;
			break;
		case 'c':
			free(shell_cmd);
			shell_cmd = xstrdup(optarg);
			break;
		case 'C':
			if (flags & IDENTIFY_CONTROL)
				flags |= IDENTIFY_TERMIOS;
			else
				flags |= IDENTIFY_CONTROL;
			break;
		case 'V':
			printf("%s %s\n", __progname, VERSION);
			exit(0);
		case 'f':
			free(cfg_file);
			cfg_file = xstrdup(optarg);
			break;
		case 'l':
			login_shell = 1;
			break;
		case 'L':
			free(label);
			label = xstrdup(optarg);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'S':
			free(path);
			path = xstrdup(optarg);
			break;
		case 'u':
			flags |= IDENTIFY_UTF8;
			break;
		case 'v':
			debug_level++;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (shell_cmd != NULL && argc != 0)
		usage();

	if (!(flags & IDENTIFY_UTF8)) {
		/*
		 * If the user has set whichever of LC_ALL, LC_CTYPE or LANG
		 * exist (in that order) to contain UTF-8, it is a safe
		 * assumption that either they are using a UTF-8 terminal, or
		 * if not they know that output from UTF-8-capable programs may
		 * be wrong.
		 */
		if ((s = getenv("LC_ALL")) == NULL || *s == '\0') {
			if ((s = getenv("LC_CTYPE")) == NULL || *s == '\0')
				s = getenv("LANG");
		}
		if (s != NULL && (strcasestr(s, "UTF-8") != NULL ||
		    strcasestr(s, "UTF8") != NULL))
			flags |= IDENTIFY_UTF8;
	}

	environ_init(&global_environ);
	for (var = environ; *var != NULL; var++)
		environ_put(&global_environ, *var);

	options_init(&global_options, NULL);
	options_table_populate_tree(server_options_table, &global_options);
	options_set_number(&global_options, "quiet", quiet);

	options_init(&global_s_options, NULL);
	options_table_populate_tree(session_options_table, &global_s_options);
	options_set_string(&global_s_options, "default-shell", "%s", getshell());

	options_init(&global_w_options, NULL);
	options_table_populate_tree(window_options_table, &global_w_options);

	/* Enable UTF-8 if the first client is on UTF-8 terminal. */
	if (flags & IDENTIFY_UTF8) {
		options_set_number(&global_s_options, "status-utf8", 1);
		options_set_number(&global_s_options, "mouse-utf8", 1);
		options_set_number(&global_w_options, "utf8", 1);
	}

	/* Override keys to vi if VISUAL or EDITOR are set. */
	if ((s = getenv("VISUAL")) != NULL || (s = getenv("EDITOR")) != NULL) {
		if (strrchr(s, '/') != NULL)
			s = strrchr(s, '/') + 1;
		if (strstr(s, "vi") != NULL)
			keys = MODEKEY_VI;
		else
			keys = MODEKEY_EMACS;
		options_set_number(&global_s_options, "status-keys", keys);
		options_set_number(&global_w_options, "mode-keys", keys);
	}

	/* Locate the configuration file. */
	if (cfg_file == NULL) {
		home = getenv("HOME");
		if (home == NULL || *home == '\0') {
			pw = getpwuid(getuid());
			if (pw != NULL)
				home = pw->pw_dir;
		}
		xasprintf(&cfg_file, "%s/%s", home, DEFAULT_CFG);
		if (access(cfg_file, R_OK) != 0 && errno == ENOENT) {
			free(cfg_file);
			cfg_file = NULL;
		}
	}

	/*
	 * Figure out the socket path. If specified on the command-line with -S
	 * or -L, use it, otherwise try $TMUX or assume -L default.
	 */
	parseenvironment();
	if (path == NULL) {
		/* If no -L, use the environment. */
		if (label == NULL) {
			if (environ_path != NULL)
				path = xstrdup(environ_path);
			else
				label = xstrdup("default");
		}

		/* -L or default set. */
		if (label != NULL) {
			if ((path = makesocketpath(label)) == NULL) {
				fprintf(stderr, "can't create socket\n");
				exit(1);
			}
		}
	}
	free(label);
	if (realpath(path, socket_path) == NULL)
		strlcpy(socket_path, path, sizeof socket_path);
	free(path);

#ifdef HAVE_SETPROCTITLE
	/* Set process title. */
	setproctitle("%s (%s)", __progname, socket_path);
#endif

	/* Pass control to the client. */
	ev_base = osdep_event_init();
	exit(client_main(argc, argv, flags));
}
