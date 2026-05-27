#include <string>

#include "check.hpp"
#include "rudp/sha256.hpp"

namespace {

std::string hash_of(const std::string& text) {
    rudp::Sha256 hasher;
    hasher.update(text);
    return rudp::to_hex(hasher.finalize());
}

}  // namespace

int main() {
    // FIPS 180-4 / NIST example vectors.
    CHECK_EQ(hash_of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hash_of("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // One million 'a', fed in uneven pieces to cross block boundaries.
    rudp::Sha256 streaming;
    const std::string piece(997, 'a');
    std::size_t remaining = 1000000;
    while (remaining > 0) {
        const std::size_t n = remaining < piece.size() ? remaining : piece.size();
        streaming.update(reinterpret_cast<const std::uint8_t*>(piece.data()), n);
        remaining -= n;
    }
    CHECK_EQ(rudp::to_hex(streaming.finalize()),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    // Padding edge cases: lengths 55, 56 and 64 straddle the length-field boundary.
    CHECK_EQ(hash_of(std::string(55, 'a')),
             std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(hash_of(std::string(56, 'a')),
             std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ(hash_of(std::string(64, 'a')),
             std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));

    check::TempDir dir;
    const auto path = dir.path() / "abc.txt";
    {
        std::ofstream(path, std::ios::binary) << "abc";
    }
    const auto digest = rudp::sha256_file(path);
    CHECK(digest.has_value());
    if (digest) CHECK_EQ(rudp::to_hex(*digest), hash_of("abc"));
    CHECK(!rudp::sha256_file(dir.path() / "missing").has_value());

    return check::finish("sha256");
}
