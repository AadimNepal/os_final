#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);
int send_packet(int fd, const char *data, size_t len);
int recv_packet(int fd, char **out, size_t *out_len);

#endif /* UTILS_H */
