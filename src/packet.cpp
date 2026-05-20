#include "rudp/packet.hpp"

#include <algorithm>

namespace rudp {
namespace wire {

void put_u32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void put_u64(Bytes& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value >> 32U));
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
}

std::uint32_t get_u32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t get_u64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(get_u32(p)) << 32U) | get_u32(p + 4);
}

}  // namespace wire

namespace {

constexpr std::size_t kSynFixedSize = 8 + 4 + 32 + 2;
constexpr std::size_t kAckPayloadSize = 4 + kSackBits / 8;

bool valid_type(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(PacketType::Syn) &&
           raw <= static_cast<std::uint8_t>(PacketType::FinAck);
}

}  // namespace

Bytes encode_packet(const Header& header, const std::uint8_t* payload, std::size_t size) {
    Bytes out;
    out.reserve(kHeaderSize + size);
    out.push_back(static_cast<std::uint8_t>(kMagic >> 8U));
    out.push_back(static_cast<std::uint8_t>(kMagic & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(header.type));
    out.push_back(header.flags);
    wire::put_u32(out, header.session);
    wire::put_u32(out, header.seq);
    if (size > 0) {
        out.insert(out.end(), payload, payload + size);
    }
    return out;
}

Bytes encode_packet(const Header& header, const Bytes& payload) {
    return encode_packet(header, payload.data(), payload.size());
}

std::optional<Packet> decode_packet(const Bytes& datagram) {
    if (datagram.size() < kHeaderSize) {
        return std::nullopt;
    }
    const std::uint16_t magic =
        static_cast<std::uint16_t>((datagram[0] << 8U) | datagram[1]);
    if (magic != kMagic || !valid_type(datagram[2])) {
        return std::nullopt;
    }
    Packet packet;
    packet.header.type = static_cast<PacketType>(datagram[2]);
    packet.header.flags = datagram[3];
    packet.header.session = wire::get_u32(datagram.data() + 4);
    packet.header.seq = wire::get_u32(datagram.data() + 8);
    packet.payload.assign(datagram.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                          datagram.end());
    return packet;
}

Bytes encode_syn(const SynInfo& info) {
    Bytes out;
    const std::size_t name_size = std::min(info.name.size(), kMaxNameLength);
    out.reserve(kSynFixedSize + name_size);
    wire::put_u64(out, info.file_size);
    wire::put_u32(out, info.chunk_size);
    out.insert(out.end(), info.sha256.begin(), info.sha256.end());
    out.push_back(static_cast<std::uint8_t>(name_size >> 8U));
    out.push_back(static_cast<std::uint8_t>(name_size & 0xFFU));
    out.insert(out.end(), info.name.begin(),
               info.name.begin() + static_cast<std::ptrdiff_t>(name_size));
    return out;
}

std::optional<SynInfo> parse_syn(const Bytes& payload) {
    if (payload.size() < kSynFixedSize) {
        return std::nullopt;
    }
    const std::uint8_t* p = payload.data();
    SynInfo info;
    info.file_size = wire::get_u64(p);
    info.chunk_size = wire::get_u32(p + 8);
    std::copy(p + 12, p + 44, info.sha256.begin());
    const std::size_t name_size = (static_cast<std::size_t>(p[44]) << 8U) | p[45];
    if (payload.size() != kSynFixedSize + name_size) {
        return std::nullopt;
    }
    info.name.assign(reinterpret_cast<const char*>(p + kSynFixedSize), name_size);
    return info;
}

Bytes encode_ack_payload(const AckInfo& info) {
    Bytes out;
    out.reserve(kAckPayloadSize);
    wire::put_u32(out, info.trigger);
    for (std::size_t byte = 0; byte < kSackBits / 8; ++byte) {
        unsigned value = 0;
        for (std::size_t bit = 0; bit < 8; ++bit) {
            if (info.sack.test(byte * 8 + bit)) {
                value |= 1U << bit;
            }
        }
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

std::optional<AckInfo> parse_ack_payload(const Bytes& payload) {
    if (payload.size() != kAckPayloadSize) {
        return std::nullopt;
    }
    AckInfo info;
    info.trigger = wire::get_u32(payload.data());
    for (std::size_t byte = 0; byte < kSackBits / 8; ++byte) {
        for (std::size_t bit = 0; bit < 8; ++bit) {
            if ((payload[4 + byte] >> bit) & 1U) {
                info.sack.set(byte * 8 + bit);
            }
        }
    }
    return info;
}

}  // namespace rudp
