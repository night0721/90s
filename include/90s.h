#ifndef S_H_
#define S_H_

#define VERSION "0.0.3-beta"
#include <stdio.h>

void *memalloc(size_t size);
void *memrealloc(void *old, size_t size);
void change_terminal_attribute(int option);

#endif
