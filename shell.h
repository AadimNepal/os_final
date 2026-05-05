/*
 * shell.h - shared definitions and function prototypes for myshell
 *
 * Declares the Command struct used throughout the shell, along with the
 * prototypes for every module so each .c file can include this single header
 * instead of forward-declaring things manually.
 */

#ifndef SHELL_H
#define SHELL_H

/* ── compile-time limits ─────────────────────────────────────────────────── */

#define MAX_INPUT_LENGTH 1024   /* maximum characters the user may type       */
#define MAX_ARGS         64     /* maximum arguments per single command        */
#define MAX_PIPE_CMDS    16     /* maximum commands in one pipeline            */

/* ── data types ──────────────────────────────────────────────────────────── */

/*
 * Command - holds everything needed to execute one command segment.
 *
 * args[]      : null-terminated array passed directly to execvp(); args[0] is
 *               the program name, args[1..arg_count-1] are its arguments.
 * arg_count   : number of valid entries in args[] (not counting the NULL).
 * input_file  : filename for stdin  redirection (<),  or NULL if none.
 * output_file : filename for stdout redirection (>),  or NULL if none.
 * error_file  : filename for stderr redirection (2>), or NULL if none.
 */
typedef struct {
    char *args[MAX_ARGS];
    int   arg_count;
    char *input_file;
    char *output_file;
    char *error_file;
    int append_mode;
} Command;

typedef struct {
    Command cmds[MAX_PIPE_CMDS];
    int     num_commands;
} Pipeline;

#endif /* SHELL_H */
