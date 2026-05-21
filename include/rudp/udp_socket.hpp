#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <sys/socket.h>

#include "rudp/channel.hpp"

namespace rudp {

// IPv4 UDP socket exposed as a Channel (POSIX).
class UdpChannel final : public Channel {
public:
    ~UdpChannel() override;

    // Listening endpoint on all interfaces; port 0 picks an ephemeral port.
    // Replies go to the source of the most recently received datagram.
    [[nodiscard]] static std::unique_ptr<UdpChannel> listen(std::uint16_t port, std::string& error);
    // Endpoint that sends to a fixed remote host.
    [[nodiscard]] static std::unique_ptr<UdpChannel> connect(const std::string& host,
                                                             std::uint16_t port,
                                                             std::string& error);

    void send(const Datagram& datagram) override;
    [[nodiscard]] std::optional<Datagram> receive(Micros timeout) override;

    [[nodiscard]] std::uint16_t local_port() const noexcept { return local_port_; }

private:
    UdpChannel(int fd, bool follow_sender) noexcept : fd_(fd), follow_sender_(follow_sender) {}

    int fd_;
    bool follow_sender_;
    bool has_peer_ = false;
    sockaddr_storage peer_{};
    socklen_t peer_length_ = 0;
    std::uint16_t local_port_ = 0;
    std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(65536);
};

}  // namespace rudp
