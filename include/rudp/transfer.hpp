#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "rudp/channel.hpp"
#include "rudp/packet.hpp"

namespace rudp {

enum class Status {
    Ok,
    Cancelled,
    Timeout,
    IntegrityFailure,
    Rejected,
    IoError,
    InvalidArgument
};

[[nodiscard]] const char* to_string(Status status) noexcept;

// Called with (bytes transferred so far, total bytes).
using ProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

struct SenderOptions {
    std::filesystem::path file;
    std::uint32_t chunk_size = kDefaultChunkSize;
    std::size_t max_window = kSackBits;  // packets in flight, capped at kSackBits
    std::chrono::milliseconds initial_rto{300};
    std::chrono::milliseconds min_rto{40};
    std::chrono::milliseconds max_rto{4000};
    std::chrono::milliseconds idle_timeout{10000};  // give up after this long without feedback
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn progress;
};

struct SenderStats {
    std::uint64_t file_size = 0;
    std::uint32_t resumed_chunks = 0;  // chunks the receiver already had
    std::uint64_t data_packets = 0;    // including retransmissions
    std::uint64_t retransmits = 0;
    std::uint64_t timeouts = 0;        // RTO expirations
    std::uint64_t fast_losses = 0;     // losses inferred from SACK gaps
    double srtt_ms = 0.0;
    double duration_s = 0.0;
    std::string sha256;
};

struct TransferResult {
    Status status = Status::Ok;
    std::string message;
    SenderStats stats;
};

[[nodiscard]] TransferResult send_file(Channel& channel, const SenderOptions& options);

struct ReceiverOptions {
    std::filesystem::path out_dir = ".";
    std::chrono::milliseconds wait_timeout{0};      // for the first SYN; 0 waits forever
    std::chrono::milliseconds idle_timeout{15000};  // silence tolerated once a transfer started
    std::chrono::milliseconds linger{1000};         // keep answering FIN after completion
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn progress;
};

struct ReceiverStats {
    std::uint64_t file_size = 0;
    std::uint32_t resumed_chunks = 0;
    std::uint64_t data_packets = 0;
    std::uint64_t duplicates = 0;
    std::string sha256;
};

struct ReceiveResult {
    Status status = Status::Ok;
    std::string message;
    std::filesystem::path file;  // final path once the transfer verified
    ReceiverStats stats;
};

[[nodiscard]] ReceiveResult receive_file(Channel& channel, const ReceiverOptions& options);

struct LoopbackResult {
    TransferResult sender;
    ReceiveResult receiver;
};

// Runs sender and receiver in this process over an impaired in-memory link.
[[nodiscard]] LoopbackResult run_loopback(const SenderOptions& sender,
                                          const ReceiverOptions& receiver,
                                          const ImpairConfig& link);

}  // namespace rudp
