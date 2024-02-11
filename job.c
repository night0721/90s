#include <unistd.h>
#include <stdbool.h>
#include <string.h>

#include "rush.h"

typedef struct job {
    pid_t pid;
    char *command;
    bool status;
    struct job *next;
} job;

job *jobs = NULL;

int num_jobs() {
    job *current = jobs;
    int count = 0;
    while (current != NULL) {
        count++;
        current = current->next;
    }
    return count;
}

int add_job(pid_t pid, char *command, bool status) {
    job *current = jobs;
    job *new_job = memalloc(sizeof(job));
    new_job->pid = pid;
    char *buf = memalloc(strlen(command) + 1);
    strcpy(buf, command);
    new_job->command = buf;
    new_job->status = status;
    new_job->next = NULL;
    if (current == NULL) {
        jobs = new_job;
        return 0;
    }
    int index = 1;
    while (current->next != NULL) {
        current = current->next;
        index++;
    }
    current->next = new_job;
    return index;
}

job *get_job(int index) {
    job *current = jobs;
    if (index == 0) {
        return current;
    }
    if (index > num_jobs()) {
        return NULL;
    }
    for (int i = 0; i < index; i++) {
        printf("current: %s\n", current->command);
        current = current->next;
        printf("next: %s\n", current->command);
    }
    return current;
}
