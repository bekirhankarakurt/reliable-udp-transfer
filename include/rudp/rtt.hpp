#pragma once

#include <algorithm>
#include <chrono>

namespace rudp {

// Jacobson/Karels round-trip time estimator (RFC 6298 constants).
class RttEstimator {
public:
    using Millis = std::chrono::duration<double, std::milli>;

    RttEstimator(Millis initial_rto, Millis min_rto, Millis max_rto) noexcept
        : rto_(std::clamp(initial_rto, min_rto, max_rto)), min_rto_(min_rto), max_rto_(max_rto) {}

    // Feeds one RTT measurement. Callers must not sample retransmitted packets (Karn).
    void sample(Millis rtt) noexcept {
        if (!has_sample_) {
            srtt_ = rtt;
            rttvar_ = rtt / 2.0;
            has_sample_ = true;
        } else {
            const Millis error = srtt_ > rtt ? srtt_ - rtt : rtt - srtt_;
            rttvar_ = (1.0 - kBeta) * rttvar_ + kBeta * error;
            srtt_ = (1.0 - kAlpha) * srtt_ + kAlpha * rtt;
        }
        rto_ = std::clamp(srtt_ + 4.0 * rttvar_, min_rto_, max_rto_);
    }

    // Exponential backoff after a retransmission timeout.
    void backoff() noexcept { rto_ = std::min(rto_ * 2.0, max_rto_); }

    [[nodiscard]] Millis rto() const noexcept { return rto_; }
    [[nodiscard]] Millis srtt() const noexcept { return srtt_; }
    [[nodiscard]] Millis rttvar() const noexcept { return rttvar_; }
    [[nodiscard]] bool has_sample() const noexcept { return has_sample_; }

private:
    static constexpr double kAlpha = 0.125;
    static constexpr double kBeta = 0.25;

    Millis srtt_{0.0};
    Millis rttvar_{0.0};
    Millis rto_;
    Millis min_rto_;
    Millis max_rto_;
    bool has_sample_ = false;
};

}  // namespace rudp
