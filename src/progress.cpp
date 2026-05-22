#include "rudp/progress.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace rudp {

std::string format_bytes(double bytes) {
    static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0;
    while (bytes >= 1024.0 && unit + 1 < std::size(kUnits)) {
        bytes /= 1024.0;
        ++unit;
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << bytes << ' ' << kUnits[unit];
    return text.str();
}

std::string format_bar(std::uint64_t done, std::uint64_t total, double bytes_per_second,
                       std::size_t width) {
    const double fraction =
        total == 0 ? 1.0 : std::min(1.0, static_cast<double>(done) / static_cast<double>(total));
    const auto filled = static_cast<std::size_t>(fraction * static_cast<double>(width));

    std::ostringstream text;
    text << '[' << std::string(filled, '#') << std::string(width - filled, '-') << "] "
         << std::fixed << std::setprecision(1) << std::setw(5) << fraction * 100.0 << "%  "
         << format_bytes(bytes_per_second) << "/s";
    if (bytes_per_second > 0.0 && done < total) {
        const auto eta = static_cast<std::uint64_t>(static_cast<double>(total - done) /
                                                    bytes_per_second);
        text << "  ETA " << std::setfill('0') << std::setw(2) << eta / 60 << ':' << std::setw(2)
             << eta % 60;
    }
    return text.str();
}

void ProgressReporter::update(std::uint64_t done, std::uint64_t total) {
    if (!enabled_) {
        return;
    }
    const auto now = Clock::now();
    if (!started_) {
        started_ = true;
        start_ = now;
        first_done_ = done;
    }
    constexpr auto kInterval = std::chrono::milliseconds(100);
    if (drawn_ && done < total && now - last_draw_ < kInterval) {
        return;
    }
    last_draw_ = now;
    drawn_ = true;
    const double elapsed = std::chrono::duration<double>(now - start_).count();
    const double rate = elapsed > 0.0 ? static_cast<double>(done - first_done_) / elapsed : 0.0;
    out_ << '\r' << format_bar(done, total, rate) << "   " << std::flush;
}

void ProgressReporter::finish() {
    if (drawn_) {
        out_ << '\n' << std::flush;
        drawn_ = false;
    }
}

}  // namespace rudp
