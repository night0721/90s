#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <time.h>
#include <stdbool.h>
#include <signal.h>
#include <ctype.h>

#include "color.h"
#include "constants.h"
#include "history.h"
#include "commands.h"

void *memalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fputs("rush: Error allocating memory\n", stderr);
        exit(EXIT_FAILURE);
    }
    return ptr;
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
    if (envpath == NULL) {
        fprintf(stderr, "rush: PATH environment variable is missing\n");
        exit(EXIT_FAILURE);
    }
    char *path_cpy = memalloc(sizeof(char) * (strlen(envpath) + 1));
    char *path = memalloc(sizeof(char) * (strlen(envpath) + 1));
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
    char **paths = memalloc(sizeof(char *) * path_count);
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
    if (strncmp(command, "", 1) == 0) {
        return false;
    }
    while (*paths != NULL) {
        char current_path[PATH_MAX];
        current_path[0] = '\0';
        sprintf(current_path, "%s/%s", *paths, command);
        if (access(current_path, X_OK) == 0) {
            // command is executable
            return true;
        } else {
            if (is_builtin(command)) {
                return true;
            }
        }
        paths++;
    }
    return false;
}

void shiftleft(int chars) {
    printf("\033[%dD", chars);
}

void shiftright(int chars) {
    printf("\033[%dC", chars);
}

void clearline() {
    printf("\033[K"); // clear line to the right of cursor
}

void highlight(char *buffer, char **paths) {
    char *cmd_part = strchr(buffer, ' ');
    char *command_without_arg = NULL;
    int cmd_len = 0;
    bool valid;

    if (cmd_part != NULL) {
        cmd_len = cmd_part - buffer;
        char *cmd = memalloc(sizeof(char) * (cmd_len + 1));
        command_without_arg = memalloc(sizeof(char) * (cmd_len + 1));
        for (int i = 0; i < (cmd_part - buffer); i++) {
            cmd[i] = buffer[i];
        }
        strcpy(command_without_arg, cmd);
        cmd[cmd_len] = '\0';
        command_without_arg[cmd_len] = '\0';
        valid = find_command(paths, cmd);
        free(cmd);
    } else {
        valid = find_command(paths, buffer);
    }

    if (valid) {
        if (command_without_arg != NULL) {
            buffer += cmd_len;
            printf("\x1b[38;2;137;180;250m%s\x1b[0m\x1b[38;2;255;255;255m%s\x1b[0m", command_without_arg, buffer); // print green as valid command, but only color the command, not the arguments
            buffer -= cmd_len;
        } else {
            printf("\x1b[38;2;137;180;250m%s\x1b[0m", buffer); // print green as valid command
        }
    } else {
        if (command_without_arg != NULL) {
            buffer += cmd_len;
            printf("\x1b[38;2;243;139;168m%s\x1b[0m\x1b[38;2;255;255;255m%s\x1b[0m", command_without_arg, buffer); // print green as valid command, but only color the command, not the arguments
            buffer -= cmd_len;
        } else {
            printf("\x1b[38;2;243;139;168m%s\x1b[0m", buffer); // print red as invalid command
        }
    }
    free(command_without_arg);
}

char *readline(char **paths) {
    int bufsize = RL_BUFSIZE;
    int position = 0;
    char *buffer = memalloc(sizeof(char) * bufsize);

    bool moved = false;
    bool backspaced = false;
    bool navigated = false;
    bool insertatmiddle = false;
    bool replaced = false;

    buffer[0] = '\0';
    while (1) {
        int c = getchar(); // read a character
        int buf_len = strlen(buffer);
        
        // check each character user has input
        switch (c) {
            case EOF:
                exit(EXIT_SUCCESS);
            case 10: {
                // enter/new line feed
                if (buf_len == 0) {
                    return NULL;
                }
                // check if command includes !!
                if (strstr(buffer, "!!") != NULL) {
                    char *last_command = read_command(1);
                    if (last_command != NULL) {
                        // replace !! with the last command
                        char *replace = strstr(buffer, "!!");
                        int replace_len = strlen(replace);
                        int last_command_len = strlen(last_command);
                        int buffer_len = strlen(buffer);
                        if (last_command_len > replace_len) {
                            buffer = realloc(buffer, buffer_len + last_command_len - replace_len + 1);
                            if (!buffer) {
                                fprintf(stderr, "rush: Error allocating memory\n");
                                exit(EXIT_FAILURE);
                            }
                        }
                        memmove(replace + last_command_len, replace + replace_len, buffer_len - (replace - buffer) - replace_len + 1);
                        memcpy(replace, last_command, last_command_len);
                        position += last_command_len - replace_len;
                        shiftright(last_command_len - replace_len);
                        replaced = true;
                        break;
                    }
                } else {
                    buffer[buf_len] = '\0';
                    // clear all characters after the command
                    for (int start = buf_len + 1; buffer[start] != '\0'; start++) {
                        buffer[start] = '\0';
                    }
                    printf("\n"); // give space for response
                    position = 0;
                    return buffer;
                }
            }
            case 127: // backspace
                if (buf_len >= 1) {
                    position--;
                    for (int i = position; i < buf_len; i++) {
                        // shift the buffer
                        buffer[i] = buffer[i + 1];
                    }
                    backspaced = true;
                    moved = false;
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
                        }
                        navigated = true;
                        moved = false;
                        break;
                    } else if (arrow_key == 66) { // down
                        char *last_command = read_command(0);
                        if (last_command != NULL) {
                            strcpy(buffer, last_command);
                        }
                        navigated = true;
                        moved = false;
                        break;
                    } else if (arrow_key == 67) { // right
                        if (position < buf_len) {
                            shiftright(1);
                            position++;
                        }
                        moved = true;
                        break;
                    } else if (arrow_key == 68) { // left
                        if (position >= 1) {
                            shiftleft(1);
                            position--;
                        }
                        moved = true;
                        break;
                    } 
                }
            default:
                if (c > 31 && c < 127) {
                    if (position == buf_len) {
                        // Append character to the end of the buffer
                        buffer[buf_len] = c;
                        buffer[buf_len + 1] = '\0';
                        moved = false;
                        navigated = false;
                    } else {
                        // Insert character at the current position
                        memmove(&buffer[position + 1], &buffer[position], buf_len - position + 1);
                        buffer[position] = c;
                        shiftright(1);
                        insertatmiddle = true;
                    }
                    position++;
                }
        }

        buf_len = strlen(buffer);

        if (replaced) {
            shiftleft(buf_len);
            clearline();
        }

        if (navigated && buf_len >= 1) {
            if (position > 0) {
                shiftleft(position);  // move cursor to the beginning
                clearline();
            }
            position = buf_len;
        }

        if (moved) {
            moved = false;
            continue;
        }

        if (!navigated && !replaced) {
            if (position != buf_len) {
                // not at normal place
                if (backspaced) {
                    shiftleft(position + 1);
                } else {
                    shiftleft(position);  // move cursor to the beginning
                }
            } else if (buf_len > 1) {
                if (backspaced) {
                    shiftleft(buf_len + 1);  // move cursor to the beginning
                } else {
                    shiftleft(buf_len - 1);  // move cursor to the beginning
                }
            } else if (buf_len == 1) {
                if (backspaced) {
                    shiftleft(2);
                }
            } else if (buf_len == 0) {
                if (backspaced) {
                    shiftleft(1);    
                }
            }
            clearline();
        } else {
            navigated = false;
        }

        highlight(buffer, paths);

        if (backspaced) {
            if (buf_len != position) {
                shiftleft(buf_len - position);
            }
            backspaced = false;
        }
        if (insertatmiddle) {
            shiftleft(buf_len - position); // move cursor back to where it was
            insertatmiddle = false;
        }
        if (replaced) {
            replaced = false;
        }

        // If we have exceeded the buffer, reallocate.
        if ((buf_len + 1) >= bufsize) {
            bufsize += RL_BUFSIZE;
            buffer = realloc(buffer, bufsize);
            if (!buffer) {
                fprintf(stderr, "rush: Error allocating memory\n");
                exit(EXIT_FAILURE);
            }
        }
    }
}

// split line into arguments
char **argsplit(char *line) {
    int bufsize = TOK_BUFSIZE, position = 0;
    char **tokens = memalloc(bufsize * sizeof(char*));
    char *token;

    token = strtok(line, TOK_DELIM);
    while (token != NULL) {
        tokens[position] = token;
        position++;

        if (position >= bufsize) {
            bufsize += TOK_BUFSIZE;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "rush: Error allocating memory\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, TOK_DELIM);
    }
    tokens[position] = NULL;
    return tokens;
}

char **modifyargs(char **args) {
    int num_arg = 0;

    // check if command is ls, diff, or grep, if so, add --color=auto to the arguments
    // this is to make ls, diff, and grep have color without user typing it
    // this is to make the shell more user friendly
    while (args[num_arg] != NULL) {
        num_arg++;
    }
    for (int i = 0; i < num_arg; i++) {
        // makes ls and diff and grep have color without user typing it
        if (strncmp(args[i], "ls", 2) == 0 || strncmp(args[i], "diff", 4) == 0 || strncmp(args[i], "grep", 4) == 0) {
            for (int j = num_arg; j > i; j--) {
                args[j + 1] = args[j];
            }
            args[i + 1] = "--color=auto";
            num_arg++;
        }
    }

    return args;
}

char *trimws(char *str) {
	char *end;
	while (isspace((unsigned char) *str))
        str++;
	if(*str == 0)
		return str;
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char) *end))
        end--;
	*(end+1) = 0;
	return str;
}

char ***pipe_argsplit(char *line) {
    char ***cmdv = memalloc(sizeof(char **) * 128); // 127 commands, 1 for NULL
    char **cmds = memalloc(sizeof(char *) * 128); // 127 arguments, 1 for NULL
    int num_arg = 0;
    char *pipe = strtok(line, "|");
    while (pipe != NULL) {
        pipe = trimws(pipe);
        cmds[num_arg] = strdup(pipe);
        pipe = strtok(NULL, "|");
        num_arg++;
    }
    cmds[num_arg] = NULL;

    for (int i = 0; i < num_arg; i++) {
        char **splitted = argsplit(cmds[i]);
        cmdv[i] = modifyargs(splitted);

    }
    cmdv[num_arg] = NULL;
    free(cmds);
    return cmdv;
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
        char *modtime = memalloc(sizeof(char) * 256);
        modtime = color_text(time, lavender); // lavender colored time string
        char *cwd = memalloc(sizeof(char) * (PATH_MAX + 2));
        sprintf(cwd, "[%s]", cwdstr);
        cwd = replace_absolute_home(cwd);
        cwd = color_text(cwd, pink); // pink colored current directory
        char *arrow = memalloc(sizeof(char) * 32);
        strcpy(arrow, "»");
        arrow = color_text(arrow, blue);
        printf("%s %s %s ", modtime, cwd, arrow);

        cmd_count = 0; // upward arrow key resets command count
        line = readline(paths);
        if (line == NULL) {
            printf("\n");
            continue;
        }
        save_command_history(line);
        bool has_pipe = false;
        for (int i = 0; line[i] != '\0'; i++) {
            if (line[i] == '|') {
                has_pipe = true;
                break;
            }
        }
        if (has_pipe) {
            char ***pipe_args = pipe_argsplit(line);
            status = execute_pipe(pipe_args);
            while (*pipe_args != NULL) {
                free(*pipe_args);
                pipe_args++;
            }
        } else {
            args = argsplit(line);
            args = modifyargs(args);
            status = execute(args, STDOUT_FILENO, OPT_FGJ);
            free(args);
        }
        free(line);
        free(modtime);
        free(cwd);
        free(arrow);
    };
}

void quit_sig(int sig) {
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    // setup
    signal(SIGINT, quit_sig);
    signal(SIGTERM, quit_sig);
    signal(SIGQUIT, quit_sig);
    check_history_file();
    char **paths = setup_path_variable();
    change_terminal_attribute(1); // turn off echoing and disabling getchar requires pressing enter key to return

    command_loop(paths);

    // cleanup
    free(paths);
    change_terminal_attribute(0); // change back to default settings
    return EXIT_SUCCESS;
}   
