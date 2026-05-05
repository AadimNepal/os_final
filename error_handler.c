/*
 * error_handler.c - centralised error reporting and input validation
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "error_handler.h"

/* system-level error: appends the OS error string */
void report_syserr(const char *message) {
    fprintf(stderr, "[ERROR] %s: %s\n", message, strerror(errno));
    fflush(stderr);
}

/* shell-level error: prints the message as-is */
void report_error(const char *message) {
    fprintf(stderr, "[ERROR] %s\n", message);
    fflush(stderr);
}

/* internal check: a command is valid if it has at least one non-empty argument */
static int cmd_has_name(const Command *cmd) {
    return cmd->arg_count > 0
        && cmd->args[0] != NULL
        && cmd->args[0][0] != '\0';
}

/* returns 0 if every command in the pipeline has a name, -1 otherwise */
int pipeline_valid(const Pipeline *pipeline) {
    if (!pipeline || pipeline->num_commands == 0)
        return -1;

    for (int i = 0; i < pipeline->num_commands; i++) {
        if (!cmd_has_name(&pipeline->cmds[i])) {
            report_error("Empty command");
            return -1;
        }
    }

    return 0;
}
