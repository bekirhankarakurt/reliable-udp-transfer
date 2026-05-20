#pragma once

#include <algorithm>
#include <cstddef>

namespace rudp {

// Additive-increase / multiplicative-decrease window, counted in packets.
class CongestionController {
public:
    explicit CongestionController(double max_window, double initial_window = 2.0) noexcept
        : cwnd_(std::min(initial_window, max_window)), ssthresh_(max_window), max_(max_window) {}

    // Called with the number of packets newly acknowledged by one ACK.
    void on_ack(unsigned acked) noexcept {
        for (unsigned i = 0; i < acked; ++i) {
            cwnd_ += in_slow_start() ? 1.0 : 1.0 / cwnd_;  // exponential, then +1 per RTT
        }
        cwnd_ = std::min(cwnd_, max_);
    }

    // Loss detected from selective ACKs: halve the window.
    void on_fast_loss() noexcept {
        ssthresh_ = std::max(cwnd_ / 2.0, kMinSsthresh);
        cwnd_ = ssthresh_;
    }

    // Retransmission timeout: halve the threshold and restart from one packet.
    void on_timeout() noexcept {
        ssthresh_ = std::max(cwnd_ / 2.0, kMinSsthresh);
        cwnd_ = 1.0;
    }

    [[nodiscard]] std::size_t window() const noexcept {
        return static_cast<std::size_t>(std::max(cwnd_, 1.0));
    }
    [[nodiscard]] double cwnd() const noexcept { return cwnd_; }
    [[nodiscard]] double ssthresh() const noexcept { return ssthresh_; }
    [[nodiscard]] bool in_slow_start() const noexcept { return cwnd_ < ssthresh_; }

private:
    static constexpr double kMinSsthresh = 2.0;

    double cwnd_;
    double ssthresh_;
    double max_;
};

}  // namespace rudp
