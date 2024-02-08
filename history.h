#ifndef HISTORY_H_
#define HISTORY_H_

void save_command_history(char **args);
void check_history_file();
char *read_command(int direction);
char **get_all_history(bool check);

#endif
