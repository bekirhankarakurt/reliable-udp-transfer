#include <thread>

#include "check.hpp"
#include "rudp/transfer.hpp"
#include "rudp/udp_socket.hpp"

int main() {
    using namespace rudp;
    namespace fs = std::filesystem;

    check::TempDir dir;
    const fs::path source = dir.path() / "payload.bin";
    check::write_random_file(source, 2 * 1024 * 1024 + 123, 21);

    std::string error;
    auto server = UdpChannel::listen(0, error);
    CHECK(server != nullptr);
    if (!server) return check::finish("udp");
    auto client = UdpChannel::connect("127.0.0.1", server->local_port(), error);
    CHECK(client != nullptr);
    if (!client) return check::finish("udp");

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

    return check::finish("udp");
}
