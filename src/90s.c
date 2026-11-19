#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "alias.h"
#include "commands.h"
#include "constants.h"
#include "git.h"
#include "history.h"
#include "runlog.h"

extern char *builtin_cmds[];
extern int last_exit_code;

void *memalloc(size_t size)
{
	void *ptr = malloc(size);
	if (!ptr) {
		fputs("90s: Error allocating memory\n", stderr);
		exit(EXIT_FAILURE);
	}
	return ptr;
}

void *memrealloc(void *old, size_t size)
{
	void *ptr = realloc(old, size);
	if (!ptr) {
		fputs("90s: Error allocating memory\n", stderr);
		exit(EXIT_FAILURE);
	}
	return ptr;
}

int get_terminal_width(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
		return 80; // fallback
	return ws.ws_col;
}

void change_terminal_attribute(int option)
{
	static struct termios oldt, newt;
	static bool saved;

	if (option) {
		if (!saved) {
			tcgetattr(STDIN_FILENO, &oldt);
			saved = true;
		}
		newt = oldt;
		// Allows getchar without pressing enter
		newt.c_lflag &= ~(ICANON | ECHO);
		// Set settings to stdin
		tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	} else if (saved) {
		// Restore to old settings
		tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	}
}

char **setup_path_variable(void)
{
	char *envpath = getenv("PATH");
	if (envpath == NULL) {
		fprintf(stderr, "90s: PATH environment variable is missing\n");
		exit(EXIT_FAILURE);
	}
	char *path = memalloc(strlen(envpath) + 1);
	strcpy(path, envpath);
	int path_count = 2; // adding one to be correct and one for terminator
	for (const char *c = envpath; *c != '\0'; c++) {
		if (*c == ':')
			path_count++;
	}
	char **paths = memalloc(sizeof(char *) * path_count);
	char *token = strtok(path, ":");
	int counter = 0;
	while (token != NULL) {
		paths[counter] = token;
		token = strtok(NULL, ":");
		counter++;
	}
	paths[counter] = NULL;
	return paths;
}

bool find_command(char **paths, char *command)
{
	if (command[0] == '\0')
		return false;
	if (is_builtin(command))
		return true;
	while (*paths != NULL) {
		char current_path[PATH_MAX];
		snprintf(current_path, sizeof(current_path), "%s/%s", *paths, command);
		if (access(current_path, X_OK) == 0)
			return true;
		paths++;
	}
	return false;
}

int prompt_visible_length(const char *str)
{
	int len = 0;
	int in_esc = 0;

	if (!str)
		return 0;

	while (*str) {
		if (*str == '\033') {
			in_esc = 1;
		} else if (in_esc) {
			if ((*str >= 'A' && *str <= 'Z') || (*str >= 'a' && *str <= 'z'))
				in_esc = 0;
		} else {
			len++;
		}
		str++;
	}

	return len;
}

void highlight(const char *buffer, char **paths)
{
	char *cmd_part = strchr(buffer, ' ');
	size_t cmd_len = cmd_part ? (size_t)(cmd_part - buffer) : strlen(buffer);

	char cmd[cmd_len + 1];
	memcpy(cmd, buffer, cmd_len);
	cmd[cmd_len] = '\0';
	bool valid = find_command(paths, cmd);

	/* green if valid, red if invalid */
	const char *cmd_color = valid ? "\x1b[32m" : "\x1b[31m";

	/* Print the colored command, then switch to white for args */
	printf("%s%s\x1b[37m", cmd_color, cmd);

	/* Walk the rest token by token, underline each one that has existing path
	 */
	const char *p = buffer + cmd_len;
	while (*p) {
		if (*p == ' ' || *p == '\t') {
			putchar(*p);
			p++;
			continue;
		}

		/* Find end of token (next whitespace) */
		const char *start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		int tok_len = (int)(p - start);

		bool exists = false;
		if (tok_len > 0 && tok_len < PATH_MAX) {
			char tok[PATH_MAX];
			memcpy(tok, start, tok_len);
			tok[tok_len] = '\0';

			const char *check_path = tok;
			char expanded[PATH_MAX];

			if (tok[0] == '~') {
				char *home = getenv("HOME");
				if (home) {
					snprintf(expanded, sizeof(expanded), "%s%s", home, tok + 1);
					check_path = expanded;
				}
			}

			if (access(check_path, F_OK) == 0)
				exists = true;
		}

		if (exists)
			printf("\033[1m\033[4m%.*s\033[22m\033[24m", tok_len, start);
		else
			printf("%.*s", tok_len, start);
	}

	printf("\x1b[m");
	fflush(stdout);
}

void render(const char *prompt, const char *buffer, int cursor_pos,
			int *prev_lines_out, char **paths)
{
	int width = get_terminal_width();
	// strip ANSI for counting
	int prompt_visible = prompt_visible_length(prompt);

	// Move cursor up to the first line if we were multiline
	if (*prev_lines_out > 1)
		printf("\033[%dA", *prev_lines_out - 1); // move up
	printf("\r");								 // go to start of line

	// Clear all lines we might have used
	for (int i = 0; i < *prev_lines_out; i++) {
		printf("\033[2K"); // clear entire line
		if (i < *prev_lines_out - 1)
			printf("\n");
	}

	// Move back up to first line
	if (*prev_lines_out > 1)
		printf("\033[%dA", *prev_lines_out - 1);

	printf("\r");

	// Print prompt + highlighted buffer
	printf("%s", prompt);
	highlight(buffer, paths);

	// Calculate where cursor should be
	int total_visible = prompt_visible + cursor_pos;
	int target_line = total_visible / width;
	int target_col = total_visible % width;

	printf("\r");
	if (target_line > 0)
		printf("\033[%dB", target_line); // move down
	if (target_col > 0)
		printf("\033[%dC", target_col); // move right

	fflush(stdout);

	// Update prev_lines for next render
	int new_total = prompt_visible + strlen(buffer);
	*prev_lines_out = new_total / width + (new_total % width ? 1 : 0);
	if (*prev_lines_out < 1)
		*prev_lines_out = 1;
}

static int str_cmp_qsort(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool has_prefix(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

static void free_str_array(char **arr, int count)
{
	for (int i = 0; i < count; i++)
		free(arr[i]);
	free(arr);
}

static void push_match(char ***matches, int *count, int *cap, const char *str)
{
	if (*count >= *cap) {
		*cap *= 2;
		*matches = memrealloc(*matches, sizeof(char *) * *cap);
	}
	char *dup = strdup(str);
	if (!dup) {
		fputs("90s: Error allocating memory\n", stderr);
		exit(EXIT_FAILURE);
	}
	(*matches)[(*count)++] = dup;
}

static void dedup_matches(char **matches, int *count)
{
	if (*count < 2)
		return;
	qsort(matches, *count, sizeof(char *), str_cmp_qsort);
	int out = 1;
	for (int i = 1; i < *count; i++) {
		if (strcmp(matches[i], matches[out - 1]) == 0)
			free(matches[i]);
		else
			matches[out++] = matches[i];
	}
	*count = out;
}

static char **complete_command(char **paths, const char *prefix, int *count)
{
	int cap = 64, n = 0;
	char **matches = memalloc(sizeof(char *) * cap);

	for (int i = 0; i < num_builtins(); i++) {
		if (has_prefix(builtin_cmds[i], prefix))
			push_match(&matches, &n, &cap, builtin_cmds[i]);
	}

	for (char **p = paths; *p != NULL; p++) {
		DIR *dir = opendir(*p);
		if (!dir)
			continue;
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (!has_prefix(entry->d_name, prefix))
				continue;
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", *p, entry->d_name);
			if (access(full, X_OK) != 0)
				continue;
			struct stat st;
			if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			push_match(&matches, &n, &cap, entry->d_name);
		}
		closedir(dir);
	}

	dedup_matches(matches, &n);
	*count = n;
	return matches;
}

static char **complete_path(const char *token, int *count, char *out_dir,
							char *out_base)
{
	const char *slash = strrchr(token, '/');
	char dir[PATH_MAX], base[PATH_MAX];

	if (slash) {
		size_t dir_len = slash - token + 1;
		memcpy(dir, token, dir_len);
		dir[dir_len] = '\0';
		snprintf(base, sizeof(base), "%s", slash + 1);
	} else {
		dir[0] = '\0';
		snprintf(base, sizeof(base), "%s", token);
	}
	strcpy(out_dir, dir);
	strcpy(out_base, base);

	char open_dir[PATH_MAX];
	if (dir[0] == '~') {
		char *home = getenv("HOME");
		snprintf(open_dir, sizeof(open_dir), "%s%s", home ? home : "", dir + 1);
	} else {
		snprintf(open_dir, sizeof(open_dir), "%s", dir[0] ? dir : ".");
	}

	DIR *d = opendir(open_dir);
	if (!d) {
		*count = 0;
		return memalloc(sizeof(char *));
	}

	int cap = 64, n = 0;
	char **matches = memalloc(sizeof(char *) * cap);
	struct dirent *entry;

	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (base[0] == '\0' && entry->d_name[0] == '.')
			continue;
		if (!has_prefix(entry->d_name, base))
			continue;
		push_match(&matches, &n, &cap, entry->d_name);
	}
	closedir(d);

	dedup_matches(matches, &n);
	*count = n;
	return matches;
}

static int longest_common_prefix(char **strs, int count, char *out,
								 int out_size)
{
	if (count == 0) {
		out[0] = '\0';
		return 0;
	}
	int len = strlen(strs[0]);
	for (int i = 1; i < count; i++) {
		int j = 0;
		while (j < len && strs[i][j] == strs[0][j])
			j++;
		len = j;
	}
	if (len >= out_size)
		len = out_size - 1;
	memcpy(out, strs[0], len);
	out[len] = '\0';
	return len;
}

static void print_matches(char **matches, int count)
{
	int width = get_terminal_width();
	int maxlen = 0;
	for (int i = 0; i < count; i++) {
		int l = strlen(matches[i]);
		if (l > maxlen)
			maxlen = l;
	}
	int colwidth = maxlen + 2;
	int cols = width / colwidth;
	if (cols < 1)
		cols = 1;

	printf("\n");
	for (int i = 0; i < count; i++) {
		printf("%-*s", colwidth, matches[i]);
		if ((i + 1) % cols == 0)
			printf("\n");
	}
	if (count % cols != 0)
		printf("\n");
}

static int find_token_start(const char *buffer, int position)
{
	int i = position;
	while (i > 0 && buffer[i - 1] != ' ' && buffer[i - 1] != '\t')
		i--;
	return i;
}

static bool is_first_token(const char *buffer, int token_start)
{
	for (int i = 0; i < token_start; i++) {
		if (buffer[i] != ' ' && buffer[i] != '\t')
			return false;
	}
	return true;
}

static char *insert_at(char *buffer, size_t *bufsize, int *position,
					   const char *text, int len)
{
	size_t cur_len = strlen(buffer);
	size_t need = cur_len + len + 1;
	if (need >= *bufsize) {
		*bufsize = need + RL_BUFSIZE;
		buffer = memrealloc(buffer, *bufsize);
	}
	memmove(&buffer[*position + len], &buffer[*position],
			cur_len - *position + 1);
	memcpy(&buffer[*position], text, len);
	*position += len;
	return buffer;
}

char *readline(char **paths, const char *prompt)
{
	size_t bufsize = RL_BUFSIZE;
	int position = 0;
	char *buffer = memalloc(bufsize);
	buffer[0] = '\0';

	int prev_lines = 1;
	bool tab_repeat = false;
	char last_tab_token[PATH_MAX] = "";

	printf("%s", prompt);
	fflush(stdout);

	while (1) {
		int c = getchar();
		int buf_len = strlen(buffer);

		if (c != '\t')
			tab_repeat = false;

		switch (c) {
		case EOF:
			exit(EXIT_SUCCESS);

		case '\n':
			printf("\n");

			if (strstr(buffer, "!!") != NULL) {
				char *last_command = read_command(1);
				if (last_command != NULL) {
					char *pos = strstr(buffer, "!!");
					char tmp[1024];
					snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)(pos - buffer),
							 buffer, last_command, pos + 2);
					buffer = memrealloc(buffer, strlen(tmp) + 1);
					strcpy(buffer, tmp);
					bufsize = strlen(buffer) + 1;
					position = strlen(buffer);
					break;
				}
			}
			return buffer;

		case 127:
			if (position > 0) {
				memmove(&buffer[position - 1], &buffer[position],
						buf_len - position + 1);
				position--;
			}
			break;

		case '\t': {
			int token_start = find_token_start(buffer, position);
			int token_len = position - token_start;
			if (token_len >= PATH_MAX)
				break;
			char token[PATH_MAX];
			memcpy(token, buffer + token_start, token_len);
			token[token_len] = '\0';

			bool is_cmd = is_first_token(buffer, token_start);
			char dirpart[PATH_MAX] = "", basepart[PATH_MAX] = "";
			int count = 0;
			char **matches =
				is_cmd ? complete_command(paths, token, &count)
					   : complete_path(token, &count, dirpart, basepart);

			if (count == 0) {
				free_str_array(matches, count);
				break;
			}

			const char *typed = is_cmd ? token : basepart;

			if (count == 1) {
				const char *remainder = matches[0] + strlen(typed);
				buffer = insert_at(buffer, &bufsize, &position, remainder,
								   strlen(remainder));

				char suffix = ' ';
				if (!is_cmd) {
					char full[PATH_MAX];
					if (dirpart[0] == '~') {
						char *home = getenv("HOME");
						snprintf(full, sizeof(full), "%s%s%s", home ? home : "",
								 dirpart + 1, matches[0]);
					} else {
						snprintf(full, sizeof(full), "%s%s", dirpart,
								 matches[0]);
					}
					struct stat st;
					if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
						suffix = '/';
				}
				buffer = insert_at(buffer, &bufsize, &position, &suffix, 1);
				tab_repeat = false;
			} else {
				char lcp[PATH_MAX];
				longest_common_prefix(matches, count, lcp, sizeof(lcp));

				if (strlen(lcp) > strlen(typed)) {
					const char *remainder = lcp + strlen(typed);
					buffer = insert_at(buffer, &bufsize, &position, remainder,
									   strlen(remainder));
					tab_repeat = false;
				} else if (tab_repeat && strcmp(last_tab_token, token) == 0) {
					print_matches(matches, count);
					prev_lines = 1;
				} else {
					tab_repeat = true;
				}
			}

			snprintf(last_tab_token, sizeof(last_tab_token), "%s", token);
			free_str_array(matches, count);
			break;
		}

		case 18: {
			char *sel = histgrep_pick();

			if (sel) {
				size_t len = strlen(sel);

				if (len + 1 >= bufsize) {
					bufsize = len + 1 + RL_BUFSIZE;
					buffer = memrealloc(buffer, bufsize);
				}
				memcpy(buffer, sel, len + 1);
				position = len;
				free(sel);
			}
			break;
		}

		case 27:
			if (getchar() == '[') {
				int arrow = getchar();
				if (arrow == 'A') { // Up - history
					char *hist = read_command(1);
					if (hist) {
						strncpy(buffer, hist, bufsize - 1);
						buffer[bufsize - 1] = '\0';
						position = strlen(buffer);
					}
				} else if (arrow == 'B') { // Down
					char *hist = read_command(0);
					if (hist) {
						strncpy(buffer, hist, bufsize - 1);
						buffer[bufsize - 1] = '\0';
						position = strlen(buffer);
					} else {
						buffer[0] = '\0';
						position = 0;
					}
				} else if (arrow == 'C') { // Right
					if (position < buf_len)
						position++;
				} else if (arrow == 'D') { // Left
					if (position > 0)
						position--;
				}
			}
			break;

		default:
			if (c > 31 && c < 127) {
				if (position == buf_len) {
					buffer[position] = c;
					buffer[position + 1] = '\0';
				} else {
					memmove(&buffer[position + 1], &buffer[position],
							buf_len - position + 1);
					buffer[position] = c;
				}
				position++;
			}
		}

		render(prompt, buffer, position, &prev_lines, paths);

		if ((strlen(buffer) + 1) >= bufsize) {
			bufsize += RL_BUFSIZE;
			buffer = memrealloc(buffer, bufsize);
		}
	}
}

void command_loop(char **paths)
{
	char *line;
	int status = 1;

	while (status) {
		time_t t = time(NULL);
		struct tm *current_time = localtime(&t);
		char timestr[256];
		if (strftime(timestr, sizeof(timestr), "[%H:%M:%S]", current_time) == 0)
			return;
		char cwd[PATH_MAX];
		if (getcwd(cwd, PATH_MAX) == NULL)
			return;
		char full_cwd[PATH_MAX];
		snprintf(full_cwd, sizeof(full_cwd), "%s", cwd);
		char *home = getenv("HOME");

		int i = 0, j = 0;
		if (home) {
			size_t home_len = strlen(home);
			if (strncmp(cwd, home, home_len) == 0) {
				cwd[j++] = '~';
				i += home_len;
			}
		}
		while (cwd[i] != '\0')
			cwd[j++] = cwd[i++];
		cwd[j] = '\0';

		/* Blue time string, pink time, teal arrow */
		char *git_info = get_git_info();
		char git_str[128] = "";
		if (git_info != NULL) {
			snprintf(git_str, sizeof(git_str), "\033[31m[%s]", git_info);
			free(git_info);
		}
		char prompt[256];
		snprintf(prompt, sizeof(prompt),
				 "\033[34m%s \033[35m[%s] %s \033[36m> ", timestr, cwd,
				 git_str);

		cmd_count = 0;
		line = readline(paths, prompt);
		if (line == NULL) {
			printf("\n");
			continue;
		}
		save_command_history(line);

		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		status = execute_line(line);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		runlog_append(line,
					  (t1.tv_sec - t0.tv_sec) * 1000 +
						  (t1.tv_nsec - t0.tv_nsec) / 1000000,
					  last_exit_code, full_cwd);
		free(line);
	}
}

void quit_sig(int sig)
{
	(void)sig;
	exit(EXIT_SUCCESS);
}

void cleanup_terminal(void)
{
	change_terminal_attribute(0);
	printf("\033[?25h\n"); // show cursor
	fflush(stdout);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	// setup
	atexit(cleanup_terminal);
	signal(SIGINT, quit_sig);
	signal(SIGTERM, quit_sig);
	signal(SIGQUIT, quit_sig);
	check_history_file();
	check_aliases_file();
	char **paths = setup_path_variable();
	change_terminal_attribute(1);

	command_loop(paths);

	// cleanup
	free(paths);
	return EXIT_SUCCESS;
}
