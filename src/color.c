#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "90s.h"

// color str in place
char *color_text(char *str, const char *color) {
    int size = snprintf(NULL, 0, "\x1b[38;2;%sm%s\x1b[0m", color, str) + 1; // calculate size that is needed for colored string
    if (size < 0) {
        fprintf(stderr, "90s: snprintf failed\n");
        exit(EXIT_FAILURE);
    }
    char *buf = memalloc(size);

    snprintf(buf, size, "\x1b[38;2;%sm%s\x1b[0m", color, str); // format string to buf
    return buf;
}
