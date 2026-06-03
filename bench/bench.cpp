// Throughput benchmark: rudp vs plain TCP on loopback, plus rudp on simulated lossy links.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rudp/transfer.hpp"
#include "rudp/udp_socket.hpp"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr double kMiB = 1024.0 * 1024.0;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void make_file(const fs::path& path, std::uint64_t bytes) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937 rng(42);
    std::vector<char> block(1 << 16);
    for (std::uint64_t done = 0; done < bytes; done += block.size()) {
        for (char& c : block) c = static_cast<char>(rng());
        out.write(block.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(block.size(), bytes - done)));
    }
}

// Plain TCP over loopback: one thread listens and drains, the main thread sends the file.
double bench_tcp(const fs::path& file, std::uint64_t bytes) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(addr);
    if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 || ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        std::cerr << "tcp setup failed\n";
        return 0.0;
    }
    std::uint64_t received = 0;
    std::thread drain([&] {
        const int conn = ::accept(listener, nullptr, nullptr);
        std::vector<char> buffer(1 << 16);
        ssize_t n = 0;
        while (conn >= 0 && (n = ::read(conn, buffer.data(), buffer.size())) > 0) {
            received += static_cast<std::uint64_t>(n);
        }
        if (conn >= 0) ::close(conn);
    });

    const auto start = Clock::now();
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> buffer(1 << 16);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const char* p = buffer.data();
            auto left = static_cast<std::size_t>(in.gcount());
            while (left > 0) {
                const ssize_t n = ::write(client, p, left);
                if (n <= 0) break;
                p += n;
                left -= static_cast<std::size_t>(n);
            }
        }
    }
    ::close(client);
    drain.join();
    const double elapsed = seconds_since(start);
    ::close(listener);
    return received == bytes ? elapsed : 0.0;
}

double bench_udp_loopback(const fs::path& file, const fs::path& out) {
    std::string error;
    auto server = rudp::UdpChannel::listen(0, error);
    auto client = server ? rudp::UdpChannel::connect("127.0.0.1", server->local_port(), error) : nullptr;
    if (!client) {
        std::cerr << error << '\n';
        return 0.0;
    }
    rudp::ReceiverOptions receiver;
    receiver.out_dir = out;
    receiver.linger = std::chrono::milliseconds(100);
    rudp::ReceiveResult received;
    std::thread thread([&] { received = rudp::receive_file(*server, receiver); });
    rudp::SenderOptions sender;
    sender.file = file;
    const auto start = Clock::now();
    const rudp::TransferResult sent = rudp::send_file(*client, sender);
    const double elapsed = seconds_since(start);
    thread.join();
    return sent.status == rudp::Status::Ok && received.status == rudp::Status::Ok ? elapsed : 0.0;
}

double bench_simulated(const fs::path& file, const fs::path& out, const rudp::ImpairConfig& link) {
    rudp::SenderOptions sender;
    sender.file = file;
    rudp::ReceiverOptions receiver;
    receiver.out_dir = out;
    receiver.linger = std::chrono::milliseconds(100);
    const auto start = Clock::now();
    const rudp::LoopbackResult result = rudp::run_loopback(sender, receiver, link);
    const double elapsed = seconds_since(start);
    return result.sender.status == rudp::Status::Ok && result.receiver.status == rudp::Status::Ok ? elapsed : 0.0;
}

void row(const std::string& name, std::uint64_t bytes, double seconds, const std::string& model) {
    std::cout << "| " << std::left << std::setw(34) << name << " | " << std::right << std::fixed;
    if (seconds > 0.0) {
        std::cout << std::setw(8) << std::setprecision(1) << static_cast<double>(bytes) / kMiB / seconds
                  << " | " << std::setw(7) << std::setprecision(2) << seconds;
    } else {
        std::cout << std::setw(8) << "FAILED" << " | " << std::setw(7) << "-";
    }
    std::cout << " | " << std::left << std::setw(11) << model << " |\n" << std::flush;
}

// Mathis et al. steady-state TCP Reno bound: MSS / RTT * 1.22 / sqrt(p).
std::string tcp_model(double rtt_ms, double loss) {
    const double bytes_per_s = 1460.0 / (rtt_ms / 1000.0) * 1.22 / std::sqrt(loss);
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << bytes_per_s / kMiB;
    return text.str();
}

}  // namespace

int main(int argc, char** argv) {
    const double loopback_mib = argc > 1 ? std::atof(argv[1]) : 64.0;
    const double simulated_mib = argc > 2 ? std::atof(argv[2]) : 4.0;
    if (loopback_mib <= 0.0 || simulated_mib <= 0.0) {
        std::cerr << "usage: rudp_bench [loopback_MiB] [simulated_MiB]\n";
        return 2;
    }

    const fs::path dir = fs::temp_directory_path() / ("rudp-bench-" + std::to_string(::getpid()));
    fs::create_directories(dir / "out");
    const auto big = static_cast<std::uint64_t>(loopback_mib * kMiB);
    const auto small = static_cast<std::uint64_t>(simulated_mib * kMiB);
    make_file(dir / "big.bin", big);
    make_file(dir / "small.bin", small);

    std::cout << "| Scenario                           |   MiB/s  | seconds | TCP model   |\n"
              << "|------------------------------------|----------|---------|-------------|\n";
    row("TCP, loopback", big, bench_tcp(dir / "big.bin", big), "-");
    row("rudp, UDP loopback", big, bench_udp_loopback(dir / "big.bin", dir / "out"), "-");

    for (const double loss : {0.0, 0.01, 0.05}) {
        rudp::ImpairConfig link;
        link.loss = loss;
        link.delay_ms = 10.0;  // each direction => 20 ms RTT
        std::ostringstream name;
        name << "rudp, 20 ms RTT, " << loss * 100.0 << "% loss";
        row(name.str(), small, bench_simulated(dir / "small.bin", dir / "out", link),
            loss > 0.0 ? tcp_model(20.0, loss) : "-");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}
