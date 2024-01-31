#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>

#include "color.h"
#include "constants.h"
#include "history.h"

/*
 * Function Declarations for builtin shell commands:
 */
int rush_cd(char **args);
int help(char **args);
int rush_exit(char **args);


/*
 * List of builtin commands, followed by their corresponding functions.
 */
char *builtin_str[] = {
    "cd",
    "help",
    "exit"
};

int (*builtin_func[]) (char **) = {
    &rush_cd,
    &help,
    &rush_exit
};

int rush_num_builtins() {
    return sizeof(builtin_str) / sizeof(char *);
}

// change directory
int rush_cd(char **args) {
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

    for (int i = 0; i < rush_num_builtins(); i++) {
        printf("  %s\n", builtin_str[i]);
    }

    printf("Use 'man' to read manual of programs\n");
    printf("Licensed under GPL v3\n");
    return 1;
}

int rush_exit(char **args) {
    return 0; // exit prompting loop, which also the shell
}

/**
  @brief Launch a program and wait for it to terminate.
  @param args Null terminated list of arguments (including program).
  @return Always returns 1, to continue execution.
  */
int rush_launch(char **args) {
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

/**
  @brief Execute shell built-in or launch program.
  @param args Null terminated list of arguments.
  @return 1 if the shell should continue running, 0 if it should terminate
  */
int rush_execute(char **args) {
    if (args[0] == NULL) {
        // An empty command was entered.
        return 1;
    }

    for (int i = 0; i < rush_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }

    return rush_launch(args);
}

void change_terminal_attribute(int option) {  
    static struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    if (option) {
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO); // allows getchar without pressing enter key and echoing the character twice
        tcsetattr(STDIN_FILENO, TCSANOW, &newt); // set settings to stdin
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // restore to old settings
    }
}  

char **setup_path_variable() {
    char *envpath = getenv("PATH");
    char *path_cpy = malloc(sizeof(char) * (strlen(envpath) + 1));
    char *path = malloc(sizeof(char) * (strlen(envpath) + 1));
    strcpy(path_cpy, envpath);
    strcpy(path, envpath);
    int path_count = 0;
    while (*path_cpy != '\0') {
        // count number of : to count number of elements
        if (*path_cpy == ':') {
            path_count++;
        }
        path_cpy++;
    }
    path_count += 2; // adding one to be correct and one for terminator
    char **paths = malloc(sizeof(char *) * path_count);
    char *token = strtok(path, ":");
    int counter = 0;
    while (token != NULL) {
        paths[counter] = token; // set element to the pointer of start of path
        token = strtok(NULL, ":");
        counter++;
    }
    paths[counter] = NULL;
    return paths;
}

bool find_command(char **paths, char *command) {
    if (strcmp(command, "") == 0) {
        return false;
    }
    int counter = 0;
    while (*paths != NULL) {
        char current_path[PATH_MAX];
        sprintf(current_path, "%s/%s", *paths, command);
        if (access(current_path, X_OK) == 0) {
            // command is executable
            return true;
        }
        paths++;
    }
    return false;
}

char *rush_read_line(char **paths) {
    int bufsize = RL_BUFSIZE;
    int position = 0;
    char *buffer = malloc(sizeof(char) * bufsize);
    int c;

    if (!buffer) {
        fprintf(stderr, "rush: allocation error\n");
        exit(EXIT_FAILURE);
    }

    buffer[0] = '\0';
    while (1) {
        c = getchar(); // read a character
        int buf_len = strlen(buffer);
        if (buf_len > 0) {
            printf("\033[%dD", strlen(buffer));  // move cursor to the beginning
            printf("\033[K"); // clear line to the right of cursor
        }
        // check each character user has input
        // printf("%i\n", c);
        switch (c) {
            case EOF:
                exit(EXIT_SUCCESS);
            case 10: // enter/new line feed
                buffer[buf_len] = '\0';
                // clear all characters after the command
                for (int start = buf_len + 1; buffer[start] != '\0'; start++) {
                    buffer[start] = '\0';
                }
                printf("%s\n", buffer); // print back the command in prompt
                save_command_history(buffer);
                return buffer;
            case 127: // backspace
                if (buf_len >= 1) {
                    buffer[buf_len - 1] = '\0'; // putting null character at last character to act as backspace
                }
                break;
            case 27: // arrow keys comes at three characters, 27, 91, then 65-68
                if (getchar() == 91) {
                    int arrow_key = getchar();
                    if (arrow_key == 65) { // up
                        // read history file and fill prompt with latest command
                        char *last_command = read_command(1);
                        if (last_command != NULL) {
                            strcpy(buffer, last_command);
                            buf_len = strlen(buffer);
                        }
                        break;
                    } else if (arrow_key == 66) { // down
                        char *last_command = read_command(0);
                        if (last_command != NULL) {
                            strcpy(buffer, last_command);
                            buf_len = strlen(buffer);
                        }
                        break;
                    } else if (arrow_key == 67) { // right
                        break;
                    } else if (arrow_key == 68) { // left
                        break;
                    }
                }
            default:
                if (c > 31 && c < 127) {
                    buffer[buf_len] = c;
                    buffer[buf_len + 1] = '\0'; // make sure printf don't print random characters
                }
        }
        char *cmd_part = strchr(buffer, ' ');
        bool valid;
        if (cmd_part != NULL) {
            char *cmd = malloc(sizeof(char) * (cmd_part - buffer + 1));
            for (int i = 0; i < (cmd_part - buffer); i++) {
                cmd[i] = buffer[i];
            }
            cmd[cmd_part - buffer] = '\0';
            valid = find_command(paths, cmd);
        } else {
            valid = find_command(paths, buffer);
        }
        if (valid) {
            printf("\x1b[38;2;000;255;000m%s\x1b[0m", buffer); // print green as valid command
        } else {
            printf("\x1b[38;2;255;000;000m%s\x1b[0m", buffer); // print red as sinvalid command
        }
        fflush(stdout);

        // If we have exceeded the buffer, reallocate.
        if ((buf_len + 1) >= bufsize) {
            bufsize += RL_BUFSIZE;
            buffer = realloc(buffer, bufsize);
            if (!buffer) {
                fprintf(stderr, "rush: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }
    }
}

// split line into arguments
char **rush_split_line(char *line) {
    int bufsize = TOK_BUFSIZE, position = 0;
    char **tokens = malloc(bufsize * sizeof(char*));
    char *token;

    if (!tokens) {
        fprintf(stderr, "rush: Allocation error\n");
        exit(EXIT_FAILURE);
    }

    token = strtok(line, TOK_DELIM);
    while (token != NULL) {
        tokens[position] = token;
        position++;

        if (position >= bufsize) {
            bufsize += TOK_BUFSIZE;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "rush: Allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, TOK_DELIM);
    }
    tokens[position] = NULL;
    return tokens;
}

// continously prompt for command and execute it
void command_loop(char **paths) {
    char *line;
    char **args;
    int status = 1;

    while (status) {
        time_t t = time(NULL);
        struct tm* current_time = localtime(&t); // get current time
        char timestr[256];
        char cwdstr[PATH_MAX];
        if (strftime(timestr, sizeof(timestr), "[%H:%M:%S]", current_time) == 0) { // format time string
            return;
        }
        if (getcwd(cwdstr, sizeof(cwdstr)) == NULL) { // get current working directory
            return;
        }
        char time[256];
        strcpy(time, timestr);
        color_text(time, lavender); // lavender colored time string
        char *cwd = malloc(sizeof(char) * PATH_MAX);
        sprintf(cwd, "[%s]", cwdstr);
        color_text(cwd, pink); // pink colored current directory
        char arrow[32] = "»";
        color_text(arrow, blue);
        printf("%s %s %s ", time, cwd, arrow);


        line = rush_read_line(paths);
        args = rush_split_line(line);
        status = rush_execute(args);

        free(line);
        free(args);
        free(cwd);
    };
}


int main(int argc, char **argv) {
    // setup
    check_history_file();
    char **paths = setup_path_variable();
    change_terminal_attribute(1); // turn off echoing and disabling getchar requires pressing enter key to return

    command_loop(paths);

    // cleanup
    change_terminal_attribute(0); // change back to default settings
    return EXIT_SUCCESS;
}   
