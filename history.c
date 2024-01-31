#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    int env_home_len = strlen(env_home);
    int histfilename_len = strlen(HISTFILE);
    int path_len = env_home_len + histfilename_len + 2; // 2 for slash and null byte
    histfile_path = malloc(sizeof(char) * path_len);
    // concatenate home and history file name to a path
    strcat(histfile_path, env_home);
    strcat(histfile_path, "/");
    strcat(histfile_path, HISTFILE);
    histfile_path[path_len] = '\0';
    if (access(histfile_path, F_OK) != 0) { // check for file existence
        history_file = fopen(histfile_path, "w"); // read and write, if doesn't exist, create
        if (history_file == NULL) {
            fprintf(stderr, "rush: Error opening history file\n");
            exit(EXIT_FAILURE);
        }
        fclose(history_file);
    }
}

void save_command_history(char *command) {
    history_file = fopen(histfile_path, "a+");
    if (history_file == NULL) {
        fprintf(stderr, "rush: Error opening history file\n");
        exit(EXIT_FAILURE);
    }
    int cmd_len = strlen(command);
    command[cmd_len] = '\n'; // put new line feed to split commands
    // ptr to first obj, size of each obj, number of obj, file ptr
    fwrite(command, sizeof(char), cmd_len + 1, history_file);
    fclose(history_file);
}

char *read_command(int direction) {
    history_file = fopen(histfile_path, "rb"); // read binary mode
    if (history_file == NULL) {
        fprintf(stderr, "rush: Error opening history file\n");
        exit(EXIT_FAILURE);
    }
    // normal bufsize is 1024, we serach for 1025 bytes for new line feed
    int search_len = RL_BUFSIZE + 1;
    char search[search_len];
    fseek(history_file, -search_len, SEEK_END); // go back 1025 characters from end of file
    int count = fread(search, 1, search_len - 1, history_file); // try to read 1025 characters from file, returning count number of bytes
    search[count] = '\0';
    char *last_nlf = strrchr(search, '\n'); // locate last occurence of \n in a searching string
    if (last_nlf == NULL) {
        // no history
        return NULL;
    }
    if (direction == 1) { // up
        cmd_count++;
    } else { // down
        if (cmd_count == 0) {
            return NULL;
        } else {
            cmd_count--;
        }
    }
    for (int i = 0; i < cmd_count; i++) {
        search[last_nlf - search] = '\0'; // terminate string earlier to find second last \n, search points to first char and last_nlf is last \n, difference is the index of \n
        last_nlf = strrchr(search, '\n'); // call strrchr 2 times to get second last new line feed in search string as every life is new line feed
        if ((last_nlf - search) == (strchr(search, '\n') - search)) {
            // check if the first \n is the last \n we searching for, if yes it is first command
            cmd_count--;
            search[last_nlf - search] = '\0'; // terminate string earlier to find second last \n, search points to first char and last_nlf is last \n, difference is the index of \n
            char *first_cmd = malloc(sizeof(char) * (last_nlf - search) + 1);
            strcpy(first_cmd, search);
            return first_cmd;
        }
    }
    fclose(history_file);
    return last_nlf + 1; // return the string from the new line feed
}
