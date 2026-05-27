#pragma once

// Minimal dependency-free test harness.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>

namespace check {

inline int& failures() {
    static int count = 0;
    return count;
}

inline int finish(const char* suite) {
    if (failures() == 0) {
        std::cout << suite << ": all checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << suite << ": " << failures() << " check(s) failed\n";
    return EXIT_FAILURE;
}

// Directory removed on destruction.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("rudp-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_random_file(const std::filesystem::path& path, std::size_t bytes, unsigned seed) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < bytes; ++i) out.put(static_cast<char>(rng()));
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace check

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; \
            ++check::failures();                                                          \
        }                                                                                 \
    } while (false)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto va_ = (a);                                                              \
        const auto vb_ = (b);                                                              \
        if (!(va_ == vb_)) {                                                               \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " #a " == " #b \
                      << "  (" << va_ << " vs " << vb_ << ")\n";                          \
            ++check::failures();                                                           \
        }                                                                                  \
    } while (false)
