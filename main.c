/*
 * main.c - entry point for myshell
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "shell.h"
#include "parser.h"
#include "input.h"

static void shell_loop(void) {
    char input[MAX_INPUT_LENGTH];

    while (1) {
        printf("$ ");
        fflush(stdout);

        if (fgets(input, MAX_INPUT_LENGTH, stdin) == NULL) {
            printf("\n");
            break;
        }

        int len = (int)strlen(input);
        if (len > 0 && input[len - 1] == '\n')
            input[len - 1] = '\0';

        char *trimmed_input = trim_whitespace(input);
        if (strlen(trimmed_input) == 0)
            continue;

        if (strcmp(trimmed_input, "exit") == 0)
            break;

        process_input(trimmed_input);
    }
}

int main(void) {
    signal(SIGINT, SIG_IGN);
    shell_loop();
    return 0;
}
