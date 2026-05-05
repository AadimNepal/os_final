#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdbool.h>
#include "utils.h"

int main() {
    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    // main shell loop
    while (true) {
        printf("$ ");
        read = getline(&line, &len, stdin);
        if (read == -1) {
            break; // exit on EOF (ctrl+D)
        }

        // parse input into an array of arguments
        char** args = parse_command(line);
        if (args[0] == NULL) { continue; } // if user just hits enter, types a space or a tab, then just continue to next loop iteration
        if (strcmp(args[0], "exit") == 0) {
            break;
        }

        const char *errmsg = NULL;
        Stage *stages = NULL;
        int nstages = 0;
        if (build_pipeline(args, &stages, &nstages, &errmsg) < 0) {
            fprintf(stderr, "%s\n", errmsg);
            free(args);
            continue;  // prompt again
        }

        if (exec_pipeline(stages, nstages, -1) < 0) {
            // setup failure; errors already printed
        }
        free(stages);
    }

    // final cleanup
    free(line);
    return 0;
}