/*
 * demo.c - schedulable test program for Phase 4
 *
 * Prints N lines, one per second, so the server scheduler can observe
 * progress line-by-line through a pipe.  fflush() after every printf
 * is critical: without it the kernel pipe buffer would hold output until
 * the process exits, defeating line-by-line scheduling.
 *
 * Usage: ./demo <N>
 *   N = total number of seconds (lines) to run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <N>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    if (n <= 0) {
        fprintf(stderr, "demo: N must be a positive integer\n");
        return 1;
    }

    for (int i = 1; i <= n; i++) {
        printf("Demo %d/%d\n", i, n);
        fflush(stdout);   /* push each line through the pipe immediately */
        sleep(1);
    }

    return 0;
}
