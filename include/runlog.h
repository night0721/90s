#ifndef RUNLOG_H
#define RUNLOG_H

void runlog_append(const char *cmd, long dur_ms, int code, const char *cwd);
void cdhist_append(const char *path);
char *histgrep_pick(void);

int runlog(char **args);
int histgrep(char **args);
int cdhist(char **args);

#endif
