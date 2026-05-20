#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rudp {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256() noexcept = default;

    void update(const std::uint8_t* data, std::size_t size) noexcept;
    void update(const std::string& text) noexcept;

    // Completes the hash. The object must not be reused afterwards.
    [[nodiscard]] Sha256Digest finalize() noexcept;

private:
    void process_block(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

// Streams the file through SHA-256. Returns nullopt if it cannot be read.
[[nodiscard]] std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path);

}  // namespace rudp
