#ifndef JOBS_H
#define JOBS_H

#include <stdbool.h>
#include <sys/types.h>

typedef struct job job_t;

void jobs_init(void);
job_t *job_new(const char *command);
pid_t job_pgid(const job_t *j);
void job_add_proc(job_t *j, pid_t pid);
void job_child_setup(const job_t *j, bool foreground);
void job_discard(job_t *j);
void job_foreground(job_t *j, bool cont);
void job_background(job_t *j);
void reap_children(void);

int jobs(char **args);
int fg(char **args);
int bg(char **args);

#endif
