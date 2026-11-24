#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "90s.h"
#include "parser.h"

#define OPERATORS "|<>&;"

static bool
is_word_end(const char *p)
{
	return isspace((unsigned char)*p) || strchr(OPERATORS, *p) != NULL;
}

static void
init_pipeline(pipeline_t *pl)
{
	memset(pl, 0, sizeof(*pl));
	pl->cmds_cap = 4;
	pl->cmds = memalloc(sizeof(command_t) * pl->cmds_cap);
}

static command_t *
push_command(pipeline_t *pl)
{
	if (pl->count >= pl->cmds_cap) {
		pl->cmds_cap *= 2;
		pl->cmds = memrealloc(pl->cmds, sizeof(command_t) * pl->cmds_cap);
	}
	command_t *c = &pl->cmds[pl->count++];
	memset(c, 0, sizeof(*c));
	c->argv_cap = 8;
	c->argv = memalloc(sizeof(char *) * c->argv_cap);
	c->argv[0] = NULL;
	return c;
}

static pipeline_t *
push_pipeline(list_t *l, int *cap)
{
	if (l->count >= *cap) {
		*cap *= 2;
		l->pipes = memrealloc(l->pipes, sizeof(pipeline_t) * *cap);
		l->seps = memrealloc(l->seps, sizeof(sep_t) * *cap);
	}
	init_pipeline(&l->pipes[l->count]);
	l->seps[l->count] = SEP_END;
	return &l->pipes[l->count++];
}

static void
push_redir(command_t *c, redir_kind_t kind, int fd, int src_fd, char *path)
{
	if (c->nredirs >= c->redirs_cap) {
		c->redirs_cap = c->redirs_cap ? c->redirs_cap * 2 : 4;
		c->redirs = memrealloc(c->redirs, sizeof(redir_t) * c->redirs_cap);
	}
	c->redirs[c->nredirs].kind = kind;
	c->redirs[c->nredirs].fd = fd;
	c->redirs[c->nredirs].src_fd = src_fd;
	c->redirs[c->nredirs].path = path;
	c->nredirs++;
}

static void
push_arg(command_t *c, char *word)
{
	if (c->argc + 2 > c->argv_cap) {
		c->argv_cap *= 2;
		c->argv = memrealloc(c->argv, sizeof(char *) * c->argv_cap);
	}
	c->argv[c->argc++] = word;
	c->argv[c->argc] = NULL;
}

static void
emit(char *word, size_t *n, char c, bool literal)
{
	if (literal && strchr("*?[\\", c))
		word[(*n)++] = QUOTE_MARK;
	word[(*n)++] = c;
}

static size_t
var_name_len(const char *q)
{
	size_t n = 0;

	if (*q == '?' || *q == '$')
		return 1;
	if (!isalpha((unsigned char)*q) && *q != '_')
		return 0;
	while (isalnum((unsigned char)q[n]) || q[n] == '_')
		n++;
	return n;
}

static const char *
tilde_dir(const char **pp)
{
	const char *q = *pp + 1;
	size_t n = 0;
	char name[256];
	struct passwd *pw;

	while (isalnum((unsigned char)q[n]) || q[n] == '_' || q[n] == '.' || q[n] == '-')
		n++;
	if (q[n] != '/' && !is_word_end(q + n))
		return NULL;
	if (n == 0) {
		*pp = q;
		return getenv("HOME");
	}
	if (n >= sizeof(name))
		return NULL;
	memcpy(name, q, n);
	name[n] = '\0';
	pw = getpwnam(name);
	if (!pw)
		return NULL;
	*pp = q + n;
	return pw->pw_dir;
}

static char *
read_word(const char **pp)
{
	const char *p = *pp;
	const char *dir = NULL;
	char *word;
	size_t n = 0, dir_len;
	bool in_single = false, in_double = false;

	if (*p == '~')
		dir = tilde_dir(&p);
	dir_len = dir ? strlen(dir) : 0;
	word = memalloc(2 * strlen(p) + 2 * dir_len + 2);
	for (size_t i = 0; i < dir_len; i++)
		emit(word, &n, dir[i], true);

	while (*p) {
		if (!in_single && !in_double && is_word_end(p))
			break;
		if (*p == '\'' && !in_double) {
			in_single = !in_single;
			p++;
		} else if (*p == '"' && !in_single) {
			in_double = !in_double;
			p++;
		} else if (*p == '\\' && !in_single && p[1]) {
			emit(word, &n, p[1], true);
			p += 2;
		} else if (*p == '$' && !in_single) {
			const char *name = p + 1;
			size_t len;
			bool braced = *name == '{';

			if (braced)
				name++;
			len = var_name_len(name);
			if (len == 0 || (braced && name[len] != '}')) {
				word[n++] = *p++;
				continue;
			}
			word[n++] = in_double ? VAR_QOPEN : VAR_OPEN;
			memcpy(word + n, name, len);
			n += len;
			word[n++] = VAR_CLOSE;
			p = name + len + (braced ? 1 : 0);
		} else {
			emit(word, &n, *p, in_single || in_double || *p == '\\');
			p++;
		}
	}

	if (in_single || in_double) {
		fputs("90s: syntax error: unterminated quote\n", stderr);
		free(word);
		return NULL;
	}
	word[n] = '\0';
	*pp = p;
	return word;
}

static char *
read_target(const char **pp)
{
	const char *p = *pp;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '\0' || strchr(OPERATORS, *p)) {
		fputs("90s: syntax error: missing redirection target\n", stderr);
		return NULL;
	}
	*pp = p;
	return read_word(pp);
}

bool
command_has_redirect(const command_t *c)
{
	return c->nredirs > 0;
}

void
free_pipeline(pipeline_t *pl)
{
	for (int i = 0; i < pl->count; i++) {
		command_t *c = &pl->cmds[i];
		for (int j = 0; j < c->argc; j++)
			free(c->argv[j]);
		free(c->argv);
		for (int j = 0; j < c->nredirs; j++)
			free(c->redirs[j].path);
		free(c->redirs);
	}
	free(pl->cmds);
	memset(pl, 0, sizeof(*pl));
}

void
free_list(list_t *l)
{
	for (int i = 0; i < l->count; i++)
		free_pipeline(&l->pipes[i]);
	free(l->pipes);
	free(l->seps);
	memset(l, 0, sizeof(*l));
}

/* [n]<f  [n]>f  [n]>>f  [n]>&m  [n]<&m  &>f  &>>f  >&f; returns 1 if consumed, 0 if not a redirection, -1 on error */
static int
parse_redirect(const char **pp, command_t *c)
{
	const char *p = *pp, *q;
	bool both = false, append = false;
	char *target;
	int fd = -1;

	if (isdigit((unsigned char)*p)) {
		for (q = p; isdigit((unsigned char)*q); q++)
			;
		if ((*q != '<' && *q != '>') || q - p > 3)
			return 0;
		fd = atoi(p);
		p = q;
	} else if (p[0] == '&' && p[1] == '>') {
		both = true;
		p++;
	} else if (*p != '<' && *p != '>') {
		return 0;
	}

	if (*p == '<') {
		p++;
		if (fd < 0)
			fd = 0;
		if (*p == '&') {
			for (q = ++p; isdigit((unsigned char)*q); q++)
				;
			if (q == p || !is_word_end(q)) {
				fputs("90s: syntax error: bad file descriptor\n", stderr);
				return -1;
			}
			push_redir(c, REDIR_DUP, fd, atoi(p), NULL);
			*pp = q;
			return 1;
		}
		if (!(target = read_target(&p)))
			return -1;
		push_redir(c, REDIR_IN, fd, -1, target);
		*pp = p;
		return 1;
	}

	p++;
	if (fd < 0)
		fd = 1;
	if (*p == '>') {
		append = true;
		p++;
	} else if (!both && *p == '&') {
		p++;
		for (q = p; isdigit((unsigned char)*q); q++)
			;
		if (q > p && is_word_end(q)) {
			push_redir(c, REDIR_DUP, fd, atoi(p), NULL);
			*pp = q;
			return 1;
		}
		both = true;
	}
	if (!(target = read_target(&p)))
		return -1;
	push_redir(c, append ? REDIR_APPEND : REDIR_OUT, fd, -1, target);
	if (both)
		push_redir(c, REDIR_DUP, 2, fd, NULL);
	*pp = p;
	return 1;
}

int
parse_list(const char *line, list_t *l)
{
	int cap = 4;
	const char *p = line;
	char *target;
	pipeline_t *pl;
	command_t *cur;

	memset(l, 0, sizeof(*l));
	l->pipes = memalloc(sizeof(pipeline_t) * cap);
	l->seps = memalloc(sizeof(sep_t) * cap);
	pl = push_pipeline(l, &cap);
	cur = push_command(pl);

	while (*p) {
		sep_t sep = SEP_END;
		bool bg = false;

		if (isspace((unsigned char)*p)) {
			p++;
			continue;
		}

		if (*p == ';') {
			sep = SEP_SEQ;
			p++;
		} else if (p[0] == '&' && p[1] == '&') {
			sep = SEP_AND;
			p += 2;
		} else if (p[0] == '|' && p[1] == '|') {
			sep = SEP_OR;
			p += 2;
		} else if (*p == '&' && p[1] != '>') {
			sep = SEP_SEQ;
			bg = true;
			p++;
		}
		if (sep != SEP_END) {
			if (cur->argc == 0) {
				fputs("90s: syntax error: missing command\n", stderr);
				goto fail;
			}
			pl->background = bg;
			l->seps[l->count - 1] = sep;
			while (isspace((unsigned char)*p))
				p++;
			if (*p == '\0' && sep == SEP_SEQ) {
				l->seps[l->count - 1] = SEP_END;
				break;
			}
			pl = push_pipeline(l, &cap);
			cur = push_command(pl);
			continue;
		}

		if (*p == '|') {
			if (cur->argc == 0) {
				fputs("90s: syntax error near '|'\n", stderr);
				goto fail;
			}
			p++;
			cur = push_command(pl);
			continue;
		}

		switch (parse_redirect(&p, cur)) {
		case -1:
			goto fail;
		case 1:
			continue;
		}

		if (!(target = read_word(&p)))
			goto fail;
		push_arg(cur, target);
	}

	if (cur->argc == 0) {
		if (l->count == 1 && pl->count == 1 && !command_has_redirect(cur)) {
			free_list(l);
			return 0;
		}
		fputs("90s: syntax error: missing command\n", stderr);
		goto fail;
	}
	return 0;

fail:
	free_list(l);
	return -1;
}

static char *
dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = memalloc(n);

	memcpy(p, s, n);
	return p;
}

static char *
expand_word(const char *w, int status, bool *drop)
{
	size_t cap = strlen(w) + 32, n = 0;
	char *out = memalloc(cap);
	bool only_refs = true, had_ref = false;

	while (*w) {
		const char *end;

		if ((*w == VAR_OPEN || *w == VAR_QOPEN) && (end = strchr(w + 1, VAR_CLOSE))) {
			char name[256], num[32];
			const char *val;
			size_t len = end - (w + 1), vlen;

			if (len >= sizeof(name))
				len = sizeof(name) - 1;
			memcpy(name, w + 1, len);
			name[len] = '\0';
			if (strcmp(name, "?") == 0) {
				snprintf(num, sizeof(num), "%d", status);
				val = num;
			} else if (strcmp(name, "$") == 0) {
				snprintf(num, sizeof(num), "%d", (int)getpid());
				val = num;
			} else {
				val = getenv(name);
				if (!val)
					val = "";
			}
			vlen = strlen(val);
			if (n + 2 * vlen + 2 > cap) {
				cap = (n + 2 * vlen + 2) * 2;
				out = memrealloc(out, cap);
			}
			for (size_t i = 0; i < vlen; i++)
				emit(out, &n, val[i], true);
			had_ref = true;
			if (*w == VAR_QOPEN)
				only_refs = false;
			w = end + 1;
			continue;
		}
		if (n + 2 > cap) {
			cap *= 2;
			out = memrealloc(out, cap);
		}
		out[n++] = *w++;
		only_refs = false;
	}
	out[n] = '\0';
	*drop = had_ref && only_refs && n == 0;
	return out;
}

static bool
has_glob(const char *w)
{
	for (; *w; w++) {
		if (*w == QUOTE_MARK && w[1])
			w++;
		else if (strchr("*?[", *w))
			return true;
	}
	return false;
}

/* drops the quote markers; with as_pattern, keeps them as glob backslash escapes */
static char *
unmark(const char *w, bool as_pattern)
{
	char *out = memalloc(2 * strlen(w) + 1);
	size_t n = 0;

	while (*w) {
		if (*w == QUOTE_MARK && w[1]) {
			if (as_pattern)
				out[n++] = '\\';
			w++;
		}
		out[n++] = *w++;
	}
	out[n] = '\0';
	return out;
}

static void
expand_file(char **file, int status)
{
	bool drop;
	char *e, *plain;

	if (!*file)
		return;
	e = expand_word(*file, status, &drop);
	plain = unmark(e, false);
	free(e);
	free(*file);
	*file = plain;
}

static void
argv_push(command_t *c, char *s)
{
	if (c->argc + 2 > c->argv_cap) {
		c->argv_cap *= 2;
		c->argv = memrealloc(c->argv, sizeof(char *) * c->argv_cap);
	}
	c->argv[c->argc++] = s;
	c->argv[c->argc] = NULL;
}

void
expand_pipeline(pipeline_t *pl, int status)
{
	for (int i = 0; i < pl->count; i++) {
		command_t *c = &pl->cmds[i];
		char **old = c->argv;
		int old_argc = c->argc;

		c->argv_cap = 8;
		c->argv = memalloc(sizeof(char *) * c->argv_cap);
		c->argv[0] = NULL;
		c->argc = 0;

		for (int j = 0; j < old_argc; j++) {
			bool drop;
			char *e = expand_word(old[j], status, &drop);
			glob_t g;

			free(old[j]);
			if (drop) {
				free(e);
				continue;
			}
			if (has_glob(e)) {
				char *pat = unmark(e, true);
				int rc = glob(pat, 0, NULL, &g);

				free(pat);
				if (rc == 0) {
					for (size_t k = 0; k < g.gl_pathc; k++)
						argv_push(c, dup_str(g.gl_pathv[k]));
					globfree(&g);
					free(e);
					continue;
				}
			}
			argv_push(c, unmark(e, false));
			free(e);
		}
		free(old);
		for (int k = 0; k < c->nredirs; k++)
			expand_file(&c->redirs[k].path, status);
	}
}
