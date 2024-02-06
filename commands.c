#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/wait.h>

#include "commands.h"
#include "constants.h"
#include "history.h"
#include "rush.h"

// Function declarations for builtin commands
int cd(char **args);
int help(char **args);
int quit(char **args);
int history(char **args);
int export(char **args);
int source(char **args);

// List of builtin commands' names
char *builtin_cmds[] = {
    "cd",
    "help",
    "exit",
    "history",
    "export",
    "source"
};

int (*builtin_func[]) (char **) = {
    &cd,
    &help,
    &quit, // cant name it exit as it is taken
    &history,
    &export,
    &source
};

// number of built in commands
int num_builtins() {
    return sizeof(builtin_cmds) / sizeof(char *);
}

// change directory
int cd(char **args) {
    if (args[1] == NULL) {
        char *home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "rush: HOME environment variable is missing\n");
            exit(EXIT_FAILURE);
        }
        if (chdir(home) != 0) {
            perror("rush");
        }
    } else {
        if (chdir(args[1]) != 0) {
            perror("rush");
        }
    }
    return 1;
}

// show help menu
int help(char **args) {
    printf("rush v0.01-alpha\n");
    printf("Built in commands:\n");

    for (int i = 0; i < num_builtins(); i++) {
        printf("  %s\n", builtin_cmds[i]);
    }

    printf("Use 'man' to read manual of programs\n");
    printf("Licensed under GPL v3\n");
    return 1;
}

int quit(char **args) {
    return 0; // exit prompting loop, which also the shell
}

int history(char **args) {
    char **history = get_all_history();

    for (int i = 0; history[i] != NULL; ++i) {
        printf("%s\n", history[i]);
        free(history[i]);
    }

    free(history);
    return 1;
}

int export(char **args) {
    args++; // skip the command
    while (*args != NULL) {
        char *variable = strtok(*args, "=\n");
        char *value = strtok(NULL, "=\n");
        if (variable != NULL && value != NULL) {
            if (setenv(variable, value, 1) != 0) {
                fprintf(stderr, "rush: Error setting environment variable\n");
                return 0;
            }
        } else {
            printf("rush: [arg0: %s] [arg1: %s]", args[0], args[1]);
            printf("rush: [Variable: %s] [Value: %s]\n", variable, value);
            fprintf(stderr, "rush: Syntax error when setting environment variable\nUse \"export VARIABLE=VALUE\"\n");
            return 0;
        }
        args++;
    }
    return 1;
}

int source(char **args) {
    if (args[1] == NULL) {
        fprintf(stderr, "rush: not enough arguments\n");
        return -1;
    }

    FILE *file = fopen(args[1], "r");

    if (file == NULL) {
        fprintf(stderr, "rush: no such file or directory '%s'\n", args[1]);
        return -1;
    }

    char line[RL_BUFSIZE];
    int status;
    while (fgets(line, sizeof(line), file) != NULL) {
        // Remove newline character if present
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        char **args = argsplit(line);
        status = execute(args);
    }

    fclose(file);
    return status; // Indicate success
}

bool is_builtin(char *command) {
    for (int i = 0; i < num_builtins(); i++) {
        if (strcmp(command, builtin_cmds[i]) == 0) {
            return true;
        }
    }
    return false;
}

// execute built in commands or launch commands and wait it to terminate, return 1 to keep shell running
int execute(char **args) {
    if (args[0] == NULL) { // An empty command was entered.
        return 1;
    }

    for (int i = 0; i < num_builtins(); i++) {
        if (strcmp(args[0], builtin_cmds[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }

    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid == 0) {
        // Child process
        if (execvp(args[0], args) == -1) {
            if (errno == ENOENT) {
                fprintf(stderr, "rush: command not found: %s\n", args[0]);
            }
        }
        exit(EXIT_FAILURE); // exit the child
    } else if (pid < 0) {
        perror("fork failed");
    } else {
        // Parent process
        while (!WIFEXITED(status) && !WIFSIGNALED(status)) {
            wpid = waitpid(pid, &status, WUNTRACED); // wait child to be exited to return to prompt
        }
    }

    return 1;
}
