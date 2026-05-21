#include "rudp/channel.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace rudp {
namespace {

struct MemoryQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Datagram> items;
};

class MemoryEndpoint final : public Channel {
public:
    MemoryEndpoint(std::shared_ptr<MemoryQueue> inbox, std::shared_ptr<MemoryQueue> outbox)
        : inbox_(std::move(inbox)), outbox_(std::move(outbox)) {}

    void send(const Datagram& datagram) override {
        {
            const std::lock_guard<std::mutex> lock(outbox_->mutex);
            outbox_->items.push_back(datagram);
        }
        outbox_->ready.notify_one();
    }

    std::optional<Datagram> receive(Micros timeout) override {
        std::unique_lock<std::mutex> lock(inbox_->mutex);
        if (!inbox_->ready.wait_for(lock, timeout, [this] { return !inbox_->items.empty(); })) {
            return std::nullopt;
        }
        Datagram datagram = std::move(inbox_->items.front());
        inbox_->items.pop_front();
        return datagram;
    }

private:
    std::shared_ptr<MemoryQueue> inbox_;
    std::shared_ptr<MemoryQueue> outbox_;
};

}  // namespace

ChannelPair make_memory_pair() {
    auto a_to_b = std::make_shared<MemoryQueue>();
    auto b_to_a = std::make_shared<MemoryQueue>();
    return {std::make_unique<MemoryEndpoint>(b_to_a, a_to_b),
            std::make_unique<MemoryEndpoint>(a_to_b, b_to_a)};
}

ImpairedChannel::ImpairedChannel(std::unique_ptr<Channel> inner, const ImpairConfig& config)
    : inner_(std::move(inner)), config_(config), rng_(config.seed) {}

void ImpairedChannel::send(const Datagram& datagram) { inner_->send(datagram); }

void ImpairedChannel::ingest(Datagram datagram) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    if (unit(rng_) < config_.loss) {
        return;
    }
    const int copies = unit(rng_) < config_.duplicate ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        const double jitter = config_.jitter_ms > 0.0 ? unit(rng_) * config_.jitter_ms : 0.0;
        const auto delay = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(config_.delay_ms + jitter));
        pending_.emplace(Clock::now() + delay, datagram);
    }
}

std::optional<Datagram> ImpairedChannel::receive(Micros timeout) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        while (auto fresh = inner_->receive(Micros::zero())) {
            ingest(std::move(*fresh));
        }
        const auto now = Clock::now();
        if (!pending_.empty() && pending_.begin()->first <= now) {
            Datagram datagram = std::move(pending_.begin()->second);
            pending_.erase(pending_.begin());
            return datagram;
        }
        if (now >= deadline) {
            return std::nullopt;
        }
        auto wake = deadline;
        if (!pending_.empty()) {
            wake = std::min(wake, pending_.begin()->first);
        }
        const auto wait = std::chrono::duration_cast<Micros>(wake - now);
        if (auto fresh = inner_->receive(wait > Micros::zero() ? wait : Micros(1))) {
            ingest(std::move(*fresh));
        }
    }
}

}  // namespace rudp
