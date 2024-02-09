#ifndef COMMANDS_H_
#define COMMANDS_H_

int num_builtins();
bool is_builtin(char *command);
int execute(char **args);
int execute_pipe(char ***args);

#endif
