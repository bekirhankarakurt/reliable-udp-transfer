#include "check.hpp"
#include "rudp/packet.hpp"

int main() {
    using namespace rudp;

    const Bytes payload = {1, 2, 3, 4, 5};
    const Header header{PacketType::Data, 0, 0xDEADBEEF, 42};
    const Bytes wire = encode_packet(header, payload);
    CHECK_EQ(wire.size(), kHeaderSize + payload.size());
    const auto decoded = decode_packet(wire);
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(decoded->header.type == PacketType::Data);
        CHECK_EQ(decoded->header.session, 0xDEADBEEFU);
        CHECK_EQ(decoded->header.seq, 42U);
        CHECK(decoded->payload == payload);
    }

    // Wire format is big-endian with a fixed magic.
    CHECK_EQ(static_cast<int>(wire[0]), 0x52);
    CHECK_EQ(static_cast<int>(wire[1]), 0x55);
    CHECK_EQ(static_cast<int>(wire[4]), 0xDE);
    CHECK_EQ(static_cast<int>(wire[11]), 42);

    // Malformed input is rejected, never crashes.
    CHECK(!decode_packet({}).has_value());
    CHECK(!decode_packet(Bytes(kHeaderSize - 1, 0)).has_value());
    Bytes bad_magic = wire;
    bad_magic[0] = 0;
    CHECK(!decode_packet(bad_magic).has_value());
    Bytes bad_type = wire;
    bad_type[2] = 99;
    CHECK(!decode_packet(bad_type).has_value());

    SynInfo syn;
    syn.file_size = 0x0102030405060708ULL;
    syn.chunk_size = 1200;
    syn.sha256.fill(0xAB);
    syn.name = "firmware.bin";
    const Bytes syn_bytes = encode_syn(syn);
    const auto parsed = parse_syn(syn_bytes);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->file_size, syn.file_size);
        CHECK_EQ(parsed->chunk_size, 1200U);
        CHECK(parsed->sha256 == syn.sha256);
        CHECK_EQ(parsed->name, syn.name);
    }
    Bytes truncated = syn_bytes;
    truncated.pop_back();
    CHECK(!parse_syn(truncated).has_value());
    CHECK(!parse_syn(Bytes(10, 0)).has_value());

    AckInfo ack;
    ack.trigger = 77;
    ack.sack.set(0);
    ack.sack.set(9);
    ack.sack.set(255);
    const auto ack_back = parse_ack_payload(encode_ack_payload(ack));
    CHECK(ack_back.has_value());
    if (ack_back) {
        CHECK_EQ(ack_back->trigger, 77U);
        CHECK(ack_back->sack == ack.sack);
    }
    CHECK(!parse_ack_payload(Bytes(5, 0)).has_value());

    return check::finish("packet");
}
