#define _XOPEN_SOURCE 700

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "90s.h"
#include "jobs.h"

#define JOBS_MAX 64

extern int last_exit_code;
void change_terminal_attribute(int option);

typedef enum {
	PROC_RUNNING,
	PROC_STOPPED,
	PROC_DONE,
} proc_state_t;

typedef struct {
	pid_t pid;
	proc_state_t state;
	int status;
} proc_t;

struct job {
	int id;
	pid_t pgid;
	char *command;
	proc_t *procs;
	int nprocs;
	int cap;
	bool background;
	bool notified;
	unsigned seq;
	struct termios tmodes;
	bool has_tmodes;
};

static job_t *table[JOBS_MAX];
static pid_t shell_pgid;
static bool interactive;
static unsigned seq_counter;

void
jobs_init(void)
{
	interactive = isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) != -1;
	if (!interactive)
		return;
	while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
		kill(-shell_pgid, SIGTTIN);
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	if (setpgid(getpid(), getpid()) == 0)
		shell_pgid = getpid();
	tcsetpgrp(STDIN_FILENO, shell_pgid);
}

static char *
dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = memalloc(n);

	memcpy(p, s, n);
	return p;
}

static bool
job_has_running(const job_t *j)
{
	for (int i = 0; i < j->nprocs; i++) {
		if (j->procs[i].state == PROC_RUNNING)
			return true;
	}
	return false;
}

static bool
job_all_done(const job_t *j)
{
	for (int i = 0; i < j->nprocs; i++) {
		if (j->procs[i].state != PROC_DONE)
			return false;
	}
	return true;
}

static bool
job_is_stopped(const job_t *j)
{
	return !job_all_done(j) && !job_has_running(j);
}

static int
proc_code(const proc_t *p)
{
	if (WIFEXITED(p->status))
		return WEXITSTATUS(p->status);
	if (WIFSIGNALED(p->status))
		return 128 + WTERMSIG(p->status);
	return 1;
}

static int
job_code(const job_t *j)
{
	return j->nprocs > 0 ? proc_code(&j->procs[j->nprocs - 1]) : 0;
}

static void
job_remove(job_t *j)
{
	for (int i = 0; i < JOBS_MAX; i++) {
		if (table[i] == j)
			table[i] = NULL;
	}
	free(j->command);
	free(j->procs);
	free(j);
}

static void
job_update(pid_t pid, int st)
{
	for (int i = 0; i < JOBS_MAX; i++) {
		job_t *j = table[i];

		if (!j)
			continue;
		for (int k = 0; k < j->nprocs; k++) {
			if (j->procs[k].pid != pid)
				continue;
			if (WIFSTOPPED(st)) {
				j->procs[k].state = PROC_STOPPED;
			} else {
				j->procs[k].state = PROC_DONE;
				j->procs[k].status = st;
			}
			return;
		}
	}
}

static void
job_mark_running(job_t *j)
{
	for (int i = 0; i < j->nprocs; i++) {
		if (j->procs[i].state == PROC_STOPPED)
			j->procs[i].state = PROC_RUNNING;
	}
}

job_t *
job_new(const char *command)
{
	job_t *j;
	int i;

	for (i = 0; i < JOBS_MAX && table[i]; i++)
		;
	if (i == JOBS_MAX)
		return NULL;
	j = memalloc(sizeof(*j));
	memset(j, 0, sizeof(*j));
	j->id = i + 1;
	j->command = dup_str(command);
	j->cap = 4;
	j->procs = memalloc(sizeof(proc_t) * j->cap);
	table[i] = j;
	return j;
}

void
job_discard(job_t *j)
{
	job_remove(j);
}

pid_t
job_pgid(const job_t *j)
{
	return j->pgid;
}

void
job_add_proc(job_t *j, pid_t pid)
{
	if (j->nprocs == 0)
		j->pgid = pid;
	if (j->nprocs >= j->cap) {
		j->cap *= 2;
		j->procs = memrealloc(j->procs, sizeof(proc_t) * j->cap);
	}
	j->procs[j->nprocs].pid = pid;
	j->procs[j->nprocs].state = PROC_RUNNING;
	j->procs[j->nprocs].status = 0;
	j->nprocs++;
	setpgid(pid, j->pgid);
}

/* runs in the child, before it touches stdin; the parent does the same calls to avoid the race */
void
job_child_setup(const job_t *j, bool foreground)
{
	setpgid(0, j->pgid);
	if (foreground && interactive && j->pgid == 0)
		tcsetpgrp(STDIN_FILENO, getpid());
}

static job_t *
nth_recent(int n)
{
	job_t *best = NULL, *second = NULL;

	for (int i = 0; i < JOBS_MAX; i++) {
		job_t *j = table[i];

		if (!j || j->seq == 0)
			continue;
		if (!best || j->seq > best->seq) {
			second = best;
			best = j;
		} else if (!second || j->seq > second->seq) {
			second = j;
		}
	}
	return n == 0 ? best : second;
}

static void
print_job(const job_t *j, const char *state)
{
	char mark = j == nth_recent(0) ? '+' : j == nth_recent(1) ? '-' : ' ';

	printf("[%d]%c %-10s %s%s\n", j->id, mark, state, j->command,
			strcmp(state, "Running") == 0 ? " &" : "");
}

static job_t *
job_find(const char *spec)
{
	if (!spec || spec[0] == '\0')
		return nth_recent(0);
	if (spec[0] == '%')
		spec++;
	if (spec[0] == '\0' || strcmp(spec, "+") == 0 || strcmp(spec, "%") == 0)
		return nth_recent(0);
	if (strcmp(spec, "-") == 0)
		return nth_recent(1);
	if (spec[0] >= '0' && spec[0] <= '9') {
		int id = atoi(spec);

		for (int i = 0; i < JOBS_MAX; i++) {
			if (table[i] && table[i]->id == id)
				return table[i];
		}
	}
	return NULL;
}

void
job_foreground(job_t *j, bool cont)
{
	int st;
	pid_t pid;

	j->background = false;
	j->notified = false;
	if (interactive)
		tcsetpgrp(STDIN_FILENO, j->pgid);
	if (cont) {
		if (j->has_tmodes)
			tcsetattr(STDIN_FILENO, TCSADRAIN, &j->tmodes);
		job_mark_running(j);
		kill(-j->pgid, SIGCONT);
	}

	while (job_has_running(j)) {
		pid = waitpid(-j->pgid, &st, WUNTRACED);
		if (pid < 0) {
			if (errno == EINTR)
				continue;
			for (int i = 0; i < j->nprocs; i++) {
				if (j->procs[i].state == PROC_RUNNING)
					j->procs[i].state = PROC_DONE;
			}
			break;
		}
		job_update(pid, st);
	}

	if (interactive)
		tcsetpgrp(STDIN_FILENO, shell_pgid);

	if (job_all_done(j)) {
		last_exit_code = job_code(j);
		if (j->nprocs > 0 && WIFSIGNALED(j->procs[j->nprocs - 1].status)
				&& WTERMSIG(j->procs[j->nprocs - 1].status) == SIGINT)
			putchar('\n');
		job_remove(j);
	} else {
		tcgetattr(STDIN_FILENO, &j->tmodes);
		j->has_tmodes = true;
		j->background = true;
		j->notified = true;
		j->seq = ++seq_counter;
		putchar('\n');
		print_job(j, "Stopped");
		last_exit_code = 128 + SIGTSTP;
	}
	change_terminal_attribute(1);
}

void
job_background(job_t *j)
{
	j->background = true;
	j->notified = false;
	j->seq = ++seq_counter;
	printf("[%d] %d\n", j->id, (int)j->pgid);
}

/* called once per prompt: collect state changes, report finished and stopped background jobs */
void
reap_children(void)
{
	int st;
	pid_t pid;

	while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0)
		job_update(pid, st);

	for (int i = 0; i < JOBS_MAX; i++) {
		job_t *j = table[i];

		if (!j)
			continue;
		if (job_all_done(j)) {
			int code = job_code(j);
			char state[24];

			if (code == 0)
				snprintf(state, sizeof(state), "Done");
			else
				snprintf(state, sizeof(state), "Exit %d", code);
			print_job(j, state);
			job_remove(j);
		} else if (job_is_stopped(j) && !j->notified) {
			j->notified = true;
			print_job(j, "Stopped");
		}
	}
}

int
jobs(char **args)
{
	(void) args;
	for (int i = 0; i < JOBS_MAX; i++) {
		job_t *j = table[i];

		if (!j)
			continue;
		print_job(j, job_is_stopped(j) ? "Stopped" : "Running");
	}
	return 1;
}

int
fg(char **args)
{
	job_t *j = job_find(args[1]);

	if (!j) {
		fprintf(stderr, "90s: fg: no such job\n");
		return -1;
	}
	printf("%s\n", j->command);
	fflush(stdout);
	change_terminal_attribute(0);
	job_foreground(j, true);
	return 1;
}

int
bg(char **args)
{
	job_t *j = job_find(args[1]);

	if (!j) {
		fprintf(stderr, "90s: bg: no such job\n");
		return -1;
	}
	if (!job_is_stopped(j)) {
		fprintf(stderr, "90s: bg: job %d is already running\n", j->id);
		return -1;
	}
	job_mark_running(j);
	j->background = true;
	j->notified = false;
	j->seq = ++seq_counter;
	kill(-j->pgid, SIGCONT);
	print_job(j, "Running");
	return 1;
}
