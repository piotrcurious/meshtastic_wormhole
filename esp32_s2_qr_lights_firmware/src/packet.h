#ifndef PACKET_H
#define PACKET_H

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <vector>
#include <cstdint>
#include <cstring>

const uint8_t MAGIC_BYTES[4] = {'W', 'H', 'O', 'L'};

struct WormholePacket {
    uint8_t version;
    uint8_t flags;
    uint16_t header_size;
    uint64_t source_id;
    uint32_t packet_id;
    uint32_t timestamp;
    uint16_t payload_length;
    std::vector<uint64_t> hops;
    std::vector<uint8_t> payload;

    // Pack the packet into a byte buffer
    std::vector<uint8_t> pack() const {
        std::vector<uint8_t> buffer;
        uint16_t computed_header_size = 26;
        if (!hops.empty()) {
            computed_header_size += 2 + (8 * hops.size());
        }

        // Magic
        buffer.insert(buffer.end(), MAGIC_BYTES, MAGIC_BYTES + 4);
        buffer.push_back(version);
        buffer.push_back(flags);

        // Header size (little endian)
        buffer.push_back(computed_header_size & 0xFF);
        buffer.push_back((computed_header_size >> 8) & 0xFF);

        // Source ID (little endian uint64_t)
        for (int i = 0; i < 8; i++) {
            buffer.push_back((source_id >> (8 * i)) & 0xFF);
        }

        // Packet ID (little endian uint32_t)
        for (int i = 0; i < 4; i++) {
            buffer.push_back((packet_id >> (8 * i)) & 0xFF);
        }

        // Timestamp (little endian uint32_t)
        for (int i = 0; i < 4; i++) {
            buffer.push_back((timestamp >> (8 * i)) & 0xFF);
        }

        // Payload length (little endian uint16_t)
        buffer.push_back(payload_length & 0xFF);
        buffer.push_back((payload_length >> 8) & 0xFF);

        // Hops count and list
        if (!hops.empty()) {
            uint16_t hops_count = hops.size();
            buffer.push_back(hops_count & 0xFF);
            buffer.push_back((hops_count >> 8) & 0xFF);
            for (uint64_t hop : hops) {
                for (int i = 0; i < 8; i++) {
                    buffer.push_back((hop >> (8 * i)) & 0xFF);
                }
            }
        }

        // Payload
        buffer.insert(buffer.end(), payload.begin(), payload.end());
        return buffer;
    }

    // Unpack from raw bytes
    static bool unpack(const uint8_t* data, size_t length, WormholePacket& pkt) {
        if (length < 26) return false;

        // Verify Magic
        if (data[0] != MAGIC_BYTES[0] || data[1] != MAGIC_BYTES[1] ||
            data[2] != MAGIC_BYTES[2] || data[3] != MAGIC_BYTES[3]) {
            return false;
        }

        pkt.version = data[4];
        pkt.flags = data[5];
        pkt.header_size = data[6] | (data[7] << 8);

        // Parse Source ID
        pkt.source_id = 0;
        for (int i = 0; i < 8; i++) {
            pkt.source_id |= ((uint64_t)data[8 + i] << (8 * i));
        }

        // Parse Packet ID
        pkt.packet_id = 0;
        for (int i = 0; i < 4; i++) {
            pkt.packet_id |= ((uint32_t)data[16 + i] << (8 * i));
        }

        // Parse Timestamp
        pkt.timestamp = 0;
        for (int i = 0; i < 4; i++) {
            pkt.timestamp |= ((uint32_t)data[20 + i] << (8 * i));
        }

        pkt.payload_length = data[24] | (data[25] << 8);

        if (length < pkt.header_size + pkt.payload_length) return false;

        pkt.hops.clear();
        // Fixed: Ensure header_size is at least 28 and length is at least 28 to prevent out-of-bounds read
        if (pkt.header_size >= 28 && length >= 28) {
            uint16_t hops_count = data[26] | (data[27] << 8);
            size_t hop_offset = 28;
            for (uint16_t h = 0; h < hops_count; h++) {
                if (hop_offset + 8 > pkt.header_size) return false;
                uint64_t hop_id = 0;
                for (int i = 0; i < 8; i++) {
                    hop_id |= ((uint64_t)data[hop_offset + i] << (8 * i));
                }
                pkt.hops.push_back(hop_id);
                hop_offset += 8;
            }
        }

        pkt.payload.clear();
        pkt.payload.insert(pkt.payload.end(), data + pkt.header_size, data + pkt.header_size + pkt.payload_length);

        return true;
    }

    bool has_visited(uint64_t node_id) const {
        if (source_id == node_id) return true;
        for (uint64_t hop : hops) {
            if (hop == node_id) return true;
        }
        return false;
    }

    void add_hop(uint64_t node_id) {
        if (!has_visited(node_id)) {
            hops.push_back(node_id);
        }
    }
};

#endif // PACKET_H
