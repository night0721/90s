#ifndef CONSTANTS_H_
#define CONSTANTS_H_

#define HISTFILE ".rush_history" // history file name
#define TOK_BUFSIZE 64 // buffer size of each token
#define RL_BUFSIZE 1024 // size of each command
#define TOK_DELIM " \t\r\n\a" // delimiter for token
#define MAX_HISTORY 8192 // maximum lines of reading history

#define MAX_JOBS 64 // maximum number of jobs
#define OPT_STDIN 0x01 // option for stdin
#define OPT_STDOUT 0x02 // option for stdout
#define OPT_STDERR 0x04 // option for stderr
#define OPT_FGJ 0x08 // option for foreground job
#define OPT_BGJ 0x10 // option for background job
#endif
