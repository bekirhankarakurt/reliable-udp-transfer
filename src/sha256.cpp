#include "rudp/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace rudp {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

}  // namespace

void Sha256::process_block(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24U) |
               (static_cast<std::uint32_t>(block[4 * i + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8U) |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + big_s1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
    total_bytes_ += size;
    if (buffer_size_ > 0) {
        const std::size_t take = std::min(size, buffer_.size() - buffer_size_);
        std::memcpy(buffer_.data() + buffer_size_, data, take);
        buffer_size_ += take;
        data += take;
        size -= take;
        if (buffer_size_ < buffer_.size()) {
            return;
        }
        process_block(buffer_.data());
        buffer_size_ = 0;
    }
    while (size >= buffer_.size()) {
        process_block(data);
        data += buffer_.size();
        size -= buffer_.size();
    }
    if (size > 0) {
        std::memcpy(buffer_.data(), data, size);
        buffer_size_ = size;
    }
}

void Sha256::update(const std::string& text) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Sha256Digest Sha256::finalize() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8U;
    constexpr std::array<std::uint8_t, 64> padding = {0x80};
    const std::size_t pad_size = buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    update(padding.data(), pad_size);

    std::array<std::uint8_t, 8> length{};
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[i] = static_cast<std::uint8_t>(bit_length >> (56U - 8U * i));
    }
    update(length.data(), length.size());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[4 * i + j] = static_cast<std::uint8_t>(state_[i] >> (24U - 8U * j));
        }
    }
    return digest;
}

std::string to_hex(const Sha256Digest& digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[byte >> 4U]);
        text.push_back(kHex[byte & 0x0FU]);
    }
    return text;
}

std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Sha256 hasher;
    std::vector<char> buffer(1U << 16U);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hasher.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                      static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return hasher.finalize();
}

}  // namespace rudp
