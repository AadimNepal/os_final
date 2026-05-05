#ifndef SERVER_H
#define SERVER_H

#define PORT     9090
#define CMD_SIZE 1024
#define OUT_SIZE 65536
#define BACKLOG  10

#include <netinet/in.h>

typedef struct {
    int  client_fd;
    int  client_id;
    int  thread_label;
    char client_ip[INET_ADDRSTRLEN];
    int  client_port;
} client_context_t;

#endif /* SERVER_H */
