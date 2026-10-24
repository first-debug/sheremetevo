#include <cerrno>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "log.hpp"
#include "udp_connection.hpp"

UdpConnection::UdpConnection(std::string server_ip, uint16_t server_port)
    : server_ip(server_ip), server_port(server_port) {
    if (server_ip.empty() || server_port == 0) {
        LOG_ERROR("UDP", "invalid server address");
        return;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOG_ERROR("UDP", "cannot create socket: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.data(), &addr.sin_addr) < 1) {
        LOG_ERROR("UDP", "cannot convert '%s' to IPv4 address", server_ip.c_str());
        ::close(fd);
        return;
    }

    this->sockfd = fd;
    this->server_addr = addr;
    this->server_ip = server_ip;
}

ssize_t UdpConnection::send(const uint8_t *data, size_t len) {
    // Ошибки не пишутся в журнал здесь: при сбое сети send вызывается часто,
    // журналирует (с ограничением частоты) вызывающий, причина — в errno.
    if (this->sockfd < 0) {
        errno = ENOTCONN;
        return -1;
    }

    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    ssize_t sent_size = sendto(this->sockfd,
            data,
            len,
            0,
            (const struct sockaddr *)&this->server_addr, sizeof(this->server_addr));

    return sent_size;
}

UdpConnection::~UdpConnection() {
    if (this->sockfd >= 0) {
        ::close(this->sockfd);
        this->sockfd = -1;
    }
}

