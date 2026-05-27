#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "rudp/transfer.hpp"

namespace {

using namespace rudp;
namespace fs = std::filesystem;

SenderOptions sender_for(const fs::path& file) {
    SenderOptions options;
    options.file = file;
    options.idle_timeout = std::chrono::seconds(30);
    options.initial_rto = std::chrono::milliseconds(100);  // keep loss recovery quick in tests
    options.min_rto = std::chrono::milliseconds(20);
    options.max_rto = std::chrono::milliseconds(250);
    return options;
}

ReceiverOptions receiver_into(const fs::path& dir) {
    ReceiverOptions options;
    options.out_dir = dir;
    options.idle_timeout = std::chrono::seconds(30);
    options.linger = std::chrono::milliseconds(200);
    return options;
}

// Runs one loopback transfer and checks the delivered bytes.
LoopbackResult transfer_and_verify(const char* label, std::size_t bytes, const ImpairConfig& link,
                                   SenderOptions tweak = {}) {
    check::TempDir dir;
    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, bytes, 7);

    SenderOptions sender = sender_for(source);
    if (tweak.chunk_size != kDefaultChunkSize) sender.chunk_size = tweak.chunk_size;
    if (tweak.max_window != kSackBits) sender.max_window = tweak.max_window;
    const LoopbackResult result = run_loopback(sender, receiver_into(dir.path() / "out"), link);

    std::cout << label << ": sender=" << to_string(result.sender.status)
              << " receiver=" << to_string(result.receiver.status)
              << " retransmits=" << result.sender.stats.retransmits
              << " timeouts=" << result.sender.stats.timeouts << std::endl;
    CHECK(result.sender.status == Status::Ok);
    CHECK(result.receiver.status == Status::Ok);
    CHECK(check::read_file(source) == check::read_file(dir.path() / "out" / "payload.bin"));
    CHECK_EQ(result.sender.stats.sha256, result.receiver.stats.sha256);
    CHECK(!fs::exists(dir.path() / "out" / "payload.bin.part"));
    return result;
}

void test_sizes() {
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{1199}, std::size_t{1200},
                                   std::size_t{1201}, std::size_t{300000}}) {
        transfer_and_verify(("size " + std::to_string(size)).c_str(), size, ImpairConfig{});
    }
}

void test_chunk_and_window_settings() {
    SenderOptions tweak;
    tweak.chunk_size = 100;
    tweak.max_window = 8;
    transfer_and_verify("small chunk, window 8", 50000, ImpairConfig{}, tweak);
    tweak.chunk_size = 9000;
    tweak.max_window = 1;
    transfer_and_verify("stop-and-wait", 40000, ImpairConfig{}, tweak);
}

void test_lossy_links() {
    ImpairConfig link;
    link.loss = 0.10;
    const LoopbackResult lossy = transfer_and_verify("10% loss", 400000, link);
    CHECK(lossy.sender.stats.retransmits > 0);

    link.loss = 0.05;
    link.duplicate = 0.05;
    link.delay_ms = 3.0;
    link.jitter_ms = 6.0;  // reorders packets
    link.seed = 99;
    transfer_and_verify("5% loss + dup + reorder", 400000, link);

    link = ImpairConfig{};
    link.loss = 0.25;
    link.seed = 5;
    transfer_and_verify("25% loss", 30000, link);
}

void test_corruption_detected() {
    // A channel that flips one payload byte of the first DATA packet it forwards.
    class Corrupting final : public Channel {
    public:
        explicit Corrupting(std::unique_ptr<Channel> inner) : inner_(std::move(inner)) {}
        void send(const Datagram& datagram) override { inner_->send(datagram); }
        std::optional<Datagram> receive(Micros timeout) override {
            auto datagram = inner_->receive(timeout);
            if (datagram && !done_ && datagram->size() > kHeaderSize + 10 &&
                (*datagram)[2] == static_cast<std::uint8_t>(PacketType::Data)) {
                (*datagram)[kHeaderSize + 5] ^= 0xFF;
                done_ = true;
            }
            return datagram;
        }

    private:
        std::unique_ptr<Channel> inner_;
        bool done_ = false;
    };

    check::TempDir dir;
    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, 20000, 3);
    ChannelPair pair = make_memory_pair();
    Corrupting receiver_end(std::move(pair.second));
    ReceiveResult received;
    ReceiverOptions receiver = receiver_into(dir.path() / "out");
    std::thread thread([&] { received = receive_file(receiver_end, receiver); });
    const TransferResult sent = send_file(*pair.first, sender_for(source));
    thread.join();

    CHECK(sent.status == Status::IntegrityFailure);
    CHECK(received.status == Status::IntegrityFailure);
    CHECK(!fs::exists(dir.path() / "out" / "payload.bin"));
}

void test_resume() {
    check::TempDir dir;
    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, 600000, 11);
    const fs::path out = dir.path() / "out";

    // First attempt is cancelled once about a third has been acknowledged.
    std::atomic<bool> cancel{false};
    SenderOptions first = sender_for(source);
    first.cancel = &cancel;
    first.progress = [&](std::uint64_t done, std::uint64_t total) {
        if (done * 3 >= total) cancel.store(true);
    };
    ReceiverOptions receiver = receiver_into(out);
    receiver.idle_timeout = std::chrono::milliseconds(400);  // notice the dead sender quickly
    const LoopbackResult interrupted = run_loopback(first, receiver, ImpairConfig{});
    CHECK(interrupted.sender.status == Status::Cancelled);
    CHECK(interrupted.receiver.status == Status::Timeout);
    CHECK(fs::exists(out / "payload.bin.part"));
    CHECK(fs::exists(out / "payload.bin.part.state"));

    // Second attempt resumes and only sends the remainder.
    const LoopbackResult resumed = run_loopback(sender_for(source), receiver_into(out), ImpairConfig{});
    CHECK(resumed.sender.status == Status::Ok);
    CHECK(resumed.receiver.status == Status::Ok);
    CHECK(resumed.sender.stats.resumed_chunks > 0);
    CHECK(resumed.sender.stats.data_packets < 500);  // whole file is 500 chunks
    CHECK(check::read_file(source) == check::read_file(out / "payload.bin"));
    CHECK(!fs::exists(out / "payload.bin.part.state"));

    // A state file for different content must be ignored.
    check::write_random_file(source, 70000, 12);
    const LoopbackResult fresh = run_loopback(sender_for(source), receiver_into(out), ImpairConfig{});
    CHECK(fresh.sender.status == Status::Ok);
    CHECK_EQ(fresh.sender.stats.resumed_chunks, 0U);
    CHECK(check::read_file(source) == check::read_file(out / "payload.bin"));
}

void test_failures() {
    check::TempDir dir;

    SenderOptions missing = sender_for(dir.path() / "nope.bin");
    ChannelPair pair = make_memory_pair();
    CHECK(send_file(*pair.first, missing).status == Status::IoError);

    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, 1000, 1);
    SenderOptions bad_chunk = sender_for(source);
    bad_chunk.chunk_size = 0;
    CHECK(send_file(*pair.first, bad_chunk).status == Status::InvalidArgument);

    SenderOptions nobody = sender_for(source);
    nobody.idle_timeout = std::chrono::milliseconds(300);
    nobody.initial_rto = std::chrono::milliseconds(50);
    CHECK(send_file(*pair.first, nobody).status == Status::Timeout);

    ReceiverOptions impatient = receiver_into(dir.path() / "out");
    impatient.wait_timeout = std::chrono::milliseconds(150);
    ChannelPair silent = make_memory_pair();
    CHECK(receive_file(*silent.second, impatient).status == Status::Timeout);

    std::atomic<bool> cancel{true};
    SenderOptions cancelled = sender_for(source);
    cancelled.cancel = &cancel;
    CHECK(send_file(*pair.first, cancelled).status == Status::Cancelled);
}

}  // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    const auto run = [](const char* name, void (*test)()) {
        const auto start = Clock::now();
        test();
        std::cout << name << " took " << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()
                  << " ms" << std::endl;
    };
    run("sizes", test_sizes);
    run("chunk/window", test_chunk_and_window_settings);
    run("lossy", test_lossy_links);
    run("corruption", test_corruption_detected);
    run("resume", test_resume);
    run("failures", test_failures);
    return check::finish("transfer");
}
