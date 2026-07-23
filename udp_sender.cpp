#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <iostream>

#include "udp_sender.hpp"

UdpConnection::UdpConnection(std::string server_ip, uint16_t server_port)
    : server_ip(server_ip), server_port(server_port) {
    assert(!server_ip.empty());
    assert(server_port < 1024);

    if (this->sockfd >= 0) {
        std::cerr << "udp_connection_init: socket connection already init" << std::endl;
        return;
    }

    this->sockfd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "udp_connection_init: socket" << std::endl;
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.data(), &addr.sin_addr) < 1) {
        std::cerr << "udp_connection_init: cannot convert '" << server_ip << "' to IPv4 byte format" << std::endl;;
        ::close(fd);
        return;
    }

    this->sockfd = fd;
    this->server_addr = addr;
    this->server_ip = server_ip;
}

ssize_t UdpConnection::send(const uint8_t *data, size_t len) {
    if (this->sockfd < 0) {
        std::cerr << "udp_send: socket not initialized" << std::endl;
        return -1;
    }

    if (data == NULL) {
        std::cerr << "udp_send: invalid data argument" << std::endl;
        return 0;
    }

    ssize_t sent_size = sendto(this->sockfd,
            data,
            len,
            0,
            (const struct sockaddr *)&this->server_addr, sizeof(this->server_addr));

    if (sent_size < 0) {
        std::cerr << "udp_send: sendto: "
              << std::error_code(errno, std::generic_category()).message()
              << std::endl;
        return -1;
    }

    if (sent_size != len) {
        std::cerr
            << "udp_send: partial send (" << sent_size << " of " << len << "bytes)"
            << std::endl;
    }

    return sent_size;
}

UdpConnection::~UdpConnection() {
    if (this->sockfd >= 0) {
        ::close(this->sockfd);
        this->sockfd = -1;
    }
}

