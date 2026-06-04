#include "rudp/udp_socket.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

namespace rudp {
namespace {

constexpr int kSocketBufferBytes = 4 * 1024 * 1024;

int open_socket(std::string& error) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    // Larger buffers absorb bursts on loopback; failure only costs throughput.
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kSocketBufferBytes,
                                   sizeof(kSocketBufferBytes)));
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &kSocketBufferBytes,
                                   sizeof(kSocketBufferBytes)));
    return fd;
}

}  // namespace

UdpChannel::~UdpChannel() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::unique_ptr<UdpChannel> UdpChannel::listen(std::uint16_t port, std::string& error) {
    const int fd = open_socket(error);
    if (fd < 0) {
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        error = std::string("getsockname: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    auto channel = std::make_unique<UdpChannel>(fd, true);
    channel->local_port_ = ntohs(bound.sin_port);
    return channel;
}

std::unique_ptr<UdpChannel> UdpChannel::connect(const std::string& host, std::uint16_t port,
                                                std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        error = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        return nullptr;
    }
    const int fd = open_socket(error);
    if (fd < 0) {
        ::freeaddrinfo(result);
        return nullptr;
    }
    auto channel = std::make_unique<UdpChannel>(fd, false);
    std::memcpy(&channel->peer_, result->ai_addr, result->ai_addrlen);
    channel->peer_length_ = static_cast<socklen_t>(result->ai_addrlen);
    channel->has_peer_ = true;
    ::freeaddrinfo(result);
    return channel;
}

void UdpChannel::send(const Datagram& datagram) {
    if (!has_peer_) {
        return;
    }
    static_cast<void>(::sendto(fd_, datagram.data(), datagram.size(), 0,
                               reinterpret_cast<const sockaddr*>(&peer_), peer_length_));
}

std::optional<Datagram> UdpChannel::receive(Micros timeout) {
    const auto millis = (timeout.count() + 999) / 1000;  // round up so short waits still block
    pollfd descriptor{fd_, POLLIN, 0};
    if (::poll(&descriptor, 1, static_cast<int>(millis)) <= 0 || (descriptor.revents & POLLIN) == 0) {
        return std::nullopt;
    }
    sockaddr_storage from{};
    socklen_t from_length = sizeof(from);
    const ssize_t received = ::recvfrom(fd_, buffer_.data(), buffer_.size(), 0,
                                        reinterpret_cast<sockaddr*>(&from), &from_length);
    if (received < 0) {
        return std::nullopt;
    }
    if (follow_sender_) {
        peer_ = from;
        peer_length_ = from_length;
        has_peer_ = true;
    }
    return Datagram(buffer_.begin(), buffer_.begin() + received);
}

}  // namespace rudp
