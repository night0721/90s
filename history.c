#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <linux/limits.h>

#include "history.h"
#include "constants.h"

FILE *history_file;
char *histfile_path;
int cmd_count = 0;

void check_history_file() {
    char *env_home;
    env_home = getenv("XDG_CONFIG_HOME");
    if (env_home == NULL) {
        // fallback to $HOME if $XDG_CONFIG_HOME is null
        env_home = getenv("HOME");
    }
    if (env_home == NULL) {
        fprintf(stderr, "rush: HOME AND XDG_CONFIG_HOME environment variable is missing\n");
        exit(EXIT_FAILURE);
    }
    int env_home_len = strlen(env_home);
    int histfilename_len = strlen(HISTFILE);
    int path_len = env_home_len + histfilename_len + 2; // 2 for slash and null byte
    histfile_path = malloc(sizeof(char) * path_len);
    if (histfile_path == NULL) {
        fprintf(stderr, "rush: Error allocating memory\n");
        exit(EXIT_FAILURE);
    }
    histfile_path[0] = '\0'; // initialise string
    // concatenate home and history file name to a path
    strcat(histfile_path, env_home);
    strcat(histfile_path, "/");
    strcat(histfile_path, HISTFILE);
    histfile_path[path_len - 1] = '\0';
    if (access(histfile_path, F_OK) != 0) { // check for file existence
        history_file = fopen(histfile_path, "w"); // read and write, if doesn't exist, create
        if (history_file == NULL) {
            fprintf(stderr, "rush: Error opening history file\n");
            exit(EXIT_FAILURE);
        }
        fclose(history_file);
    }
}

void save_command_history(char **args) {
    history_file = fopen(histfile_path, "a+");
    if (history_file == NULL) {
        fprintf(stderr, "rush: Error opening history file\n");
        exit(EXIT_FAILURE);
    }
    char cmd[RL_BUFSIZE];
    cmd[0] = '\0';
    for (int i = 0; args[i] != NULL; ++i) {
        strcat(cmd, args[i]);
        strcat(cmd, " ");
    }
    int cmd_len = strlen(cmd);
    cmd[cmd_len] = '\n'; // put new line feed to split commands
    // ptr to first obj, size of each obj, number of obj, file ptr
    fwrite(cmd, sizeof(char), cmd_len + 1, history_file);
    fclose(history_file);
}

char *read_command(int direction) {
    if (direction == 1) { // up
        cmd_count++;
    } else { // down
        if (cmd_count == 0) {
            return NULL;
        } else {
            cmd_count--;
        }
    }
    char **history = get_all_history(false);
    int num_history = 0;
    while (*history != NULL) {
        num_history++;
        history++;
    }
    if (cmd_count > num_history) {
        cmd_count = num_history;
        return NULL;
    }
    history -= num_history;
    return history[num_history - cmd_count];
}

int is_duplicate(char **history, int line_count, char *line) {
    for (int i = 0; i < line_count; ++i) {
        if (strcmp(history[i], line) == 0) {
            return 1;
        }
    }
    return 0;
}

char **get_all_history(bool check) {
    history_file = fopen(histfile_path, "r");
    if (history_file == NULL) {
        fprintf(stderr, "rush: Error opening history file\n");
        exit(EXIT_FAILURE);
    }

    char **history = malloc(MAX_HISTORY * sizeof(char*));
    if (history == NULL) {
        fprintf(stderr, "rush: Error allocating memory\n");
        exit(EXIT_FAILURE);
    }
    char buffer[RL_BUFSIZE];
    int line_count = 0;

    while (fgets(buffer, sizeof(buffer), history_file) != NULL) {
        buffer[strcspn(buffer, "\n")] = '\0';
        if (check) {
            if (!is_duplicate(history, line_count, buffer)) {
                history[line_count] = strdup(buffer);
                if (history[line_count] == NULL) {
                    fprintf(stderr, "Error allocating memory\n");
                    exit(EXIT_FAILURE);
                }
                line_count++;
                if (line_count >= MAX_HISTORY) {
                    fprintf(stderr, "Maximum number of lines reached.\n");
                    exit(EXIT_FAILURE);
                }
            }
        } else {
            history[line_count] = strdup(buffer);
            if (history[line_count] == NULL) {
                fprintf(stderr, "Error allocating memory\n");
                exit(EXIT_FAILURE);
            }
            line_count++;
            if (line_count >= MAX_HISTORY) {
                fprintf(stderr, "Maximum number of lines reached.\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    fclose(history_file);
    history[line_count] = NULL;
    return history;
}
