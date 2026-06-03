// rudp: reliable file transfer over UDP.
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "rudp/progress.hpp"
#include "rudp/transfer.hpp"
#include "rudp/udp_socket.hpp"

namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_cancel{false};

extern "C" void on_signal(int) { g_cancel.store(true); }

constexpr const char* kUsage = R"(rudp - reliable file transfer over UDP

Usage:
  rudp send <file> <host:port> [options]
  rudp recv <port> [options]
  rudp selftest [options]
  rudp hash <file>

Options:
  --chunk BYTES       payload bytes per packet (send, selftest; default 1200)
  --window PACKETS    max packets in flight, 1..256 (send, selftest; default 256)
  --out DIR           output directory (recv; default .)
  --wait SECONDS      give up if no sender appears (recv; default: wait forever)
  --timeout SECONDS   give up after this much silence (default 10 send, 15 recv)
  --linger SECONDS    keep answering FIN after completion (recv; default 1)
  --size-mb N         generated file size (selftest; default 8)
  --sim-loss P        drop probability of datagrams received here, 0..0.9
  --sim-dup P         duplicate probability, 0..0.9
  --sim-delay MS      added one-way delay in milliseconds
  --sim-jitter MS     random extra delay (reorders packets)
  --seed N            simulation seed (default 1)
  --no-progress       never draw the progress bar
  -h, --help          show this help
)";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
};

bool parse_args(int argc, char** argv, Args& args, std::string& error) {
    static const std::set<std::string> kValueOptions = {
        "--chunk", "--window", "--out",       "--wait", "--timeout",   "--linger",
        "--size-mb", "--sim-loss", "--sim-dup", "--sim-delay", "--sim-jitter", "--seed"};
    static const std::set<std::string> kFlags = {"--no-progress", "--help", "-h"};
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (kFlags.count(arg) != 0) {
            args.flags.insert(arg);
        } else if (kValueOptions.count(arg) != 0) {
            if (i + 1 >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            args.values[arg] = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            error = "unknown option " + arg;
            return false;
        } else {
            args.positional.push_back(arg);
        }
    }
    return true;
}

bool number(const Args& args, const std::string& name, double low, double high, double& out,
            std::string& error) {
    const auto it = args.values.find(name);
    if (it == args.values.end()) return true;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(it->second.c_str(), &end);
    if (errno != 0 || end == it->second.c_str() || *end != '\0' || value < low || value > high) {
        std::ostringstream text;
        text << name << " must be a number in [" << low << ", " << high << "]";
        error = text.str();
        return false;
    }
    out = value;
    return true;
}

struct CommonSettings {
    rudp::ImpairConfig sim;
    double chunk = rudp::kDefaultChunkSize;
    double window = static_cast<double>(rudp::kSackBits);
    double timeout = -1.0;
    double wait = 0.0;
    double linger = 1.0;
    double size_mb = 8.0;
    double seed = 1.0;
    bool progress = false;
};

bool read_settings(const Args& args, CommonSettings& s, std::string& error) {
    double seed = 1.0;
    const bool ok =
        number(args, "--chunk", 1, rudp::kMaxChunkSize, s.chunk, error) &&
        number(args, "--window", 1, static_cast<double>(rudp::kSackBits), s.window, error) &&
        number(args, "--timeout", 0.1, 86400, s.timeout, error) &&
        number(args, "--wait", 0, 86400, s.wait, error) &&
        number(args, "--linger", 0, 60, s.linger, error) &&
        number(args, "--size-mb", 0.001, 4096, s.size_mb, error) &&
        number(args, "--sim-loss", 0, 0.9, s.sim.loss, error) &&
        number(args, "--sim-dup", 0, 0.9, s.sim.duplicate, error) &&
        number(args, "--sim-delay", 0, 5000, s.sim.delay_ms, error) &&
        number(args, "--sim-jitter", 0, 5000, s.sim.jitter_ms, error) &&
        number(args, "--seed", 0, 4294967295.0, seed, error);
    s.sim.seed = static_cast<std::uint32_t>(seed);
    s.progress = args.flags.count("--no-progress") == 0 && ::isatty(STDERR_FILENO) != 0;
    return ok;
}

std::chrono::milliseconds seconds_to_ms(double seconds) {
    return std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0));
}

void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

std::unique_ptr<rudp::Channel> with_simulation(std::unique_ptr<rudp::Channel> channel,
                                               const rudp::ImpairConfig& sim) {
    if (!sim.active()) return channel;
    return std::make_unique<rudp::ImpairedChannel>(std::move(channel), sim);
}

double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
}

void print_send_summary(const std::string& name, const rudp::SenderStats& st) {
    const std::uint64_t skipped = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(st.resumed_chunks) * st.chunk_size, st.file_size);
    std::cout << "sent      " << name << "  " << rudp::format_bytes(static_cast<double>(st.file_size))
              << "  in " << std::fixed << std::setprecision(2) << st.duration_s << " s  ("
              << std::setprecision(1) << mib_per_second(st.file_size - skipped, st.duration_s) << " MiB/s)\n"
              << "          packets " << st.data_packets << "  retransmits " << st.retransmits
              << "  timeouts " << st.timeouts << "  fast-losses " << st.fast_losses << "  srtt "
              << std::setprecision(2) << st.srtt_ms << " ms";
    if (st.resumed_chunks > 0) std::cout << "  resumed-chunks " << st.resumed_chunks;
    std::cout << "\n          sha256 " << st.sha256 << '\n';
}

void print_receive_summary(const rudp::ReceiveResult& r) {
    std::cout << "received  " << r.file.string() << "  "
              << rudp::format_bytes(static_cast<double>(r.stats.file_size)) << '\n'
              << "          packets " << r.stats.data_packets << "  duplicates "
              << r.stats.duplicates;
    if (r.stats.resumed_chunks > 0) std::cout << "  resumed-chunks " << r.stats.resumed_chunks;
    std::cout << "\n          sha256 " << r.stats.sha256 << " (verified)\n";
}

int report_failure(const char* role, rudp::Status status, const std::string& message) {
    std::cerr << role << " failed: " << rudp::to_string(status) << ": " << message << '\n';
    return 1;
}

int run_send(const Args& args, const CommonSettings& s) {
    if (args.positional.size() != 2) {
        std::cerr << "usage: rudp send <file> <host:port> [options]\n";
        return 2;
    }
    const std::string& target = args.positional[1];
    const auto colon = target.rfind(':');
    char* end = nullptr;
    const long port = colon == std::string::npos ? 0 : std::strtol(target.c_str() + colon + 1, &end, 10);
    if (colon == std::string::npos || colon == 0 || *end != '\0' || port < 1 || port > 65535) {
        std::cerr << "target must look like host:port\n";
        return 2;
    }
    std::string error;
    auto udp = rudp::UdpChannel::connect(target.substr(0, colon), static_cast<std::uint16_t>(port), error);
    if (!udp) {
        std::cerr << error << '\n';
        return 1;
    }
    auto channel = with_simulation(std::move(udp), s.sim);

    rudp::ProgressReporter bar(std::cerr, s.progress);
    rudp::SenderOptions options;
    options.file = args.positional[0];
    options.chunk_size = static_cast<std::uint32_t>(s.chunk);
    options.max_window = static_cast<std::size_t>(s.window);
    if (s.timeout > 0) options.idle_timeout = seconds_to_ms(s.timeout);
    options.cancel = &g_cancel;
    options.progress = [&bar](std::uint64_t done, std::uint64_t total) { bar.update(done, total); };

    const rudp::TransferResult result = rudp::send_file(*channel, options);
    bar.finish();
    if (result.status != rudp::Status::Ok) return report_failure("send", result.status, result.message);
    print_send_summary(fs::path(options.file).filename().string(), result.stats);
    return 0;
}

int run_recv(const Args& args, const CommonSettings& s) {
    if (args.positional.size() != 1) {
        std::cerr << "usage: rudp recv <port> [options]\n";
        return 2;
    }
    char* end = nullptr;
    const long port = std::strtol(args.positional[0].c_str(), &end, 10);
    if (*end != '\0' || port < 0 || port > 65535) {
        std::cerr << "port must be 0..65535\n";
        return 2;
    }
    std::string error;
    auto udp = rudp::UdpChannel::listen(static_cast<std::uint16_t>(port), error);
    if (!udp) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cerr << "listening on udp/" << udp->local_port() << '\n';
    auto channel = with_simulation(std::move(udp), s.sim);

    rudp::ProgressReporter bar(std::cerr, s.progress);
    rudp::ReceiverOptions options;
    if (args.values.count("--out") != 0) options.out_dir = args.values.at("--out");
    options.wait_timeout = seconds_to_ms(s.wait);
    if (s.timeout > 0) options.idle_timeout = seconds_to_ms(s.timeout);
    options.linger = seconds_to_ms(s.linger);
    options.cancel = &g_cancel;
    options.progress = [&bar](std::uint64_t done, std::uint64_t total) { bar.update(done, total); };

    const rudp::ReceiveResult result = rudp::receive_file(*channel, options);
    bar.finish();
    if (result.status != rudp::Status::Ok) return report_failure("receive", result.status, result.message);
    print_receive_summary(result);
    return 0;
}

int run_selftest(const CommonSettings& s) {
    const fs::path dir = fs::temp_directory_path() / ("rudp-selftest-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(dir / "out", ec);
    const fs::path source = dir / "payload.bin";
    {
        std::ofstream out(source, std::ios::binary);
        std::mt19937 rng(s.sim.seed);
        const auto bytes = static_cast<std::uint64_t>(s.size_mb * 1024.0 * 1024.0);
        std::vector<char> block(1 << 16);
        for (std::uint64_t written = 0; written < bytes; written += block.size()) {
            for (char& c : block) c = static_cast<char>(rng());
            out.write(block.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(block.size(), bytes - written)));
        }
    }

    rudp::SenderOptions sender;
    sender.file = source;
    sender.chunk_size = static_cast<std::uint32_t>(s.chunk);
    sender.max_window = static_cast<std::size_t>(s.window);
    sender.idle_timeout = seconds_to_ms(s.timeout > 0 ? s.timeout : 30);
    sender.cancel = &g_cancel;
    rudp::ReceiverOptions receiver;
    receiver.out_dir = dir / "out";
    receiver.idle_timeout = sender.idle_timeout;
    receiver.linger = seconds_to_ms(0.3);
    receiver.cancel = &g_cancel;

    std::cout << "selftest  " << rudp::format_bytes(s.size_mb * 1024.0 * 1024.0) << " over in-memory link: loss "
              << s.sim.loss * 100.0 << "%  dup " << s.sim.duplicate * 100.0 << "%  delay " << s.sim.delay_ms
              << " ms  jitter " << s.sim.jitter_ms << " ms  seed " << s.sim.seed << '\n';
    const rudp::LoopbackResult result = rudp::run_loopback(sender, receiver, s.sim);

    int code = 0;
    if (result.sender.status != rudp::Status::Ok) {
        code = report_failure("send", result.sender.status, result.sender.message);
    } else if (result.receiver.status != rudp::Status::Ok) {
        code = report_failure("receive", result.receiver.status, result.receiver.message);
    } else {
        print_send_summary("payload.bin", result.sender.stats);
        std::cout << "          receiver sha256 " << result.receiver.stats.sha256 << "  duplicates "
                  << result.receiver.stats.duplicates << '\n'
                  << "result    PASS (hashes match)\n";
    }
    fs::remove_all(dir, ec);
    return code;
}

int run_hash(const Args& args) {
    if (args.positional.size() != 1) {
        std::cerr << "usage: rudp hash <file>\n";
        return 2;
    }
    const auto digest = rudp::sha256_file(args.positional[0]);
    if (!digest) {
        std::cerr << "cannot read " << args.positional[0] << '\n';
        return 1;
    }
    std::cout << rudp::to_hex(*digest) << "  " << args.positional[0] << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cout << kUsage;
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    Args args;
    std::string error;
    CommonSettings settings;
    if (!parse_args(argc, argv, args, error) || !read_settings(args, settings, error)) {
        std::cerr << "error: " << error << "\n\n" << kUsage;
        return 2;
    }
    if (args.flags.count("--help") != 0 || args.flags.count("-h") != 0) {
        std::cout << kUsage;
        return 0;
    }
    install_signal_handlers();
    if (command == "send") return run_send(args, settings);
    if (command == "recv") return run_recv(args, settings);
    if (command == "selftest") return run_selftest(settings);
    if (command == "hash") return run_hash(args);
    std::cerr << "error: unknown command '" << command << "'\n\n" << kUsage;
    return 2;
}
