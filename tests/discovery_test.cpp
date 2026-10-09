#include "../shared/discovery_protocol.h"
#include <stdexcept>
using namespace lanaudio;
void check(bool value) { if (!value) throw std::runtime_error("discovery protocol check failed"); }
int main() {
    std::array<uint8_t, DiscoveryQueryBytes + 1> query{};
    put32(query.data(), DiscoveryQuery); query[4] = DiscoveryVersion;
    put32(query.data() + 5, 0xf1234567);
    uint32_t nonce = 0;
    check(parseDiscoveryQuery(query.data(), DiscoveryQueryBytes, nonce) && nonce == 0xf1234567);
    for (size_t size = 0; size < query.size(); ++size)
        if (size != DiscoveryQueryBytes) check(!parseDiscoveryQuery(query.data(), size, nonce));
    check(!parseDiscoveryQuery(query.data(), query.size(), nonce));
    query[4] = 2; check(!parseDiscoveryQuery(query.data(), DiscoveryQueryBytes, nonce));
    query[4] = DiscoveryVersion; query[0] = 0; check(!parseDiscoveryQuery(query.data(), DiscoveryQueryBytes, nonce));
    std::array<uint8_t, DiscoveryHeaderBytes + DiscoveryNameBytes + 1> reply{};
    reply.back() = 0xab;
    const auto count = writeDiscoveryReply(reply.data(), nonce, 15010, std::string(100, 'x'));
    check(count == DiscoveryHeaderBytes + DiscoveryNameBytes && reply.back() == 0xab);
    check(read32(reply.data()) == DiscoveryReply && reply[4] == DiscoveryVersion);
    check(read32(reply.data() + 5) == nonce && reply[9] == Version);
    check(read16(reply.data() + 10) == 15010 && read16(reply.data() + 12) == PacketFrames);
    check(reply[14] == DiscoveryNameBytes && reply[15] == 'x');
    check(writeDiscoveryReply(reply.data(), 42, 30010, "PC") == 17);
    check(reply[14] == 2 && reply[15] == 'P' && reply[16] == 'C');
}
