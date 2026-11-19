#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "90s.h"
#include "alias.h"
#include "commands.h"
#include "constants.h"
#include "history.h"
#include "job.h"
#include "parser.h"
#include "runlog.h"

int last_exit_code;

int cd(char **args);
int help(char **args);
int quit(char **args);
int history(char **args);
int export(char **args);
int source(char **args);
int j(char **args);
int bg(char **args);

char *builtin_cmds[] = {
	"cd", "help", "exit",	"history",	"export", "source",
	"j",  "bg",	  "runlog", "histgrep", "cdhist",
};

int (*builtin_func[])(char **) = {
	&cd, &help, &quit,	 &history,	&export, &source,
	&j,	 &bg,	&runlog, &histgrep, &cdhist,
};

char *shortcut_dirs[] = {
	"90s",
	"bin",
	"localbin",
};

char *shortcut_expand_dirs[] = {
	"~/.nky/git/90s",
	"~/.local/bin",
	"/usr/local/bin",
};

char *gethome(void)
{
	char *home = getenv("HOME");

	if (home == NULL) {
		fprintf(stderr, "Error: HOME environment variable not set.\n");
		exit(EXIT_FAILURE);
	}
	return home;
}

int num_builtins(void) { return sizeof(builtin_cmds) / sizeof(char *); }

bool is_builtin(char *command)
{
	for (int i = 0; i < num_builtins(); i++) {
		if (strcmp(command, builtin_cmds[i]) == 0)
			return true;
	}
	return false;
}

/*
 *autojump
 */
int j(char **args)
{
	if (args[1] == NULL) {
		fprintf(stderr, "90s: not enough arguments\n");
		return -1;
	}
	for (size_t i = 0; i < sizeof(shortcut_dirs) / sizeof(char *); i++) {
		if (strncmp(args[1], shortcut_dirs[i], strlen(shortcut_dirs[i])) != 0)
			continue;
		char path[PATH_MAX];
		const char *dir = shortcut_expand_dirs[i];
		if (dir[0] == '~')
			snprintf(path, sizeof(path), "%s%s", gethome(), dir + 1);
		else
			snprintf(path, sizeof(path), "%s", dir);
		char *cd_args[] = {"cd", path, NULL};
		cd(cd_args);
		printf("jumped to %s\n", path);
		return 1;
	}
	return 1;
}

int cd(char **args)
{
	const char *target = args[1] ? args[1] : gethome();
	char cwd[PATH_MAX];

	if (chdir(target) != 0) {
		perror("90s");
		return 1;
	}
	if (getcwd(cwd, sizeof(cwd)))
		cdhist_append(cwd);
	return 1;
}

/*
 * Show help menu
 */
int help(char **args)
{
	(void)args;
	printf("90s %s\n", VERSION);
	printf("Built in commands:\n");
	for (int i = 0; i < num_builtins(); i++)
		printf("  %s\n", builtin_cmds[i]);
	printf("Use 'man' to read manual of programs\n");
	printf("Licensed under GPL v3\n");
	return 1;
}

int quit(char **args)
{
	(void)args;
	/* Exit prompt loop */
	return 0;
}

int history(char **args)
{
	(void)args;
	char **hist = get_all_history(true);

	for (int i = 0; hist[i] != NULL; ++i) {
		printf("%s\n", hist[i]);
		free(hist[i]);
	}
	free(hist);
	return 1;
}

int export(char **args)
{
	args++;
	while (*args != NULL) {
		char *variable = strtok(*args, "=\n");
		char *value = strtok(NULL, "=\n");
		if (variable != NULL && value != NULL) {
			if (setenv(variable, value, 1) != 0) {
				fprintf(stderr, "90s: Error setting environment variable\n");
				return -1;
			}
		} else {
			fprintf(stderr, "90s: Syntax error when setting environment "
							"variable\nUse \"export VARIABLE=VALUE\"\n");
			return -1;
		}
		args++;
	}
	return 1;
}

int source(char **args)
{
	if (args[1] == NULL) {
		fprintf(stderr, "90s: not enough arguments\n");
		return -1;
	}

	FILE *file = fopen(args[1], "r");
	if (file == NULL) {
		fprintf(stderr, "90s: no such file or directory '%s'\n", args[1]);
		return -1;
	}

	char line[RL_BUFSIZE];
	int status = 1;
	while (status && fgets(line, sizeof(line), file) != NULL) {
		// Remove newline character if present
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		status = execute_line(line);
	}

	fclose(file);
	return status; // Indicate success
}

int bg(char **args)
{
	if (args[1] == NULL) {
		fprintf(stderr, "90s: not enough arguments\n");
		return -1;
	}
	int job_index = atoi(args[1]);
	if (job_index == 0) {
		fprintf(stderr, "90s: invalid job index\n");
		return -1;
	}
	job *search = get_job(job_index - 1);
	if (search == NULL) {
		fprintf(stderr, "90s: no such job\n");
		return -1;
	}
	printf("Job %i: %s\n", job_index, search->command);
	return 1;
}

static int run_builtin(char **args)
{
	for (int i = 0; i < num_builtins(); i++) {
		if (strcmp(args[0], builtin_cmds[i]) == 0)
			return builtin_func[i](args);
	}
	return 1;
}

static int redirect_fd(const char *path, int flags, int target)
{
	int fd = open(path, flags, 0666);

	if (fd == -1) {
		fprintf(stderr, "90s: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fd != target) {
		dup2(fd, target);
		close(fd);
	}
	return 0;
}

static int apply_redirects(const command_t *c)
{
	if (c->in_file && redirect_fd(c->in_file, O_RDONLY, STDIN_FILENO) == -1)
		return -1;
	if (c->out_file &&
		redirect_fd(c->out_file,
					O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC),
					STDOUT_FILENO) == -1)
		return -1;
	if (c->err_file &&
		redirect_fd(c->err_file,
					O_WRONLY | O_CREAT | (c->err_append ? O_APPEND : O_TRUNC),
					STDERR_FILENO) == -1)
		return -1;
	if (c->err_to_out)
		dup2(STDOUT_FILENO, STDERR_FILENO);
	return 0;
}

static char **colorize(char **argv, int argc)
{
	static char *names[] = {"ls", "diff", "grep"};
	bool match = false;

	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
		if (strcmp(argv[0], names[i]) == 0)
			match = true;
	}
	if (!match)
		return argv;

	char **out = memalloc(sizeof(char *) * (argc + 2));
	out[0] = argv[0];
	out[1] = "--color=auto";
	memcpy(&out[2], &argv[1], sizeof(char *) * argc);
	return out;
}

static void exec_command(command_t *c)
{
	if (apply_redirects(c) == -1)
		_exit(1);

	if (is_builtin(c->argv[0])) {
		int rc = run_builtin(c->argv);
		fflush(stdout);
		_exit(rc < 0 ? 1 : 0);
	}

	char **argv = colorize(c->argv, c->argc);
	execvp(argv[0], argv);
	if (errno == ENOENT)
		fprintf(stderr, "90s: command not found: %s\n", argv[0]);
	else
		fprintf(stderr, "90s: %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

int run_pipeline(pipeline_t *pl)
{
	int n = pl->count;

	if (n == 0)
		return 1;

	command_t *first = &pl->cmds[0];
	if (n == 1 && !pl->background && !command_has_redirect(first) &&
		is_builtin(first->argv[0])) {
		int rc = run_builtin(first->argv);

		last_exit_code = rc < 0 ? 1 : 0;
		return rc;
	}

	pid_t pids[n];
	int started = 0;
	int in = STDIN_FILENO;
	int status = 0;

	last_exit_code = 1;

	fflush(NULL);
	change_terminal_attribute(0);

	for (int i = 0; i < n; i++) {
		int fds[2] = {-1, -1};

		if (i < n - 1 && pipe(fds) == -1) {
			perror("90s");
			break;
		}

		pid_t pid = fork();
		if (pid < 0) {
			perror("90s");
			if (fds[0] != -1) {
				close(fds[0]);
				close(fds[1]);
			}
			break;
		}
		if (pid == 0) {
			if (in != STDIN_FILENO) {
				dup2(in, STDIN_FILENO);
				close(in);
			}
			if (fds[1] != -1) {
				dup2(fds[1], STDOUT_FILENO);
				close(fds[0]);
				close(fds[1]);
			}
			exec_command(&pl->cmds[i]);
		}

		pids[started++] = pid;
		if (in != STDIN_FILENO)
			close(in);
		if (fds[1] != -1) {
			close(fds[1]);
			in = fds[0];
		}
	}
	if (in != STDIN_FILENO)
		close(in);

	if (pl->background && started > 0) {
		char *name = pl->cmds[0].argv[0];
		int job_index = add_job(pids[started - 1], name, true);
		printf("[Job: %i] [Process ID: %i] [Command: %s]\n", job_index + 1,
			   pids[started - 1], name);
		last_exit_code = 0;
	} else {
		for (int i = 0; i < started; i++) {
			int st;

			waitpid(pids[i], &st, 0);
			if (i == started - 1)
				status = st;
		}
		if (started > 0)
			last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
											   : 128 + WTERMSIG(status);
	}

	change_terminal_attribute(1);
	return 1;
}

static char *expand_aliases(const char *line)
{
	char *out = memalloc(1);
	size_t out_len = 0;
	const char *seg = line;
	bool in_single = false, in_double = false;

	out[0] = '\0';
	for (const char *p = line;; p++) {
		if (*p == '\\' && !in_single && p[1]) {
			p++;
			continue;
		}
		if (*p == '\'' && !in_double)
			in_single = !in_single;
		else if (*p == '"' && !in_single)
			in_double = !in_double;
		if (*p != '\0' && !(*p == '|' && !in_single && !in_double))
			continue;

		size_t seg_len = p - seg;
		char *piece = memalloc(seg_len + 1);
		memcpy(piece, seg, seg_len);
		piece[seg_len] = '\0';
		char *expanded = expand_segment(piece);
		free(piece);

		size_t add = strlen(expanded);
		out = memrealloc(out, out_len + add + 2);
		memcpy(out + out_len, expanded, add);
		out_len += add;
		free(expanded);
		if (*p == '|')
			out[out_len++] = '|';
		out[out_len] = '\0';

		if (*p == '\0')
			break;
		seg = p + 1;
	}
	return out;
}

int execute_line(const char *line)
{
	pipeline_t pl;
	char *expanded = expand_aliases(line);
	int status = 1;

	if (parse_line(expanded, &pl) == 0) {
		status = run_pipeline(&pl);
		free_pipeline(&pl);
	} else {
		last_exit_code = 2;
	}
	free(expanded);
	return status;
}
