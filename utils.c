/*
 * utils.c - reliable framed I/O for the remote shell
 *
 * Wire format: every message is prefixed with a 4-byte big-endian length
 * so the receiver always knows exactly how many bytes to expect.
 *
 * Layer structure:
 *   send_bytes / recv_bytes  - EINTR-safe wrappers around send/recv
 *   send_header / recv_header - encode/decode the 4-byte length prefix
 *   send_all / recv_all      - public byte-level helpers
 *   send_packet / recv_packet - public framed-message API
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "utils.h"

/* ── low-level: loop until every requested byte is transferred ───────────── */

static int send_bytes(int fd, const void *buf, size_t n) {
    const char *cursor = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t sent = send(fd, cursor, left, 0);
        if (sent == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += sent;
        left   -= (size_t)sent;
    }
    return 0;
}

static int recv_bytes(int fd, void *buf, size_t n) {
    char *cursor = (char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t got = recv(fd, cursor, left, 0);
        if (got == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return -1;    /* connection closed by peer */
        cursor += got;
        left   -= (size_t)got;
    }
    return 0;
}

/* ── mid-level: encode and decode the 4-byte length header ──────────────── */

static int send_header(int fd, size_t payload_len) {
    uint32_t wire_len = htonl((uint32_t)payload_len);
    return send_bytes(fd, &wire_len, sizeof(wire_len));
}

static int recv_header(int fd, size_t *payload_len) {
    uint32_t wire_len = 0;
    if (recv_bytes(fd, &wire_len, sizeof(wire_len)) != 0)
        return -1;
    *payload_len = (size_t)ntohl(wire_len);
    return 0;
}

/* ── public byte-level helpers ───────────────────────────────────────────── */

int send_all(int fd, const void *buf, size_t len) {
    return send_bytes(fd, buf, len);
}

int recv_all(int fd, void *buf, size_t len) {
    return recv_bytes(fd, buf, len);
}

/* ── public framed-message API ───────────────────────────────────────────── */

int send_packet(int fd, const char *data, size_t len) {
    if (send_header(fd, len) != 0)
        return -1;
    if (len == 0)
        return 0;
    return send_bytes(fd, data, len);
}

int recv_packet(int fd, char **out, size_t *out_len) {
    size_t payload_len = 0;
    if (recv_header(fd, &payload_len) != 0)
        return -1;

    char *buf = malloc(payload_len + 1);
    if (!buf)
        return -1;

    if (payload_len > 0 && recv_bytes(fd, buf, payload_len) != 0) {
        free(buf);
        return -1;
    }

    buf[payload_len] = '\0';
    *out = buf;
    if (out_len)
        *out_len = payload_len;
    return 0;
}
