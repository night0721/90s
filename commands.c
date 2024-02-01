#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/wait.h>

#include "commands.h"
#include "history.h"

// Function declarations for builtin commands
int cd(char **args);
int help(char **args);
int quit(char **args);
int history(char **args);

// List of builtin commands' names
char *builtin_cmds[] = {
    "cd",
    "help",
    "exit",
    "history"
};

int (*builtin_func[]) (char **) = {
    &cd,
    &help,
    &quit, // cant name it exit as it is taken
    &history
};

// number of built in commands
int num_builtins() {
    return sizeof(builtin_cmds) / sizeof(char *);
}

// change directory
int cd(char **args) {
    if (args[1] == NULL) {
        char *home = getenv("HOME");
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
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("Cannot fork");
    } else {
        // Parent process
        do {
            wpid = waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    return 1;
}
