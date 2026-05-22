#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

namespace rudp {

[[nodiscard]] std::string format_bytes(double bytes);

// One-line progress bar, e.g. "[#####-----]  50.0%  12.0 MiB/s  ETA 00:03".
[[nodiscard]] std::string format_bar(std::uint64_t done, std::uint64_t total,
                                     double bytes_per_second, std::size_t width = 30);

// Redraws a progress bar on a stream, at most ten times per second.
class ProgressReporter {
public:
    ProgressReporter(std::ostream& out, bool enabled) noexcept : out_(out), enabled_(enabled) {}

    void update(std::uint64_t done, std::uint64_t total);
    void finish();

private:
    using Clock = std::chrono::steady_clock;

    std::ostream& out_;
    bool enabled_;
    bool started_ = false;
    bool drawn_ = false;
    std::uint64_t first_done_ = 0;
    Clock::time_point start_{};
    Clock::time_point last_draw_{};
};

}  // namespace rudp
