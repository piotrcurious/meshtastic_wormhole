#ifndef MULTICAST_MESH_IMPL_H
#define MULTICAST_MESH_IMPL_H

#include "wifi_mesh_interface.h"
#include "packet.h"
#include <WiFi.h>
#include <WiFiUdp.h>

// Fully functional WiFi Mesh transport using standard UDP multicast on ESP32-C3
// It implements multicast group subscription, handling multi-node decentralized WiFi environments perfectly.
class MulticastMeshImpl : public WifiMeshInterface {
private:
    WiFiUDP udp;
    IPAddress multicast_ip;
    uint16_t port;

public:
    MulticastMeshImpl(const char* mcast_group = "239.10.10.10", uint16_t port_num = 4403)
        : multicast_ip(), port(port_num) {
        multicast_ip.fromString(mcast_group);
    }

    bool start() override {
        // Subscribe to UDP Multicast group
        if (!udp.beginMulticast(multicast_ip, port)) {
            return false;
        }
        return true;
    }

    bool broadcast(const uint8_t* data, size_t length) override {
        if (udp.beginPacket(multicast_ip, port)) {
            udp.write(data, length);
            return udp.endPacket() == 1;
        }
        return false;
    }

    void update() override {
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            std::vector<uint8_t> buffer(packetSize);
            int len = udp.read(buffer.data(), packetSize);
            if (len > 0) {
                // Decode the packet header dynamically to extract actual Source ID
                uint64_t from_node = 0;
                WormholePacket temp_pkt;
                if (WormholePacket::unpack(buffer.data(), len, temp_pkt)) {
                    from_node = temp_pkt.source_id;
                } else {
                    // Fallback to sender's IP address on malformed packets
                    from_node = (uint32_t)udp.remoteIP();
                }
                trigger_rx(buffer.data(), len, from_node);
            }
        }
    }
};

#endif // MULTICAST_MESH_IMPL_H
