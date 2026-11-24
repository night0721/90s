#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "90s.h"
#include "constants.h"
#include "history.h"

int cmd_count = 0;

static char *histfile_path;

/* newest MAX_HISTORY entries, extended from the file's tail whenever it grows
 */
static char **cache;
static size_t cache_n, cache_cap;
static off_t loaded;
static ino_t loaded_ino;

static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = memalloc(n);

	memcpy(p, s, n);
	return p;
}

static void cache_clear(void)
{
	for (size_t i = 0; i < cache_n; i++)
		free(cache[i]);
	cache_n = 0;
	loaded = 0;
}

static void cache_append(char **lines, size_t k)
{
	size_t first = k > (size_t)MAX_HISTORY ? k - MAX_HISTORY : 0;
	size_t add = k - first;

	if (cache_n + add > (size_t)MAX_HISTORY) {
		size_t drop = cache_n + add - MAX_HISTORY;

		for (size_t i = 0; i < drop; i++)
			free(cache[i]);
		memmove(cache, cache + drop, sizeof(char *) * (cache_n - drop));
		cache_n -= drop;
	}
	if (cache_n + add > cache_cap) {
		while (cache_n + add > cache_cap)
			cache_cap = cache_cap ? cache_cap * 2 : 256;
		cache = memrealloc(cache, sizeof(char *) * cache_cap);
	}
	for (size_t i = first; i < k; i++)
		cache[cache_n++] = dup_str(lines[i]);
}

static char *read_range(off_t from, size_t want, size_t *got_out)
{
	FILE *f = fopen(histfile_path, "r");
	char *buf;
	size_t got;

	if (!f)
		return NULL;
	if (fseeko(f, from, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = memalloc(want + 1);
	got = fread(buf, 1, want, f);
	fclose(f);
	buf[got] = '\0';
	*got_out = got;
	return buf;
}

/* splits the complete lines of chunk in place, skipping empty ones; returns the
 * bytes consumed */
static size_t split_lines(char *chunk, size_t len, char ***out, size_t *count)
{
	size_t consumed = len, nl = 0, k = 0;
	char **lines, *p, *end;

	while (consumed > 0 && chunk[consumed - 1] != '\n')
		consumed--;
	*out = NULL;
	*count = 0;
	if (consumed == 0)
		return 0;
	for (size_t i = 0; i < consumed; i++) {
		if (chunk[i] == '\n')
			nl++;
	}
	lines = memalloc(sizeof(char *) * nl);
	for (p = chunk, end = chunk + consumed; p < end;) {
		char *e = memchr(p, '\n', end - p);

		*e = '\0';
		if (*p)
			lines[k++] = p;
		p = e + 1;
	}
	*out = lines;
	*count = k;
	return consumed;
}

/* first load: only the end of the file, widened until MAX_HISTORY lines are
 * found, so the file itself is never rewritten or fully read */
static void history_load_tail(off_t size)
{
	size_t window = 65536;

	for (;;) {
		off_t from = size > (off_t)window ? size - (off_t)window : 0;
		size_t got, skip = 0, consumed, k;
		char *chunk = read_range(from, size - from, &got), **lines;

		if (!chunk)
			return;
		if (from > 0) {
			char *nl = memchr(chunk, '\n', got);

			skip = nl ? (size_t)(nl + 1 - chunk) : got;
		}
		consumed = split_lines(chunk + skip, got - skip, &lines, &k);
		if (from == 0 || k >= (size_t)MAX_HISTORY) {
			if (consumed > 0) {
				cache_append(lines, k);
				free(lines);
			}
			loaded = from + skip + consumed;
			free(chunk);
			return;
		}
		free(lines);
		free(chunk);
		window *= 4;
	}
}

static void history_refresh(void)
{
	struct stat st;
	size_t got, consumed, k;
	char *chunk, **lines;

	if (!histfile_path || stat(histfile_path, &st) != 0)
		return;
	if (st.st_size < loaded || (loaded > 0 && st.st_ino != loaded_ino))
		cache_clear();
	loaded_ino = st.st_ino;
	if (st.st_size == loaded)
		return;
	if (loaded == 0) {
		history_load_tail(st.st_size);
		return;
	}
	chunk = read_range(loaded, st.st_size - loaded, &got);
	if (!chunk)
		return;
	consumed = split_lines(chunk, got, &lines, &k);
	if (consumed > 0) {
		cache_append(lines, k);
		free(lines);
		loaded += consumed;
	}
	free(chunk);
}

void check_history_file(void)
{
	char *env_home;
	size_t path_len;
	int fd;

	env_home = getenv("XDG_CONFIG_HOME");
	if (env_home == NULL)
		// fallback to $HOME if $XDG_CONFIG_HOME is null
		env_home = getenv("HOME");
	if (env_home == NULL) {
		fprintf(
			stderr,
			"90s: HOME AND XDG_CONFIG_HOME environment variable is missing\n");
		exit(EXIT_FAILURE);
	}
	path_len = strlen(env_home) + strlen(HISTFILE) + 2;
	histfile_path = memalloc(path_len);
	snprintf(histfile_path, path_len, "%s/%s", env_home, HISTFILE);

	fd = open(histfile_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd == -1) {
		fprintf(stderr, "90s: cannot open history file %s, history disabled\n",
				histfile_path);
		free(histfile_path);
		histfile_path = NULL;
		return;
	}
	close(fd);
}

void save_command_history(char *args)
{
	size_t len = strlen(args), i;
	char *line;
	ssize_t r;
	int fd;

	if (!histfile_path)
		return;
	for (i = 0; i < len && isspace((unsigned char)args[i]); i++)
		;
	if (i == len)
		return;

	line = memalloc(len + 2);
	for (i = 0; i < len; i++)
		line[i] = args[i] == '\n' ? ' ' : args[i];
	line[len] = '\n';
	fd = open(histfile_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd != -1) {
		r = write(fd, line, len + 1);
		(void)r;
		close(fd);
	}
	free(line);
}

/* the returned string belongs to the cache: valid until the next history call,
 * never freed by the caller */
char *read_command(int direction)
{
	if (direction == 1) {
		cmd_count++;
	} else {
		if (cmd_count == 0)
			return NULL;
		cmd_count--;
	}
	history_refresh();
	if (cmd_count > (int)cache_n) {
		cmd_count = cache_n;
		return NULL;
	}
	if (cmd_count == 0)
		return NULL;
	return cache[cache_n - cmd_count];
}

/* returns a fresh NULL-terminated array of copies; the caller frees every entry
 * and the array */
char **get_all_history(bool check)
{
	char **out;
	const char **seen = NULL;
	size_t n = 0, seen_cap = 16;

	history_refresh();
	out = memalloc(sizeof(char *) * (cache_n + 1));
	if (check) {
		while (seen_cap < cache_n * 2)
			seen_cap <<= 1;
		seen = memalloc(sizeof(char *) * seen_cap);
		memset(seen, 0, sizeof(char *) * seen_cap);
	}
	for (size_t i = 0; i < cache_n; i++) {
		if (check) {
			unsigned long h = 5381;
			size_t slot;
			bool dup = false;

			for (const char *p = cache[i]; *p; p++)
				h = h * 33 + (unsigned char)*p;
			slot = h & (seen_cap - 1);
			while (seen[slot]) {
				if (strcmp(seen[slot], cache[i]) == 0) {
					dup = true;
					break;
				}
				slot = (slot + 1) & (seen_cap - 1);
			}
			if (dup)
				continue;
			seen[slot] = cache[i];
		}
		out[n++] = dup_str(cache[i]);
	}
	out[n] = NULL;
	free(seen);
	return out;
}
