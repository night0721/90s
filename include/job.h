#ifndef JOB_H_
#define JOB_H_

#include <stdbool.h>
#include <unistd.h>

typedef struct job {
	pid_t pid;
	char *command;
	bool status;
	struct job *next;
} job;

int add_job(pid_t pid, char *command, bool status);
job *get_job(pid_t pid);

#endif
