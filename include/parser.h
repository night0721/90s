#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>

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
	bool background;
} pipeline_t;

/* returns 0 on success (count may be 0 for empty line), -1 on syntax error */
int parse_line(const char *line, pipeline_t *pl);
void free_pipeline(pipeline_t *pl);
bool command_has_redirect(const command_t *c);

#endif
