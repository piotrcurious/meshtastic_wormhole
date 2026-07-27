#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdint>

// Lightweight mocks for Arduino environments so we can compile and test under standard desktop g++
#define Arduino_h
class String {
public:
    String() {}
    String(const char*) {}
    const char* c_str() const { return ""; }
};
inline uint32_t millis() { return 1000; }

// Inject packet.h
#include "packet.h"

void test_cpp_packet_serialization() {
    WormholePacket pkt;
    pkt.version = 1;
    pkt.flags = 0;
    pkt.source_id = 0x1122334455667788ULL;
    pkt.packet_id = 12345;
    pkt.timestamp = 987654321;
    pkt.payload_length = 5;
    pkt.payload = {'H', 'e', 'l', 'l', 'o'};
    pkt.hops = {0xAAAABBBBCCCCDDDDUll};

    std::vector<uint8_t> packed = pkt.pack();
    assert(packed.size() == 26 + 2 + 8 + 5);

    WormholePacket unpacked;
    bool success = WormholePacket::unpack(packed.data(), packed.size(), unpacked);
    assert(success == true);

    assert(unpacked.version == pkt.version);
    assert(unpacked.flags == pkt.flags);
    assert(unpacked.source_id == pkt.source_id);
    assert(unpacked.packet_id == pkt.packet_id);
    assert(unpacked.timestamp == pkt.timestamp);
    assert(unpacked.payload_length == pkt.payload_length);
    assert(unpacked.payload == pkt.payload);
    assert(unpacked.hops == pkt.hops);

    std::cout << "test_cpp_packet_serialization: PASSED" << std::endl;
}

void test_cpp_packet_bounds_checks() {
    uint8_t malformed[27];
    memset(malformed, 0, sizeof(malformed));
    // Valid magic
    malformed[0] = 'W'; malformed[1] = 'H'; malformed[2] = 'O'; malformed[3] = 'l'; // mismatch case on last byte

    WormholePacket unpacked;
    bool success = WormholePacket::unpack(malformed, sizeof(malformed), unpacked);
    assert(success == false); // magic fails

    // Correct magic
    malformed[3] = 'L';
    success = WormholePacket::unpack(malformed, 20, unpacked);
    assert(success == false); // too short (<26)

    // Header size mismatch
    malformed[6] = 40; // Header size 40 but total length 27
    success = WormholePacket::unpack(malformed, 27, unpacked);
    assert(success == false);

    std::cout << "test_cpp_packet_bounds_checks: PASSED" << std::endl;
}

int main() {
    test_cpp_packet_serialization();
    test_cpp_packet_bounds_checks();
    std::cout << "All C++ verification tests PASSED!" << std::endl;
    return 0;
}
