// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "net.h"

static int send_frame(int fd, const char *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (writen(fd, &be, 4) != 4) return -1;
    if (len && writen(fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

static int recv_frame(int fd, char **buf, uint32_t *len) {
    uint32_t be;
    if (readn(fd, &be, 4) != 4)
        return -1;

    uint32_t val = ntohl(be);

    // 🔸 Check for special control frame (server wants to close)
    if (val == 0xFFFFFFFF) {
        *buf = NULL;
        *len = 0;
        return 1;  // signal "session closed"
    }

    *len = val;
    *buf = NULL;

    if (*len == 0)
        return 0;  // valid empty payload (e.g., command had no output)

    *buf = malloc(*len + 1);
    if (!*buf)
        return -1;

    if (readn(fd, *buf, *len) != (ssize_t)*len) {
        free(*buf);
        *buf = NULL;
        return -1;
    }

    (*buf)[*len] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";  // default host
    uint16_t port = 5050;            // default port

    int fd = tcp_connect(host, port);
    if (fd < 0) { perror("connect"); return 1; }

    char *line = NULL; size_t cap = 0;
    for (;;) {
        printf("$ "); fflush(stdout);
        ssize_t r = getline(&line, &cap, stdin);
        if (r == -1) break;

        // send the line (including newline)
        if (send_frame(fd, line, (uint32_t)r) < 0) { fprintf(stderr, "send error\n"); break; }

        while (1) {
            char *out = NULL; 
            uint32_t olen = 0;
            int rf = recv_frame(fd, &out, &olen);

            if (rf < 0) {
                fprintf(stderr, "recv error\n");
                free(line);
                close(fd);
                return 0;
            }
            if (rf == 1) { // Server closed session
                free(line);
                close(fd);
                return 0;
            }
            
            // LENGTH 0 signals END OF COMMAND in our protocol
            if (olen == 0) {
                free(out);
                break; 
            }

            // Print chunk and flush immediately
            fwrite(out, 1, olen, stdout);
            fflush(stdout); 
            free(out);
        }
    }
    free(line);
    close(fd);
    return 0;
}
