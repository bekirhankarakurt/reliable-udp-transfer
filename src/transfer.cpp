#include "rudp/transfer.hpp"

#include <thread>

namespace rudp {

const char* to_string(Status status) noexcept {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Cancelled: return "cancelled";
        case Status::Timeout: return "timeout";
        case Status::IntegrityFailure: return "integrity failure";
        case Status::Rejected: return "rejected";
        case Status::IoError: return "I/O error";
        case Status::InvalidArgument: return "invalid argument";
    }
    return "unknown";
}

LoopbackResult run_loopback(const SenderOptions& sender, const ReceiverOptions& receiver,
                            const ImpairConfig& link) {
    ChannelPair pair = make_memory_pair();
    ImpairConfig receiver_link = link;
    receiver_link.seed = link.seed + 1;
    ImpairedChannel sender_end(std::move(pair.first), link);
    ImpairedChannel receiver_end(std::move(pair.second), receiver_link);

    LoopbackResult result;
    std::thread receiver_thread(
        [&] { result.receiver = receive_file(receiver_end, receiver); });
    result.sender = send_file(sender_end, sender);
    receiver_thread.join();
    return result;
}

}  // namespace rudp
