#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace rudp {

using Datagram = std::vector<std::uint8_t>;
using Micros = std::chrono::microseconds;

// Unreliable, message-oriented transport. Implemented by UDP sockets and
// by in-memory links used for simulation and tests.
class Channel {
public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    virtual ~Channel() = default;

    // Best effort: failures are indistinguishable from packet loss.
    virtual void send(const Datagram& datagram) = 0;
    // Waits up to `timeout` for one datagram.
    [[nodiscard]] virtual std::optional<Datagram> receive(Micros timeout) = 0;
};

struct ImpairConfig {
    double loss = 0.0;       // probability a datagram is dropped, [0, 1)
    double duplicate = 0.0;  // probability a datagram is delivered twice, [0, 1)
    double delay_ms = 0.0;   // fixed one-way delay
    double jitter_ms = 0.0;  // extra uniform delay in [0, jitter]; causes reordering
    std::uint32_t seed = 1;

    [[nodiscard]] bool active() const noexcept {
        return loss > 0.0 || duplicate > 0.0 || delay_ms > 0.0 || jitter_ms > 0.0;
    }
};

// Applies loss, duplication, delay and reordering to every datagram received
// through the wrapped channel. Needs no root privileges or tc/netem.
class ImpairedChannel final : public Channel {
public:
    ImpairedChannel(std::unique_ptr<Channel> inner, const ImpairConfig& config);

    void send(const Datagram& datagram) override;
    [[nodiscard]] std::optional<Datagram> receive(Micros timeout) override;

private:
    using Clock = std::chrono::steady_clock;

    void ingest(Datagram datagram);

    std::unique_ptr<Channel> inner_;
    ImpairConfig config_;
    std::mt19937 rng_;
    std::multimap<Clock::time_point, Datagram> pending_;
};

struct ChannelPair {
    std::unique_ptr<Channel> first;
    std::unique_ptr<Channel> second;
};

// Two connected in-process endpoints with an unbounded lossless queue each way.
[[nodiscard]] ChannelPair make_memory_pair();

}  // namespace rudp
