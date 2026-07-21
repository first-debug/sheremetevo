#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

typedef struct {
    int                sockfd;
    struct sockaddr_in server_addr;
    char               server_ip[INET_ADDRSTRLEN];
    uint16_t           server_port;
} udp_connection_t;

int udp_connection_init(udp_connection_t *conn, const char *server_ip, uint16_t server_port);
ssize_t udp_send(udp_connection_t *conn, const uint8_t *data, size_t len);
void udp_connection_close(udp_connection_t *conn);

#endif /* UDP_CLIENT_H */
