#ifndef HISTORY_H_
#define HISTORY_H_

void save_command_history(char *command);
void check_history_file();
char *read_command(int direction);

#endif
