# rush

rush is a minimalistic shell for Unix systems written in C.

# Preview
[![Invalid command](https://nightly.502-bad-gateway.host/aqnpvvud.png)]
[![Valid command](https://nightly.502-bad-gateway.host/xf80dq0b.png)]

# Dependencies
- gcc

# Building
```sh
$ make
```

# Usage
```sh
$ ./rush
```

# Features
- Showing current time and directory with custom color
- Syntax highlighting on valid commands using ANSI colors
- History navigation using up and down keys with history command
- Built in commands
- Export environment variable
- Source commands from a file

# Built in commands
- cd
- help
- exit
- history
- export
- source

# Todo Features
- Pipe
- stdin, stdout, stderr redirect
- background jobs
- editing using left and right arrow keys
- tab completion

# Credits
- [Tutorial - Write a shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/)
- [dash](https://github.com/danishprakash/dash)
- [Shell assignment](https://www.cs.cornell.edu/courses/cs414/2004su/homework/shell/shell.html)
- [khol](https://github.com/SanketDG/khol/)
