#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>

#define VAR_OPEN '\x01'
#define VAR_QOPEN '\x04'
#define VAR_CLOSE '\x02'

typedef struct {
	char **argv;
	int argc;
	int argv_cap;
	char *in_file;
	char *out_file;
	char *err_file;
	bool out_append;
	bool err_append;
	bool err_to_out;
} command_t;

typedef struct {
	command_t *cmds;
	int count;
	int cmds_cap;
	bool background;
} pipeline_t;

typedef enum {
	SEP_END,
	SEP_SEQ,
	SEP_AND,
	SEP_OR,
} sep_t;

/* seps[i] joins pipes[i] to pipes[i + 1] */
typedef struct {
	pipeline_t *pipes;
	sep_t *seps;
	int count;
} list_t;

/* returns 0 on success (count may be 0 for empty line), -1 on syntax error */
int parse_list(const char *line, list_t *l);
void free_list(list_t *l);
/* run-time expansion of $VAR, ${VAR}, $?, $$ markers left by the parser */
void expand_pipeline(pipeline_t *pl, int status);
void free_pipeline(pipeline_t *pl);
bool command_has_redirect(const command_t *c);

#endif
