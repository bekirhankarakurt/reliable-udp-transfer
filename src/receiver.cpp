#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>

#include "rudp/transfer.hpp"

namespace rudp {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

constexpr Ms kPollSlice{50};
constexpr int kMaxAckBurst = 16;  // data packets covered by one ACK
constexpr std::uint32_t kStateSaveInterval = 64;  // chunks between resume-state writes
constexpr std::uint32_t kStateMagic = 0x52535431;  // "RST1"
constexpr std::size_t kStateSize = 4 + 32 + 8 + 4 + 4;

class ReceiveSession {
public:
    ReceiveSession(Channel& channel, const ReceiverOptions& options)
        : channel_(channel), options_(options) {}

    ReceiveResult run() {
        Status status = await_syn();
        if (status == Status::Ok) status = serve();
        result_.status = status;
        return std::move(result_);
    }

private:
    bool cancelled() const { return options_.cancel != nullptr && options_.cancel->load(); }

    Status fail(Status status, std::string message) {
        result_.message = std::move(message);
        return status;
    }

    void reply(PacketType type, std::uint8_t flags, std::uint32_t seq, const Bytes& payload = {}) {
        channel_.send(encode_packet(Header{type, flags, session_, seq}, payload));
    }

    Status await_syn() {
        const auto begin = Clock::now();
        for (;;) {
            if (cancelled()) return fail(Status::Cancelled, "cancelled while waiting for a sender");
            if (options_.wait_timeout.count() > 0 && Clock::now() - begin > options_.wait_timeout) {
                return fail(Status::Timeout, "no sender contacted us");
            }
            const auto datagram = channel_.receive(std::chrono::duration_cast<Micros>(kPollSlice));
            if (!datagram) continue;
            const auto packet = decode_packet(*datagram);
            if (!packet || packet->header.type != PacketType::Syn) continue;
            const auto info = parse_syn(packet->payload);
            if (!info) continue;
            session_ = packet->header.session;
            syn_ = *info;
            return open_transfer();
        }
    }

    Status open_transfer() {
        const fs::path name = fs::path(syn_.name).filename();
        if (syn_.chunk_size == 0 || syn_.chunk_size > kMaxChunkSize || name.empty() ||
            name == "." || name == "..") {
            reply(PacketType::SynAck, kFlagFailed, 0);
            return fail(Status::Rejected, "invalid transfer parameters from sender");
        }
        const std::uint64_t chunks = (syn_.file_size + syn_.chunk_size - 1) / syn_.chunk_size;
        if (chunks >= 0xFFFFFFFFULL) {
            reply(PacketType::SynAck, kFlagFailed, 0);
            return fail(Status::Rejected, "file too large");
        }
        total_ = static_cast<std::uint32_t>(chunks);

        std::error_code ec;
        fs::create_directories(options_.out_dir, ec);
        final_path_ = options_.out_dir / name;
        part_path_ = final_path_;
        part_path_ += ".part";
        state_path_ = final_path_;
        state_path_ += ".part.state";
        if (ec) {
            reply(PacketType::SynAck, kFlagFailed, 0);
            return fail(Status::IoError, "cannot create " + options_.out_dir.string());
        }

        const std::uint32_t prefix = load_resume_prefix();
        if (prefix == 0) {
            std::ofstream create(part_path_, std::ios::binary | std::ios::trunc);
            if (!create) {
                reply(PacketType::SynAck, kFlagFailed, 0);
                return fail(Status::IoError, "cannot create " + part_path_.string());
            }
        }
        part_.open(part_path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!part_) {
            reply(PacketType::SynAck, kFlagFailed, 0);
            return fail(Status::IoError, "cannot open " + part_path_.string());
        }

        received_.assign(total_, false);
        std::fill_n(received_.begin(), prefix, true);
        cumulative_ = got_ = resume_ = saved_prefix_ = prefix;
        write_position_ = static_cast<std::uint64_t>(prefix) * syn_.chunk_size;
        part_.seekp(static_cast<std::streamoff>(write_position_));
        result_.stats.file_size = syn_.file_size;
        result_.stats.resumed_chunks = prefix;
        result_.file = final_path_;
        reply(PacketType::SynAck, 0, resume_);
        if (got_ == total_) finalize();
        return Status::Ok;
    }

    // Resume state: magic, sha256, size, chunk size, number of leading chunks on disk.
    std::uint32_t load_resume_prefix() const {
        std::ifstream in(state_path_, std::ios::binary);
        Bytes raw(kStateSize);
        if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
            return 0;
        }
        const std::uint8_t* p = raw.data();
        if (wire::get_u32(p) != kStateMagic ||
            !std::equal(syn_.sha256.begin(), syn_.sha256.end(), p + 4) ||
            wire::get_u64(p + 36) != syn_.file_size || wire::get_u32(p + 44) != syn_.chunk_size) {
            return 0;
        }
        const std::uint32_t prefix = wire::get_u32(p + 48);
        std::error_code ec;
        const std::uint64_t on_disk = fs::file_size(part_path_, ec);
        const std::uint64_t needed =
            std::min<std::uint64_t>(static_cast<std::uint64_t>(prefix) * syn_.chunk_size, syn_.file_size);
        return (ec || prefix > total_ || on_disk < needed) ? 0 : prefix;
    }

    void save_state() {
        part_.flush();
        Bytes raw;
        wire::put_u32(raw, kStateMagic);
        raw.insert(raw.end(), syn_.sha256.begin(), syn_.sha256.end());
        wire::put_u64(raw, syn_.file_size);
        wire::put_u32(raw, syn_.chunk_size);
        wire::put_u32(raw, cumulative_);
        std::ofstream out(state_path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        saved_prefix_ = cumulative_;
    }

    Status interrupted(Status status, std::string message) {
        if (!complete_) save_state();
        return fail(status, std::move(message));
    }

    Status serve() {
        auto last_activity = Clock::now();
        auto linger_until = Clock::time_point::max();
        for (;;) {
            const auto now = Clock::now();
            if (cancelled()) return interrupted(Status::Cancelled, "cancelled");
            if (fin_answered_ && now >= linger_until) return verdict();
            if (!fin_answered_ && now - last_activity > options_.idle_timeout) {
                if (complete_) return verdict();
                return interrupted(Status::Timeout, "sender stopped responding; progress saved for resume");
            }
            if (io_failed_) return interrupted(Status::IoError, "cannot write " + part_path_.string());

            Clock::duration wait = kPollSlice;
            if (fin_answered_) wait = std::min<Clock::duration>(wait, linger_until - now);
            auto datagram = channel_.receive(std::chrono::duration_cast<Micros>(wait));
            for (int burst = 1; datagram; ++burst) {
                const auto packet = decode_packet(*datagram);
                if (packet && packet->header.session == session_) {
                    last_activity = Clock::now();
                    switch (packet->header.type) {
                        case PacketType::Syn: reply(PacketType::SynAck, 0, resume_); break;
                        case PacketType::Data: on_data(*packet); break;
                        case PacketType::Fin:
                            if (!complete_) break;
                            reply(PacketType::FinAck, verified_ ? 0 : kFlagFailed, 0);
                            if (!fin_answered_) {
                                fin_answered_ = true;
                                linger_until = Clock::now() + options_.linger;
                            }
                            break;
                        default: break;
                    }
                }
                datagram = burst < kMaxAckBurst ? channel_.receive(Micros::zero()) : std::nullopt;
            }
            flush_ack();
            if (got_ == total_ && !complete_) finalize();
        }
    }

    Status verdict() {
        if (verified_) return Status::Ok;
        return fail(Status::IntegrityFailure, "SHA-256 of received data does not match the sender's");
    }

    void on_data(const Packet& packet) {
        const std::uint32_t seq = packet.header.seq;
        if (seq >= total_) return;
        const std::uint64_t offset = static_cast<std::uint64_t>(seq) * syn_.chunk_size;
        const std::uint64_t expected = std::min<std::uint64_t>(syn_.chunk_size, syn_.file_size - offset);
        if (packet.payload.size() != expected) return;
        ++result_.stats.data_packets;

        if (received_[seq]) {
            ++result_.stats.duplicates;
        } else {
            if (offset != write_position_) part_.seekp(static_cast<std::streamoff>(offset));
            part_.write(reinterpret_cast<const char*>(packet.payload.data()),
                        static_cast<std::streamsize>(packet.payload.size()));
            if (!part_) {
                io_failed_ = true;
                return;
            }
            write_position_ = offset + expected;
            received_[seq] = true;
            ++got_;
            while (cumulative_ < total_ && received_[cumulative_]) ++cumulative_;
            if (options_.progress) {
                options_.progress(std::min<std::uint64_t>(static_cast<std::uint64_t>(got_) * syn_.chunk_size,
                                                          syn_.file_size),
                                  syn_.file_size);
            }
            if (cumulative_ - saved_prefix_ >= kStateSaveInterval) save_state();
        }
        ack_trigger_ = seq;
        ack_pending_ = true;
    }

    // One cumulative+SACK ACK per burst of arrivals keeps ACK traffic and wake-ups low.
    void flush_ack() {
        if (!ack_pending_) return;
        ack_pending_ = false;
        AckInfo ack;
        ack.trigger = ack_trigger_;
        for (std::size_t bit = 0; bit < kSackBits; ++bit) {
            const std::uint64_t seq = static_cast<std::uint64_t>(cumulative_) + 1 + bit;
            if (seq >= total_) break;
            if (received_[static_cast<std::size_t>(seq)]) ack.sack.set(bit);
        }
        reply(PacketType::Ack, 0, cumulative_, encode_ack_payload(ack));
    }

    // All chunks are present: verify the end-to-end hash and publish the file.
    void finalize() {
        part_.flush();
        part_.close();
        const auto digest = sha256_file(part_path_);
        std::error_code ec;
        if (digest && *digest == syn_.sha256) {
            fs::rename(part_path_, final_path_, ec);
            verified_ = !ec;
        }
        if (!verified_) fs::remove(part_path_, ec);
        fs::remove(state_path_, ec);
        result_.stats.sha256 = digest ? to_hex(*digest) : std::string();
        complete_ = true;
    }

    Channel& channel_;
    const ReceiverOptions& options_;
    ReceiveResult result_;

    SynInfo syn_;
    std::uint32_t session_ = 0;
    std::uint32_t total_ = 0;
    fs::path final_path_, part_path_, state_path_;
    std::fstream part_;
    std::vector<bool> received_;
    std::uint32_t cumulative_ = 0;   // next chunk not yet received in order
    std::uint32_t got_ = 0;
    std::uint32_t resume_ = 0;       // prefix advertised in SYN-ACK
    std::uint32_t saved_prefix_ = 0;
    std::uint64_t write_position_ = 0;
    bool complete_ = false;
    bool verified_ = false;
    bool fin_answered_ = false;
    bool io_failed_ = false;
    bool ack_pending_ = false;
    std::uint32_t ack_trigger_ = 0;
};

}  // namespace

ReceiveResult receive_file(Channel& channel, const ReceiverOptions& options) {
    return ReceiveSession(channel, options).run();
}

}  // namespace rudp
