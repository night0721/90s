#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "90s.h"
#include "parser.h"

#define OPERATORS "|<>&;"

static void
set_str(char **dst, char *src)
{
	free(*dst);
	*dst = src;
}

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
push_arg(command_t *c, char *word)
{
	if (c->argc + 2 > c->argv_cap) {
		c->argv_cap *= 2;
		c->argv = memrealloc(c->argv, sizeof(char *) * c->argv_cap);
	}
	c->argv[c->argc++] = word;
	c->argv[c->argc] = NULL;
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
	word = memalloc(2 * strlen(p) + dir_len + 2);
	if (dir) {
		memcpy(word, dir, dir_len);
		n = dir_len;
	}

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
			word[n++] = p[1];
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
			word[n++] = *p++;
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
	return c->in_file || c->out_file || c->err_file || c->err_to_out;
}

void
free_pipeline(pipeline_t *pl)
{
	for (int i = 0; i < pl->count; i++) {
		command_t *c = &pl->cmds[i];
		for (int j = 0; j < c->argc; j++)
			free(c->argv[j]);
		free(c->argv);
		free(c->in_file);
		free(c->out_file);
		free(c->err_file);
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

		if (*p == '<') {
			p++;
			if (!(target = read_target(&p)))
				goto fail;
			set_str(&cur->in_file, target);
			continue;
		}

		if (strncmp(p, "2>&1", 4) == 0) {
			cur->err_to_out = true;
			p += 4;
			continue;
		}

		if (*p == '2' && p[1] == '>') {
			p += 2;
			cur->err_append = false;
			if (*p == '>') {
				cur->err_append = true;
				p++;
			}
			if (!(target = read_target(&p)))
				goto fail;
			set_str(&cur->err_file, target);
			continue;
		}

		if (*p == '>' || *p == '&') {
			bool both = false, append = false;

			if (*p == '&') {
				both = true;
				p++;
			}
			p++;
			if (*p == '>') {
				append = true;
				p++;
			} else if (!both && *p == '&') {
				both = true;
				p++;
			}
			if (!(target = read_target(&p)))
				goto fail;
			set_str(&cur->out_file, target);
			cur->out_append = append;
			if (both)
				cur->err_to_out = true;
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
			if (n + vlen + 2 > cap) {
				cap = (n + vlen + 2) * 2;
				out = memrealloc(out, cap);
			}
			memcpy(out + n, val, vlen);
			n += vlen;
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

static void
expand_file(char **file, int status)
{
	bool drop;
	char *e;

	if (!*file)
		return;
	e = expand_word(*file, status, &drop);
	free(*file);
	*file = e;
}

void
expand_pipeline(pipeline_t *pl, int status)
{
	for (int i = 0; i < pl->count; i++) {
		command_t *c = &pl->cmds[i];
		int k = 0;

		for (int j = 0; j < c->argc; j++) {
			bool drop;
			char *e = expand_word(c->argv[j], status, &drop);

			free(c->argv[j]);
			if (drop) {
				free(e);
				continue;
			}
			c->argv[k++] = e;
		}
		c->argc = k;
		c->argv[k] = NULL;
		expand_file(&c->in_file, status);
		expand_file(&c->out_file, status);
		expand_file(&c->err_file, status);
	}
}
