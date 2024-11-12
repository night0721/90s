#ifndef S_H_
#define S_H_

#define VERSION "1.0.0"

#include <stdio.h>

void *memalloc(size_t size);
char **argsplit(char *line);

#endif
