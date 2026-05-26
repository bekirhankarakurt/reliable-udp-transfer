#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <vector>

#include "rudp/congestion.hpp"
#include "rudp/rtt.hpp"
#include "rudp/transfer.hpp"

namespace rudp {
namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

constexpr Ms kPollSlice{50};
constexpr int kMaxDrainPerWake = 256;
constexpr unsigned kFastLossDistance = 3;  // later chunks SACKed before a gap counts as lost

enum class ChunkState : std::uint8_t { Pending, InFlight, Lost, Done };

struct Slot {
    Clock::time_point sent{};
    std::uint8_t transmissions = 0;
    ChunkState state = ChunkState::Pending;
};

class SendSession {
public:
    SendSession(Channel& channel, const SenderOptions& options)
        : channel_(channel),
          options_(options),
          rtt_(RttEstimator::Millis(static_cast<double>(options.initial_rto.count())),
               RttEstimator::Millis(static_cast<double>(options.min_rto.count())),
               RttEstimator::Millis(static_cast<double>(options.max_rto.count()))),
          cc_(static_cast<double>(std::clamp<std::size_t>(options.max_window, 1, kSackBits))) {}

    TransferResult run() {
        start_ = Clock::now();
        Status status = prepare();
        if (status == Status::Ok) status = handshake();
        if (status == Status::Ok) status = transfer();
        if (status == Status::Ok) status = close();
        result_.status = status;
        result_.stats.duration_s = std::chrono::duration<double>(Clock::now() - start_).count();
        result_.stats.srtt_ms = rtt_.srtt().count();
        return std::move(result_);
    }

private:
    bool cancelled() const { return options_.cancel != nullptr && options_.cancel->load(); }

    Status fail(Status status, std::string message) {
        result_.message = std::move(message);
        return status;
    }

    Status prepare() {
        std::error_code ec;
        const std::uint64_t size = std::filesystem::file_size(options_.file, ec);
        if (ec) {
            return fail(Status::IoError, "cannot stat " + options_.file.string() + ": " + ec.message());
        }
        if (options_.chunk_size == 0 || options_.chunk_size > kMaxChunkSize) {
            return fail(Status::InvalidArgument, "chunk size must be 1.." + std::to_string(kMaxChunkSize));
        }
        chunk_ = options_.chunk_size;
        const std::uint64_t chunks = (size + chunk_ - 1) / chunk_;
        if (chunks >= 0xFFFFFFFFULL) {
            return fail(Status::InvalidArgument, "file too large for this chunk size");
        }
        total_ = static_cast<std::uint32_t>(chunks);
        name_ = options_.file.filename().string();
        if (name_.empty() || name_.size() > kMaxNameLength) {
            return fail(Status::InvalidArgument, "file name is empty or too long");
        }
        const auto digest = sha256_file(options_.file);
        file_.open(options_.file, std::ios::binary);
        if (!digest || !file_) {
            return fail(Status::IoError, "cannot read " + options_.file.string());
        }
        digest_ = *digest;
        result_.stats.file_size = size;
        result_.stats.sha256 = to_hex(digest_);
        file_size_ = size;
        slots_.assign(total_, Slot{});
        session_ = std::random_device{}();
        return Status::Ok;
    }

    Status handshake() {
        SynInfo info;
        info.file_size = file_size_;
        info.chunk_size = chunk_;
        info.sha256 = digest_;
        info.name = name_;
        const Bytes syn = encode_packet(Header{PacketType::Syn, 0, session_, 0}, encode_syn(info));

        const auto begin = Clock::now();
        auto wait = options_.initial_rto;
        bool retried = false;
        for (;;) {
            if (Clock::now() - begin > options_.idle_timeout) {
                return fail(Status::Timeout, "no response from receiver");
            }
            channel_.send(syn);
            const auto until = Clock::now() + wait;
            while (Clock::now() < until) {
                if (cancelled()) return fail(Status::Cancelled, "cancelled during handshake");
                const auto datagram = channel_.receive(
                    std::chrono::duration_cast<Micros>(std::min<Clock::duration>(until - Clock::now(), kPollSlice)));
                if (!datagram) continue;
                const auto packet = decode_packet(*datagram);
                if (!packet || packet->header.type != PacketType::SynAck ||
                    packet->header.session != session_) {
                    continue;
                }
                if ((packet->header.flags & kFlagFailed) != 0) {
                    return fail(Status::Rejected, "receiver rejected the transfer");
                }
                if (packet->header.seq > total_) {
                    return fail(Status::Rejected, "receiver sent an invalid resume offset");
                }
                if (!retried) {
                    rtt_.sample(std::chrono::duration<double, std::milli>(Clock::now() - begin));
                }
                start_transfer(packet->header.seq);
                return Status::Ok;
            }
            retried = true;
            wait = std::min(wait * 2, options_.max_rto);
        }
    }

    void start_transfer(std::uint32_t resume) {
        for (std::uint32_t i = 0; i < resume; ++i) slots_[i].state = ChunkState::Done;
        base_ = next_ = retx_cursor_ = recover_ = resume;
        result_.stats.resumed_chunks = resume;
    }

    Status transfer() {
        last_activity_ = Clock::now();
        report_progress();
        while (base_ < total_) {
            if (cancelled()) return fail(Status::Cancelled, "cancelled");
            auto now = Clock::now();
            if (now - last_activity_ > options_.idle_timeout) {
                return fail(Status::Timeout, "receiver stopped responding");
            }
            const auto expiry = expire_timeouts(now);
            if (!pump(now)) return fail(Status::IoError, "cannot read " + options_.file.string());

            Clock::duration wait = kPollSlice;
            if (expiry != Clock::time_point::max()) {
                wait = std::clamp<Clock::duration>(expiry - now, Clock::duration::zero(), kPollSlice);
            }
            auto datagram = channel_.receive(std::chrono::duration_cast<Micros>(wait));
            for (int i = 0; datagram && i < kMaxDrainPerWake; ++i) {
                on_datagram(*datagram);
                datagram = channel_.receive(Micros::zero());
            }
        }
        return Status::Ok;
    }

    Status close() {
        const Bytes fin = encode_packet(Header{PacketType::Fin, 0, session_, 0}, nullptr, 0);
        const auto begin = Clock::now();
        auto wait = std::clamp<Clock::duration>(
            std::chrono::duration_cast<Clock::duration>(rtt_.rto()), Clock::duration(Ms(20)),
            Clock::duration(options_.max_rto));
        for (;;) {
            if (Clock::now() - begin > options_.idle_timeout) {
                return fail(Status::Timeout, "all data acknowledged but no FIN acknowledgement");
            }
            channel_.send(fin);
            const auto until = Clock::now() + wait;
            while (Clock::now() < until) {
                if (cancelled()) return fail(Status::Cancelled, "cancelled");
                const auto datagram = channel_.receive(
                    std::chrono::duration_cast<Micros>(std::min<Clock::duration>(until - Clock::now(), kPollSlice)));
                if (!datagram) continue;
                const auto packet = decode_packet(*datagram);
                if (!packet || packet->header.type != PacketType::FinAck ||
                    packet->header.session != session_) {
                    continue;
                }
                if ((packet->header.flags & kFlagFailed) != 0) {
                    return fail(Status::IntegrityFailure, "receiver reports SHA-256 mismatch");
                }
                return Status::Ok;
            }
            wait = std::min(wait * 2, Clock::duration(options_.max_rto));
        }
    }

    void report_progress() const {
        if (options_.progress) {
            options_.progress(std::min<std::uint64_t>(static_cast<std::uint64_t>(base_) * chunk_, file_size_),
                              file_size_);
        }
    }

    void mark_lost(std::uint32_t seq) {
        slots_[seq].state = ChunkState::Lost;
        --inflight_;
        retx_cursor_ = std::min(retx_cursor_, seq);
    }

    // Marks timed-out chunks lost and reacts once per expiry event.
    // Returns the earliest remaining deadline.
    Clock::time_point expire_timeouts(Clock::time_point now) {
        const auto rto = std::chrono::duration_cast<Clock::duration>(rtt_.rto());
        auto earliest = Clock::time_point::max();
        bool expired = false;
        for (std::uint32_t seq = base_; seq < next_; ++seq) {
            const Slot& slot = slots_[seq];
            if (slot.state != ChunkState::InFlight) continue;
            if (now - slot.sent >= rto) {
                mark_lost(seq);
                expired = true;
            } else {
                earliest = std::min(earliest, slot.sent + rto);
            }
        }
        if (expired) {
            ++result_.stats.timeouts;
            cc_.on_timeout();
            rtt_.backoff();
            recover_ = next_;
        }
        return earliest;
    }

    bool next_lost(std::uint32_t& seq) {
        for (std::uint32_t i = std::max(retx_cursor_, base_); i < next_; ++i) {
            if (slots_[i].state == ChunkState::Lost) {
                retx_cursor_ = i + 1;
                seq = i;
                return true;
            }
        }
        retx_cursor_ = next_;
        return false;
    }

    bool pump(Clock::time_point now) {
        while (inflight_ < cc_.window()) {
            std::uint32_t seq = 0;
            if (!next_lost(seq)) {
                if (next_ >= total_) break;
                seq = next_++;
            }
            if (!transmit(seq, now)) return false;
        }
        return true;
    }

    bool transmit(std::uint32_t seq, Clock::time_point now) {
        const std::uint64_t offset = static_cast<std::uint64_t>(seq) * chunk_;
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_, file_size_ - offset));
        buffer_.resize(length);
        if (offset != file_position_) {
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(offset));
        }
        file_.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(length));
        if (static_cast<std::size_t>(file_.gcount()) != length) return false;
        file_position_ = offset + length;

        channel_.send(encode_packet(Header{PacketType::Data, 0, session_, seq}, buffer_.data(), length));
        Slot& slot = slots_[seq];
        slot.sent = now;
        slot.state = ChunkState::InFlight;
        if (slot.transmissions < 255) ++slot.transmissions;
        if (slot.transmissions > 1) ++result_.stats.retransmits;
        ++result_.stats.data_packets;
        ++inflight_;
        return true;
    }

    // Marks a chunk delivered. Returns true if it was newly acknowledged.
    bool settle(std::uint32_t seq) {
        Slot& slot = slots_[seq];
        if (slot.state != ChunkState::InFlight && slot.state != ChunkState::Lost) return false;
        if (slot.state == ChunkState::InFlight) --inflight_;
        slot.state = ChunkState::Done;
        return true;
    }

    void on_datagram(const Datagram& datagram) {
        const auto packet = decode_packet(datagram);
        if (!packet || packet->header.type != PacketType::Ack || packet->header.session != session_) return;
        const auto ack = parse_ack_payload(packet->payload);
        const std::uint32_t cumulative = packet->header.seq;
        if (!ack || cumulative > next_) return;
        const auto now = Clock::now();
        last_activity_ = now;

        unsigned newly = 0;
        const bool trigger_known = ack->trigger < total_;
        const bool trigger_was_done = trigger_known && slots_[ack->trigger].state == ChunkState::Done;

        while (base_ < cumulative) {
            if (settle(base_)) ++newly;
            ++base_;
        }
        bool have_highest = false;
        std::uint32_t highest = 0;
        for (std::size_t bit = 0; bit < kSackBits; ++bit) {
            if (!ack->sack.test(bit)) continue;
            const std::uint64_t seq = static_cast<std::uint64_t>(cumulative) + 1 + bit;
            if (seq >= next_) break;
            const auto index = static_cast<std::uint32_t>(seq);
            if (settle(index)) ++newly;
            highest = index;
            have_highest = true;
        }

        if (trigger_known && !trigger_was_done && slots_[ack->trigger].state == ChunkState::Done &&
            slots_[ack->trigger].transmissions == 1) {  // Karn: skip retransmitted chunks
            rtt_.sample(std::chrono::duration<double, std::milli>(now - slots_[ack->trigger].sent));
        }
        cc_.on_ack(newly);
        if (have_highest) detect_gaps(highest);
        report_progress();
    }

    // SACK-based loss inference: a chunk with kFastLossDistance later chunks
    // already delivered is considered lost. Retransmissions are left to the timer.
    void detect_gaps(std::uint32_t highest) {
        bool lost_any = false;
        std::uint32_t first_lost = 0;
        for (std::uint32_t seq = base_; seq < next_ && seq + kFastLossDistance <= highest; ++seq) {
            Slot& slot = slots_[seq];
            if (slot.state != ChunkState::InFlight || slot.transmissions != 1) continue;
            if (!lost_any) first_lost = seq;
            lost_any = true;
            mark_lost(seq);
        }
        if (lost_any) {
            ++result_.stats.fast_losses;
            if (first_lost >= recover_) {  // one window reduction per loss episode
                cc_.on_fast_loss();
                recover_ = next_;
            }
        }
    }

    Channel& channel_;
    const SenderOptions& options_;
    RttEstimator rtt_;
    CongestionController cc_;
    TransferResult result_;
    Clock::time_point start_{};
    Clock::time_point last_activity_{};

    std::ifstream file_;
    std::uint64_t file_position_ = 0;
    Bytes buffer_;
    Sha256Digest digest_{};
    std::string name_;
    std::uint64_t file_size_ = 0;
    std::uint32_t chunk_ = 0;
    std::uint32_t total_ = 0;
    std::uint32_t session_ = 0;

    std::vector<Slot> slots_;
    std::uint32_t base_ = 0;         // lowest unacknowledged chunk
    std::uint32_t next_ = 0;         // next never-sent chunk
    std::uint32_t retx_cursor_ = 0;  // no Lost chunk exists below this index
    std::uint32_t recover_ = 0;      // losses of chunks below this are part of an earlier episode
    std::size_t inflight_ = 0;
};

}  // namespace

TransferResult send_file(Channel& channel, const SenderOptions& options) {
    return SendSession(channel, options).run();
}

}  // namespace rudp
