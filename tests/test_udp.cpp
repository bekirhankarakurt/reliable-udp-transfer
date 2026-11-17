#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.hpp"
#include "rudp/transfer.hpp"
#include "rudp/udp_socket.hpp"

namespace {

// Plain loopback UDP socket the tests use to play a peer or an attacker.
class RawSocket {
public:
    RawSocket() : fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {
        sockaddr_in addr = loopback(0);
        static_cast<void>(::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
        socklen_t length = sizeof(addr);
        static_cast<void>(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length));
        port_ = ntohs(addr.sin_port);
    }
    RawSocket(const RawSocket&) = delete;
    RawSocket& operator=(const RawSocket&) = delete;
    ~RawSocket() { ::close(fd_); }

    static sockaddr_in loopback(std::uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        return addr;
    }

    std::uint16_t port() const { return port_; }

    void send_to(const sockaddr_in& to, const rudp::Bytes& data) const {
        static_cast<void>(::sendto(fd_, data.data(), data.size(), 0,
                                   reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
    }

    // Next datagram within `millis`, decoded; `from` receives its source.
    std::optional<rudp::Packet> receive(int millis, sockaddr_in* from = nullptr) const {
        pollfd descriptor{fd_, POLLIN, 0};
        if (::poll(&descriptor, 1, millis) <= 0) return std::nullopt;
        rudp::Bytes buffer(65536);
        sockaddr_in source{};
        socklen_t length = sizeof(source);
        const ssize_t got = ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source), &length);
        if (got < 0) return std::nullopt;
        buffer.resize(static_cast<std::size_t>(got));
        if (from != nullptr) *from = source;
        return rudp::decode_packet(buffer);
    }

    // Waits up to `millis` for a packet of `type`, skipping others.
    std::optional<rudp::Packet> expect(rudp::PacketType type, int millis, sockaddr_in* from = nullptr) const {
        for (int waited = 0; waited < millis; waited += 20) {
            auto packet = receive(20, from);
            if (packet && packet->header.type == type) return packet;
        }
        return std::nullopt;
    }

private:
    int fd_;
    std::uint16_t port_ = 0;
};

void test_transfer_over_udp() {
    using namespace rudp;
    namespace fs = std::filesystem;

    check::TempDir dir;
    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, 2 * 1024 * 1024 + 123, 21);

    std::string error;
    auto server = UdpChannel::listen(0, error);
    CHECK(server != nullptr);
    if (!server) return;
    auto client = UdpChannel::connect("127.0.0.1", server->local_port(), error);
    CHECK(client != nullptr);
    if (!client) return;

    ReceiverOptions receiver;
    receiver.out_dir = dir.path() / "out";
    receiver.idle_timeout = std::chrono::seconds(20);
    receiver.linger = std::chrono::milliseconds(200);
    ReceiveResult received;
    std::thread thread([&] { received = receive_file(*server, receiver); });

    SenderOptions sender;
    sender.file = source;
    sender.idle_timeout = std::chrono::seconds(20);
    const TransferResult sent = send_file(*client, sender);
    thread.join();

    CHECK(sent.status == Status::Ok);
    CHECK(received.status == Status::Ok);
    CHECK(check::read_file(source) == check::read_file(dir.path() / "out" / "payload.bin"));

    std::string bad;
    CHECK(UdpChannel::connect("no-such-host.invalid", 9, bad) == nullptr);
    CHECK(!bad.empty());
}

// A third host that forges a cumulative ACK and a FIN-ACK must not make the
// sender report success while the real receiver has acknowledged nothing.
void test_sender_ignores_forged_acks() {
    using namespace rudp;

    check::TempDir dir;
    const auto source = dir.path() / "small.bin";
    check::write_random_file(source, 1000, 5);  // a single chunk

    RawSocket receiver;
    RawSocket attacker;
    std::string error;
    auto client = UdpChannel::connect("127.0.0.1", receiver.port(), error);
    CHECK(client != nullptr);
    if (!client) return;

    SenderOptions options;
    options.file = source;
    options.idle_timeout = std::chrono::milliseconds(1500);
    std::atomic<bool> done{false};
    TransferResult sent;
    std::thread thread([&] {
        sent = send_file(*client, options);
        done = true;
    });

    sockaddr_in sender_addr{};
    const auto syn = receiver.expect(PacketType::Syn, 2000, &sender_addr);
    CHECK(syn.has_value());
    if (syn) {
        const std::uint32_t session = syn->header.session;
        receiver.send_to(sender_addr, encode_packet(Header{PacketType::SynAck, 0, session, 0}, Bytes{}));
        CHECK(receiver.expect(PacketType::Data, 2000).has_value());
        AckInfo ack;
        const Bytes forged_ack =
            encode_packet(Header{PacketType::Ack, 0, session, 1}, encode_ack_payload(ack));
        const Bytes forged_fin_ack = encode_packet(Header{PacketType::FinAck, 0, session, 0}, Bytes{});
        while (!done) {
            attacker.send_to(sender_addr, forged_ack);
            attacker.send_to(sender_addr, forged_fin_ack);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    thread.join();
    CHECK(sent.status != Status::Ok);
}

rudp::Bytes make_syn(std::uint32_t session, std::uint64_t file_size, std::uint32_t chunk_size,
                     const rudp::Sha256Digest& digest, const std::string& name) {
    rudp::SynInfo info;
    info.file_size = file_size;
    info.chunk_size = chunk_size;
    info.sha256 = digest;
    info.name = name;
    return rudp::encode_packet(rudp::Header{rudp::PacketType::Syn, 0, session, 0}, rudp::encode_syn(info));
}

// Once a SYN is accepted the receiver only talks to that sender: datagrams
// from another host are dropped and never redirect its ACKs.
void test_receiver_locks_to_syn_sender() {
    using namespace rudp;

    check::TempDir dir;
    std::string error;
    auto server = UdpChannel::listen(0, error);
    CHECK(server != nullptr);
    if (!server) return;
    const sockaddr_in server_addr = RawSocket::loopback(server->local_port());

    ReceiverOptions options;
    options.out_dir = dir.path() / "out";
    options.wait_timeout = std::chrono::seconds(5);
    options.idle_timeout = std::chrono::milliseconds(800);
    options.linger = std::chrono::milliseconds(50);
    ReceiveResult received;
    std::thread thread([&] { received = receive_file(*server, options); });

    RawSocket sender;
    RawSocket attacker;
    constexpr std::uint32_t kSession = 0x1234abcd;
    std::optional<Packet> syn_ack;
    for (int attempt = 0; attempt < 10 && !syn_ack; ++attempt) {
        sender.send_to(server_addr, make_syn(kSession, 2000, 1000, Sha256Digest{}, "locked.bin"));
        syn_ack = sender.expect(PacketType::SynAck, 200);
    }
    CHECK(syn_ack.has_value());

    const Bytes chunk(1000, 0x5a);
    attacker.send_to(server_addr, encode_packet(Header{PacketType::Data, 0, kSession, 0}, chunk));
    CHECK(!attacker.receive(400).has_value());  // no ACK leaks to the attacker
    thread.join();
    CHECK_EQ(received.stats.data_packets, std::uint64_t{0});
    CHECK(received.status != Status::Ok);
}

// A file size so large that the chunk count wraps must be rejected instead of
// being treated as an empty, already complete transfer.
void test_receiver_rejects_wrapped_file_size() {
    using namespace rudp;

    check::TempDir dir;
    std::string error;
    auto server = UdpChannel::listen(0, error);
    CHECK(server != nullptr);
    if (!server) return;
    const sockaddr_in server_addr = RawSocket::loopback(server->local_port());

    ReceiverOptions options;
    options.out_dir = dir.path() / "out";
    options.wait_timeout = std::chrono::seconds(5);
    options.idle_timeout = std::chrono::milliseconds(500);
    options.linger = std::chrono::milliseconds(50);
    ReceiveResult received;
    std::thread thread([&] { received = receive_file(*server, options); });

    RawSocket sender;
    const Sha256Digest empty_digest = Sha256().finalize();
    std::optional<Packet> syn_ack;
    for (int attempt = 0; attempt < 10 && !syn_ack; ++attempt) {
        sender.send_to(server_addr, make_syn(7, std::numeric_limits<std::uint64_t>::max(), 1024,
                                             empty_digest, "wrapped.bin"));
        syn_ack = sender.expect(PacketType::SynAck, 200);
    }
    thread.join();

    CHECK(syn_ack.has_value());
    if (syn_ack) CHECK((syn_ack->header.flags & kFlagFailed) != 0);
    CHECK(received.status == Status::Rejected);
    CHECK(!std::filesystem::exists(dir.path() / "out" / "wrapped.bin"));
}

}  // namespace

int main() {
    test_transfer_over_udp();
    test_sender_ignores_forged_acks();
    test_receiver_locks_to_syn_sender();
    test_receiver_rejects_wrapped_file_size();
    return check::finish("udp");
}
