#ifndef EXTENDER_H
#define EXTENDER_H

#include "wifi_mesh_interface.h"
#include "packet.h"
#include "deduplicator.h"
#include "region.h"
#include "meshcore_packet.h"
#include <vector>
#include <string>

class Extender {
private:
    WifiMeshInterface& wifi;
    Deduplicator& dedup;
    uint64_t node_id;
    double lat;
    double lon;
    std::vector<Region> candidate_regions;

public:
    Extender(WifiMeshInterface& w_port, Deduplicator& d_cache, uint64_t n_id,
             double node_lat, double node_lon, const std::vector<Region>& regions)
        : wifi(w_port), dedup(d_cache), node_id(n_id), lat(node_lat), lon(node_lon),
          candidate_regions(regions) {}

    bool start() {
        wifi.register_rx_callback(on_wifi_rx_static);
        instance = this;
        return wifi.start();
    }

    void update() {
        wifi.update();
    }

    // Process packets received from WiFi Mesh -> Check spatial bounds -> Repeat/Extend
    void handle_wifi_rx(const uint8_t* payload, size_t length, uint64_t from_node) {
        WormholePacket pkt;
        if (!WormholePacket::unpack(payload, length, pkt)) {
            return;
        }

        // 1. Loop prevention check
        if (pkt.has_visited(node_id)) {
            return;
        }

        // 2. Deduplication check
        if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
            return;
        }

        // Parse MeshCore packet payload
        std::string payload_str((char*)pkt.payload.data(), pkt.payload.size());
        MeshCorePacket mc_pkt = MeshCorePacket::from_json(payload_str);

        // Resolve active regions
        std::vector<Region> active_regions;
        for (const auto& r : candidate_regions) {
            if (r.contains(lat, lon)) {
                active_regions.push_back(r);
            }
        }

        // 3. Geographical spatial scope checking
        if (!mc_pkt.is_valid_for_node(lat, lon, active_regions)) {
            return;
        }

        // 4. Register hop visit
        pkt.add_hop(node_id);

        // 5. Broadcast back to mesh to repeat/extend coverage
        std::vector<uint8_t> updated_packed = pkt.pack();
        wifi.broadcast(updated_packed.data(), updated_packed.size());
    }

private:
    static Extender* instance;

    static void on_wifi_rx_static(const uint8_t* payload, size_t length, uint64_t from_node) {
        if (instance) {
            instance->handle_wifi_rx(payload, length, from_node);
        }
    }
};

#endif // EXTENDER_H
