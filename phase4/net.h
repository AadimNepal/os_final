#pragma once
#include <stdint.h>
#include <sys/types.h>

int tcp_listen(uint16_t port);                     // returns listening fd
int tcp_connect(const char *host, uint16_t port);  // returns connected fd

ssize_t readn(int fd, void *buf, size_t n);        // read exactly n bytes or fail
ssize_t writen(int fd, const void *buf, size_t n); // write exactly n bytes or fail