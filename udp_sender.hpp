#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <string>

class UdpConnection {
private:
    int                sockfd;
    struct sockaddr_in server_addr;

public:
    std::string              server_ip;
    const uint16_t           server_port;

    UdpConnection(std::string server_ip, uint16_t server_port);
    ssize_t send(const uint8_t *data, size_t len);

    ~UdpConnection();
};

#endif /* UDP_CLIENT_H */
