#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "udp_sender.h"

int udp_connection_init(udp_connection_t *conn, const char *server_ip, uint16_t server_port) {
    assert(conn != NULL);
    assert(server_ip != NULL);

    if (conn->sockfd >= 0) {
        fprintf(stderr, "udp_connection_init: socket connection already init\n");
        return -1;
    }

    // очищаем структуру
    memset(conn, 0, sizeof(*conn));
    conn->sockfd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("udp_connection_init: socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) < 1) {
        fprintf(stderr, "udp_connection_init: cannot convert '%s' to IPv4 byte format\n", server_ip);
        close(fd);
        return -1;
    }

    conn->sockfd = fd;
    conn->server_addr = addr;
    conn->server_port = server_port;
    strncpy(conn->server_ip, server_ip, sizeof(conn->server_ip) - 1);

    return 0;
}

ssize_t udp_connection_send(udp_connection_t *conn, const uint8_t *data, size_t len) {
    assert(conn != NULL);

    if (conn->sockfd < 0) {
        fprintf(stderr, "udp_send: socket not initialized\n");
        return -1;
    }

    if (data == NULL) {
        fprintf(stderr, "udp_send: invalid data argument\n");
        return 0;
    }

    ssize_t sent_size = sendto(conn->sockfd, data, len, 0, (const struct sockaddr *)&conn->server_addr, sizeof(conn->server_addr));

    if (sent_size < 0) {
        perror("udp_send: sendto");
        return -1;
    }

    if (sent_size != len) {
        fprintf(stderr, "udp_send: partial send (%zd of %zu bytes)\n", sent_size, len);
    }

    return sent_size;
}

void udp_connection_close(udp_connection_t *conn) {
    if (conn == NULL)
        return;
    if (conn->sockfd >= 0) {
        close(conn->sockfd);
        conn->sockfd = -1;
    }
}

