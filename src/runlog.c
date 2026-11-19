#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "90s.h"
#include "runlog.h"

#define RUNLOG_FILE ".90s_runlog"
#define CDHIST_FILE ".90s_cdhist"
#define PICKER_CMD "neo"
#define LIST_DEFAULT 20
#define HISTGREP_DEFAULT 50

int cd(char **args);

typedef struct {
	long ts;
	long dur_ms;
	int code;
	char *cwd;
	char *cmd;
} entry_t;

typedef struct {
	char *buf;
	entry_t *v;
	size_t n;
} log_t;

typedef struct {
	char *path;
	long count;
	long last;
	double score;
} dir_t;

typedef struct {
	char *buf;
	dir_t *v;
	size_t n;
} dirs_t;

typedef struct {
	char *key;
	long count;
	long fail;
	long dur_ms;
} group_t;

typedef struct {
	const char **slot;
	size_t cap;
} seen_t;

static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = memalloc(n);

	memcpy(p, s, n);
	return p;
}

static void sanitize(char *s)
{
	for (; *s; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
	}
}

static char *data_path(const char *name, char *out, size_t size)
{
	const char *home = getenv("HOME");

	snprintf(out, size, "%s/%s", home ? home : ".", name);
	return out;
}

static void append_line(const char *name, const char *line, size_t len)
{
	char path[PATH_MAX];
	int fd = open(data_path(name, path, sizeof(path)),
				  O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd == -1)
		return;
	ssize_t r = write(fd, line, len);
	(void)r;
	close(fd);
}

static char *read_file(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	FILE *f = fopen(data_path(name, path, sizeof(path)), "r");
	if (!f)
		return NULL;
	if (fstat(fileno(f), &st) != 0) {
		fclose(f);
		return NULL;
	}
	char *buf = memalloc(st.st_size + 1);
	size_t got = fread(buf, 1, st.st_size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

void runlog_append(const char *cmd, long dur_ms, int code, const char *cwd)
{
	const char *p = cmd;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '\0' || cmd[0] == ' ')
		return;

	char *c = dup_str(cmd);
	char *w = dup_str(cwd);
	sanitize(c);
	sanitize(w);
	size_t size = strlen(c) + strlen(w) + 64;
	char *line = memalloc(size);
	int len = snprintf(line, size, "%ld\t%ld\t%d\t%s\t%s\n", (long)time(NULL),
					   dur_ms, code, w, c);
	append_line(RUNLOG_FILE, line, len);
	free(line);
	free(c);
	free(w);
}

void cdhist_append(const char *path)
{
	char *p = dup_str(path);
	size_t size = strlen(p) + 32;
	char *line = memalloc(size);

	sanitize(p);
	int len = snprintf(line, size, "%ld\t%s\n", (long)time(NULL), p);
	append_line(CDHIST_FILE, line, len);
	free(line);
	free(p);
}

static int load_runlog(log_t *l)
{
	size_t cap = 1024;

	l->v = NULL;
	l->n = 0;
	l->buf = read_file(RUNLOG_FILE);
	if (!l->buf)
		return -1;
	l->v = memalloc(sizeof(entry_t) * cap);

	char *next;
	for (char *line = l->buf; *line; line = next) {
		char *nl = strchr(line, '\n');
		char *f[5];
		int k;

		next = nl ? nl + 1 : line + strlen(line);
		if (nl)
			*nl = '\0';
		f[0] = line;
		for (k = 1; k < 5; k++) {
			char *tab = strchr(f[k - 1], '\t');
			if (!tab)
				break;
			*tab = '\0';
			f[k] = tab + 1;
		}
		if (k < 5)
			continue;
		if (l->n >= cap) {
			cap *= 2;
			l->v = memrealloc(l->v, sizeof(entry_t) * cap);
		}
		l->v[l->n].ts = atol(f[0]);
		l->v[l->n].dur_ms = atol(f[1]);
		l->v[l->n].code = atoi(f[2]);
		l->v[l->n].cwd = f[3];
		l->v[l->n].cmd = f[4];
		l->n++;
	}
	return 0;
}

static void free_log(log_t *l)
{
	free(l->buf);
	free(l->v);
}

static double recency_factor(long age)
{
	if (age < 3600)
		return 4.0;
	if (age < 86400)
		return 2.0;
	if (age < 604800)
		return 0.5;
	return 0.25;
}

static int cmp_dir_path(const void *a, const void *b)
{
	return strcmp(((const dir_t *)a)->path, ((const dir_t *)b)->path);
}

static int cmp_dir_score(const void *a, const void *b)
{
	const dir_t *x = a, *y = b;

	if (x->score != y->score)
		return x->score < y->score ? 1 : -1;
	return (y->last > x->last) - (y->last < x->last);
}

static int cmp_dir_last(const void *a, const void *b)
{
	const dir_t *x = a, *y = b;

	return (y->last > x->last) - (y->last < x->last);
}

static int load_dirs(dirs_t *d)
{
	size_t cap = 256, n = 0, out = 0, keep = 0;
	long now = (long)time(NULL);
	char *line, *next;
	dir_t *v;

	d->v = NULL;
	d->n = 0;
	d->buf = read_file(CDHIST_FILE);
	if (!d->buf)
		return -1;
	v = memalloc(sizeof(dir_t) * cap);

	for (line = d->buf; *line; line = next) {
		char *nl = strchr(line, '\n');
		char *tab;

		next = nl ? nl + 1 : line + strlen(line);
		if (nl)
			*nl = '\0';
		tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		if (n >= cap) {
			cap *= 2;
			v = memrealloc(v, sizeof(dir_t) * cap);
		}
		v[n].path = tab + 1;
		v[n].last = atol(line);
		v[n].count = 1;
		v[n].score = 0;
		n++;
	}

	qsort(v, n, sizeof(dir_t), cmp_dir_path);
	for (size_t i = 0; i < n; i++) {
		if (out > 0 && strcmp(v[out - 1].path, v[i].path) == 0) {
			v[out - 1].count++;
			if (v[i].last > v[out - 1].last)
				v[out - 1].last = v[i].last;
		} else {
			v[out++] = v[i];
		}
	}

	for (size_t i = 0; i < out; i++) {
		struct stat st;

		if (stat(v[i].path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		v[i].score = v[i].count * recency_factor(now - v[i].last);
		v[keep++] = v[i];
	}
	d->v = v;
	d->n = keep;
	return 0;
}

static void free_dirs(dirs_t *d)
{
	free(d->buf);
	free(d->v);
}

static bool contains_ci(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);

	if (nl == 0)
		return true;
	for (; *hay; hay++) {
		if (strncasecmp(hay, needle, nl) == 0)
			return true;
	}
	return false;
}

static bool fuzzy_ci(const char *hay, const char *needle)
{
	for (; *needle; hay++) {
		if (!*hay)
			return false;
		if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle))
			needle++;
	}
	return true;
}

static bool matches_words(const char *hay, char **words, bool fuzzy)
{
	for (; *words; words++) {
		if (!(fuzzy ? fuzzy_ci(hay, *words) : contains_ci(hay, *words)))
			return false;
	}
	return true;
}

static bool under_dir(const char *cwd, const char *dir)
{
	size_t len = strlen(dir);

	if (strncmp(cwd, dir, len) != 0)
		return false;
	return cwd[len] == '\0' || cwd[len] == '/' ||
		   (len > 0 && dir[len - 1] == '/');
}

static void seen_init(seen_t *s, size_t n)
{
	s->cap = 16;
	while (s->cap < n * 2)
		s->cap <<= 1;
	s->slot = memalloc(sizeof(char *) * s->cap);
	memset(s->slot, 0, sizeof(char *) * s->cap);
}

static bool seen_add(seen_t *s, const char *str)
{
	unsigned long h = 5381;
	size_t i;

	for (const char *p = str; *p; p++)
		h = h * 33 + (unsigned char)*p;
	i = h & (s->cap - 1);
	while (s->slot[i]) {
		if (strcmp(s->slot[i], str) == 0)
			return false;
		i = (i + 1) & (s->cap - 1);
	}
	s->slot[i] = str;
	return true;
}

static void fmt_dur(long ms, char *out, size_t size)
{
	if (ms < 1000)
		snprintf(out, size, "%ldms", ms);
	else if (ms < 60000)
		snprintf(out, size, "%.1fs", ms / 1000.0);
	else if (ms < 3600000)
		snprintf(out, size, "%ldm%02lds", ms / 60000, ms / 1000 % 60);
	else
		snprintf(out, size, "%ldh%02ldm", ms / 3600000, ms / 60000 % 60);
}

static const char *short_path(const char *path, char *out, size_t size)
{
	const char *home = getenv("HOME");
	size_t hl = home ? strlen(home) : 0;

	if (hl > 1 && strncmp(path, home, hl) == 0 &&
		(path[hl] == '/' || path[hl] == '\0')) {
		snprintf(out, size, "~%s", path + hl);
		return out;
	}
	return path;
}

static void print_entry(const entry_t *e)
{
	char when[32] = "", dur[16], cwd[PATH_MAX];
	time_t ts = e->ts;
	struct tm *tm = localtime(&ts);

	if (tm)
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
	fmt_dur(e->dur_ms, dur, sizeof(dur));
	printf("%s  %3d  %7s  %s  %s\n", when, e->code, dur,
		   short_path(e->cwd, cwd, sizeof(cwd)), e->cmd);
}

static char *pick(char **items, size_t n)
{
	char tmpl[] = "/tmp/90s-pick-XXXXXX";
	char cmd[96];
	char *line = NULL;
	size_t cap = 0;
	ssize_t len = -1;
	FILE *f, *p;
	int fd;

	fd = mkstemp(tmpl);
	if (fd == -1)
		return NULL;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmpl);
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		fputs(items[i], f);
		fputc('\n', f);
	}
	fclose(f);

	snprintf(cmd, sizeof(cmd), PICKER_CMD " < %s", tmpl);
	fflush(NULL);
	change_terminal_attribute(0);
	p = popen(cmd, "r");
	if (p) {
		len = getline(&line, &cap, p);
		pclose(p);
	}
	change_terminal_attribute(1);
	unlink(tmpl);

	if (len > 0 && line[len - 1] == '\n')
		line[--len] = '\0';
	if (len <= 0) {
		free(line);
		return NULL;
	}
	return line;
}

char *histgrep_pick(void)
{
	log_t l;
	seen_t seen;
	char **items, *sel;
	size_t n = 0;

	if (load_runlog(&l) == -1 || l.n == 0) {
		free_log(&l);
		return NULL;
	}
	seen_init(&seen, l.n);
	items = memalloc(sizeof(char *) * l.n);
	for (size_t k = l.n; k-- > 0;) {
		if (seen_add(&seen, l.v[k].cmd))
			items[n++] = l.v[k].cmd;
	}
	sel = pick(items, n);
	free(items);
	free(seen.slot);
	free_log(&l);
	return sel;
}

int histgrep(char **args)
{
	bool ok_only = false, fail_only = false, fuzzy = false;
	bool piped = !isatty(STDOUT_FILENO);
	char dirprefix[PATH_MAX] = "";
	long limit = -1;
	log_t l;
	seen_t seen;
	size_t *sel, ns = 0;
	char **words;
	int i;

	for (i = 1; args[i] && args[i][0] == '-' && args[i][1]; i++) {
		if (strcmp(args[i], "-s") == 0) {
			ok_only = true;
		} else if (strcmp(args[i], "-f") == 0) {
			fail_only = true;
		} else if (strcmp(args[i], "-z") == 0) {
			fuzzy = true;
		} else if (strcmp(args[i], "-n") == 0 && args[i + 1]) {
			limit = atol(args[++i]);
		} else if (strcmp(args[i], "-d") == 0 && args[i + 1]) {
			i++;
			if (!realpath(args[i], dirprefix))
				snprintf(dirprefix, sizeof(dirprefix), "%s", args[i]);
		} else {
			fputs(
				"usage: histgrep [-s|-f] [-z] [-d dir] [-n count] [word...]\n",
				stderr);
			return -1;
		}
	}
	words = &args[i];

	if (load_runlog(&l) == -1 || l.n == 0) {
		fputs("90s: histgrep: no history\n", stderr);
		free_log(&l);
		return -1;
	}
	if (limit < 0)
		limit = piped ? 0 : HISTGREP_DEFAULT;
	if (piped)
		seen_init(&seen, l.n);
	sel = memalloc(sizeof(size_t) * l.n);

	for (size_t k = l.n; k-- > 0;) {
		entry_t *e = &l.v[k];

		if (ok_only && e->code != 0)
			continue;
		if (fail_only && e->code == 0)
			continue;
		if (dirprefix[0] && !under_dir(e->cwd, dirprefix))
			continue;
		if (!matches_words(e->cmd, words, fuzzy))
			continue;
		if (piped && !seen_add(&seen, e->cmd))
			continue;
		sel[ns++] = k;
		if (limit > 0 && ns >= (size_t)limit)
			break;
	}

	if (piped) {
		for (size_t k = 0; k < ns; k++)
			puts(l.v[sel[k]].cmd);
		free(seen.slot);
	} else {
		for (size_t k = ns; k-- > 0;)
			print_entry(&l.v[sel[k]]);
	}
	free(sel);
	free_log(&l);
	return 1;
}

static void first_word(const char *cmd, char *out, size_t size)
{
	size_t n = strcspn(cmd, " ");

	if (n >= size)
		n = size - 1;
	memcpy(out, cmd, n);
	out[n] = '\0';
}

static void project_of(const char *cwd, char *out, size_t size)
{
	const char *home = getenv("HOME");
	char dir[PATH_MAX], probe[PATH_MAX + 8];
	char *slash;

	snprintf(dir, sizeof(dir), "%s", cwd);
	for (;;) {
		if (home && strcmp(dir, home) == 0)
			break;
		snprintf(probe, sizeof(probe), "%s/.git", dir);
		if (access(probe, F_OK) == 0) {
			snprintf(out, size, "%s", dir);
			return;
		}
		slash = strrchr(dir, '/');
		if (!slash || slash == dir)
			break;
		*slash = '\0';
	}
	snprintf(out, size, "%s", cwd);
}

static int cmp_group_key(const void *a, const void *b)
{
	return strcmp(((const group_t *)a)->key, ((const group_t *)b)->key);
}

static int cmp_group_count(const void *a, const void *b)
{
	const group_t *x = a, *y = b;

	return (y->count > x->count) - (y->count < x->count);
}

static int cmp_group_dur(const void *a, const void *b)
{
	const group_t *x = a, *y = b;

	return (y->dur_ms > x->dur_ms) - (y->dur_ms < x->dur_ms);
}

static group_t *group_entries(const log_t *l, bool by_project, size_t *n_out)
{
	group_t *g = memalloc(sizeof(group_t) * l->n);
	char cache_cwd[PATH_MAX] = "", cache_proj[PATH_MAX] = "", word[PATH_MAX];
	size_t out = 0;

	for (size_t i = 0; i < l->n; i++) {
		const entry_t *e = &l->v[i];

		if (by_project) {
			if (strcmp(cache_cwd, e->cwd) != 0) {
				project_of(e->cwd, cache_proj, sizeof(cache_proj));
				snprintf(cache_cwd, sizeof(cache_cwd), "%s", e->cwd);
			}
			g[i].key = dup_str(cache_proj);
		} else {
			first_word(e->cmd, word, sizeof(word));
			g[i].key = dup_str(word);
		}
		g[i].count = 1;
		g[i].fail = e->code != 0;
		g[i].dur_ms = e->dur_ms;
	}

	qsort(g, l->n, sizeof(group_t), cmp_group_key);
	for (size_t i = 0; i < l->n; i++) {
		if (out > 0 && strcmp(g[out - 1].key, g[i].key) == 0) {
			g[out - 1].count += g[i].count;
			g[out - 1].fail += g[i].fail;
			g[out - 1].dur_ms += g[i].dur_ms;
			free(g[i].key);
		} else {
			g[out++] = g[i];
		}
	}
	*n_out = out;
	return g;
}

static void print_groups(group_t *g, size_t n, long limit, bool by_project)
{
	char dur[16], key[PATH_MAX];

	qsort(g, n, sizeof(group_t), by_project ? cmp_group_dur : cmp_group_count);
	for (size_t i = 0; i < n && (long)i < limit; i++) {
		fmt_dur(g[i].dur_ms, dur, sizeof(dur));
		printf("%6ld  %8s  %4ld  %s\n", g[i].count, dur, g[i].fail,
			   by_project ? short_path(g[i].key, key, sizeof(key)) : g[i].key);
	}
}

int runlog(char **args)
{
	const char *sub = args[1] ? args[1] : "tail";
	long n = (args[1] && args[2]) ? atol(args[2]) : LIST_DEFAULT;
	log_t l;

	if (n <= 0)
		n = LIST_DEFAULT;
	if (load_runlog(&l) == -1 || l.n == 0) {
		fputs("90s: runlog: no history\n", stderr);
		free_log(&l);
		return -1;
	}

	if (strcmp(sub, "tail") == 0) {
		size_t start = (size_t)n >= l.n ? 0 : l.n - n;

		for (size_t k = start; k < l.n; k++)
			print_entry(&l.v[k]);
	} else if (strcmp(sub, "top") == 0 || strcmp(sub, "time") == 0) {
		bool by_project = strcmp(sub, "time") == 0;
		size_t ng;
		group_t *g = group_entries(&l, by_project, &ng);

		print_groups(g, ng, n, by_project);
		for (size_t k = 0; k < ng; k++)
			free(g[k].key);
		free(g);
	} else {
		fputs("usage: runlog [tail|top|time] [count]\n", stderr);
		free_log(&l);
		return -1;
	}
	free_log(&l);
	return 1;
}

int cdhist(char **args)
{
	bool recent = false, interactive = false, piped = !isatty(STDOUT_FILENO);
	dirs_t d;
	dir_t **sel;
	char **words;
	size_t n = 0;
	int i, rc = 1;

	for (i = 1; args[i] && args[i][0] == '-' && args[i][1]; i++) {
		if (strcmp(args[i], "-r") == 0) {
			recent = true;
		} else if (strcmp(args[i], "-i") == 0) {
			interactive = true;
		} else {
			fputs("usage: cdhist [-r] [-i] [query...]\n", stderr);
			return -1;
		}
	}
	words = &args[i];

	if (load_dirs(&d) == -1 || d.n == 0) {
		fputs("90s: cdhist: no history\n", stderr);
		free_dirs(&d);
		return -1;
	}
	qsort(d.v, d.n, sizeof(dir_t), recent ? cmp_dir_last : cmp_dir_score);
	sel = memalloc(sizeof(dir_t *) * d.n);
	for (size_t k = 0; k < d.n; k++) {
		if (matches_words(d.v[k].path, words, false))
			sel[n++] = &d.v[k];
	}

	if (n == 0) {
		fputs("90s: cdhist: no match\n", stderr);
		rc = -1;
	} else if (interactive) {
		char **paths = memalloc(sizeof(char *) * n);
		char *choice;

		for (size_t k = 0; k < n; k++)
			paths[k] = sel[k]->path;
		choice = pick(paths, n);
		if (choice) {
			char *cd_args[] = {"cd", choice, NULL};

			cd(cd_args);
			free(choice);
		}
		free(paths);
	} else if (words[0] && !piped) {
		char *cd_args[] = {"cd", sel[0]->path, NULL};

		cd(cd_args);
		puts(sel[0]->path);
	} else if (piped) {
		for (size_t k = 0; k < n; k++)
			puts(sel[k]->path);
	} else {
		char path[PATH_MAX];

		for (size_t k = 0; k < n && k < LIST_DEFAULT; k++)
			printf("%7.1f  %5ld  %s\n", sel[k]->score, sel[k]->count,
				   short_path(sel[k]->path, path, sizeof(path)));
	}
	free(sel);
	free_dirs(&d);
	return rc;
}
