/*
 * client.c - remote shell client for myshell (Phase 4)
 *
 * Phase 4 changes from Phase 3:
 *   The server now streams output in multiple packets (one per output chunk)
 *   and terminates each command with a zero-length "sentinel" packet.
 *   The receive loop therefore keeps reading until it sees that sentinel,
 *   instead of consuming exactly one packet per command.  This allows the
 *   client to display ./demo output line-by-line as the scheduler delivers it.
 *
 * Wire format (unchanged from Phase 2/3):
 *   Every message is a 4-byte big-endian length followed by that many bytes
 *   of payload.  A length of zero means "end of command output".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "client.h"
#include "utils.h"

/* open a TCP connection to SERVER_IP:PORT; returns the socket fd or -1 */
static int connect_to_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * receive_command_output — read all packets for one command.
 *
 * The server sends zero or more data packets followed by a zero-length
 * sentinel packet.  We print each data packet immediately so the user
 * sees ./demo output line-by-line as it is scheduled.
 *
 * Returns 0 on success, -1 if the connection dropped unexpectedly.
 */
static int receive_command_output(int sock)
{
    while (1) {
        char  *pkt = NULL;
        size_t pkt_len = 0;

        if (recv_packet(sock, &pkt, &pkt_len) < 0) {
            free(pkt);
            return -1;   /* connection closed or error */
        }

        if (pkt_len == 0) {
            /* zero-length sentinel: this command's output is complete */
            free(pkt);
            return 0;
        }

        /* print the chunk; add a newline if the server did not include one */
        printf("%s", pkt);
        if (pkt[pkt_len - 1] != '\n')
            printf("\n");
        fflush(stdout);

        free(pkt);
    }
}

int main(void)
{
    int sock = connect_to_server();
    if (sock < 0)
        return EXIT_FAILURE;

    printf("Connected to a server\n");

    char line[BUF_SIZE];

    while (1) {
        printf(">>> ");
        fflush(stdout);

        if (fgets(line, BUF_SIZE, stdin) == NULL) {
            /* EOF on stdin (Ctrl-D) */
            printf("\n");
            break;
        }

        /* strip the trailing newline */
        line[strcspn(line, "\n")] = '\0';

        /* skip blank input without contacting the server */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        /* send the command to the server */
        if (send_packet(sock, line, strlen(line)) < 0) {
            perror("send_packet");
            break;
        }

        /* disconnect immediately after sending "exit"; no response expected */
        if (strcmp(p, "exit") == 0)
            break;

        if (receive_command_output(sock) < 0)
            break;
    }

    close(sock);
    return EXIT_SUCCESS;
}
