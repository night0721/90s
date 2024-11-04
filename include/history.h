#ifndef HISTORY_H_
#define HISTORY_H_

extern int cmd_count;

void save_command_history(char *args);
void check_history_file(void);
char *read_command(int direction);
char **get_all_history(bool check);

#endif
