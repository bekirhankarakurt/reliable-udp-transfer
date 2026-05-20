#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rudp/sha256.hpp"

namespace rudp {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint16_t kMagic = 0x5255;  // "RU"
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kSackBits = 256;
inline constexpr std::uint32_t kDefaultChunkSize = 1200;
inline constexpr std::uint32_t kMaxChunkSize = 60000;
inline constexpr std::size_t kMaxNameLength = 255;
inline constexpr std::uint8_t kFlagFailed = 1;  // SYN-ACK: rejected, FIN-ACK: hash mismatch

enum class PacketType : std::uint8_t { Syn = 1, SynAck = 2, Data = 3, Ack = 4, Fin = 5, FinAck = 6 };

struct Header {
    PacketType type = PacketType::Data;
    std::uint8_t flags = 0;
    std::uint32_t session = 0;
    // Data: chunk index. Ack: cumulative ack (next chunk expected).
    // SynAck: number of leading chunks the receiver already holds.
    std::uint32_t seq = 0;
};

struct Packet {
    Header header;
    Bytes payload;
};

struct SynInfo {
    std::uint64_t file_size = 0;
    std::uint32_t chunk_size = 0;
    Sha256Digest sha256{};
    std::string name;
};

struct AckInfo {
    std::uint32_t trigger = 0;  // chunk whose arrival produced this ACK
    // Bit i set => chunk (cumulative + 1 + i) was received out of order.
    std::bitset<kSackBits> sack;
};

[[nodiscard]] Bytes encode_packet(const Header& header, const std::uint8_t* payload,
                                  std::size_t size);
[[nodiscard]] Bytes encode_packet(const Header& header, const Bytes& payload);
[[nodiscard]] std::optional<Packet> decode_packet(const Bytes& datagram);

[[nodiscard]] Bytes encode_syn(const SynInfo& info);
[[nodiscard]] std::optional<SynInfo> parse_syn(const Bytes& payload);

[[nodiscard]] Bytes encode_ack_payload(const AckInfo& info);
[[nodiscard]] std::optional<AckInfo> parse_ack_payload(const Bytes& payload);

namespace wire {

void put_u32(Bytes& out, std::uint32_t value);
void put_u64(Bytes& out, std::uint64_t value);
[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* p) noexcept;
[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* p) noexcept;

}  // namespace wire
}  // namespace rudp
