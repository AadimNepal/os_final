/*
 * input.c - user input handling for myshell
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "parser.h"
#include "executor.h"
#include "error_handler.h"

void process_input(char *input) {
    char input_copy[MAX_INPUT_LENGTH];
    strncpy(input_copy, input, MAX_INPUT_LENGTH - 1);
    input_copy[MAX_INPUT_LENGTH - 1] = '\0';

    char *trimmed = trim_whitespace(input_copy);

    if (trimmed[0] == '|') {
        report_error("Command missing before pipe.");
        return;
    }

    int input_len = (int)strlen(trimmed);
    if (input_len > 0 && trimmed[input_len - 1] == '|') {
        report_error("Command missing after pipe.");
        return;
    }

    /*
     * Use a separate copy for strtok so its in-place '\0' writes don't
     * affect the leading/trailing pipe checks above.
     */
    char input_for_split[MAX_INPUT_LENGTH];
    strncpy(input_for_split, input, MAX_INPUT_LENGTH - 1);
    input_for_split[MAX_INPUT_LENGTH - 1] = '\0';

    /*
     * Each segment gets its own private buffer so parse_command's in-place
     * writes never corrupt adjacent segments (the bug that broke
     * "cat < input | grep Hello" in the original submission).
     */
    char segment_bufs[MAX_PIPE_CMDS][MAX_INPUT_LENGTH];
    int  num_segments = 0;

    char *token = strtok(input_for_split, "|");
    while (token != NULL && num_segments < MAX_PIPE_CMDS) {
        strncpy(segment_bufs[num_segments], token, MAX_INPUT_LENGTH - 1);
        segment_bufs[num_segments][MAX_INPUT_LENGTH - 1] = '\0';
        num_segments++;
        token = strtok(NULL, "|");
    }

    for (int i = 0; i < num_segments; i++) {
        if (strlen(trim_whitespace(segment_bufs[i])) == 0) {
            report_error("Empty command between pipes.");
            return;
        }
    }

    Pipeline pl;
    pl.num_commands = num_segments;

    for (int i = 0; i < num_segments; i++) {
        if (parse_command(segment_bufs[i], &pl.cmds[i]) != 0)
            return;
    }

    if (pipeline_valid(&pl) != 0)
        return;

    execute_pipeline(&pl);
}
