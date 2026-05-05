// demo.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <n>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    // The loop runs 0 to n-1, printing N lines total.
    for (int i = 0; i < n; i++) {
        // Strict format matching the screenshots
        printf("Demo %d/%d\n", i+1, n);
        fflush(stdout); // CRITICAL: Push to pipe immediately
        sleep(1);       // Simulate 1 second of work
    }
    return 0;
}