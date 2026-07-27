#ifndef LORA_BRIDGE_H
#define LORA_BRIDGE_H

#include "lora_interface.h"
#include "wifi_mesh_interface.h"
#include "packet.h"
#include "deduplicator.h"
#include "region.h"
#include "meshcore_packet.h"
#include <vector>
#include <string>

class LoraBridge {
private:
    LoraInterface& lora;
    WifiMeshInterface& wifi;
    Deduplicator& dedup;
    uint64_t node_id;
    uint32_t& next_packet_id;
    double lat;
    double lon;
    std::vector<Region> candidate_regions;

public:
    LoraBridge(LoraInterface& l_port, WifiMeshInterface& w_port, Deduplicator& d_cache,
               uint64_t n_id, uint32_t& p_id_counter, double node_lat, double node_lon,
               const std::vector<Region>& regions)
        : lora(l_port), wifi(w_port), dedup(d_cache), node_id(n_id), next_packet_id(p_id_counter),
          lat(node_lat), lon(node_lon), candidate_regions(regions) {}

    bool start() {
        lora.register_rx_callback(on_lora_rx_static);
        wifi.register_rx_callback(on_wifi_rx_static);
        instance = this;
        return lora.start() && wifi.start();
    }

    void update() {
        lora.update();
        wifi.update();
    }

    // Process packets received from local LoRa network -> Bridge to WiFi Mesh
    void handle_lora_rx(const uint8_t* payload, size_t length) {
        // Wrap local LoRa packet in a MeshCorePacket
        char sender_hex[16];
        snprintf(sender_hex, sizeof(sender_hex), "%08x", (uint32_t)(node_id & 0xFFFFFFFF));

        // Resolve active region
        std::string primary_region = "";
        for (const auto& r : candidate_regions) {
            if (r.contains(lat, lon)) {
                primary_region = r.name;
                break;
            }
        }

        MeshCorePacket mc_pkt("#QR", sender_hex, "limited", "message");
        mc_pkt.data_text = std::string((char*)payload, length);
        mc_pkt.region = primary_region;

        std::string json_payload = mc_pkt.to_json();

        // Wrap in WormholePacket
        WormholePacket out_pkt;
        out_pkt.version = 1;
        out_pkt.flags = 0;
        out_pkt.source_id = node_id;
        out_pkt.packet_id = next_packet_id++;
        out_pkt.timestamp = millis() / 1000;
        out_pkt.payload.assign(json_payload.begin(), json_payload.end());
        out_pkt.payload_length = out_pkt.payload.size();
        out_pkt.hops.push_back(node_id);

        std::vector<uint8_t> packed = out_pkt.pack();
        wifi.broadcast(packed.data(), packed.size());
    }

    // Process packets received from WiFi Mesh -> Check location scope -> Bridge to local LoRa
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

        // 4. Record hop and transmit over local LoRa serial (if it's a message or command for this region)
        pkt.add_hop(node_id);

        std::string text_to_transmit = mc_pkt.data_text;
        if (text_to_transmit.empty() && !mc_pkt.data_target.empty()) {
            text_to_transmit = mc_pkt.data_light;
        }

        if (!text_to_transmit.empty()) {
            lora.transmit((const uint8_t*)text_to_transmit.c_str(), text_to_transmit.size());
        }

        // Repeat to WiFi mesh to extend coverage
        std::vector<uint8_t> updated_packed = pkt.pack();
        wifi.broadcast(updated_packed.data(), updated_packed.size());
    }

private:
    static LoraBridge* instance;

    static void on_lora_rx_static(const uint8_t* payload, size_t length) {
        if (instance) {
            instance->handle_lora_rx(payload, length);
        }
    }

    static void on_wifi_rx_static(const uint8_t* payload, size_t length, uint64_t from_node) {
        if (instance) {
            instance->handle_wifi_rx(payload, length, from_node);
        }
    }
};

#endif // LORA_BRIDGE_H
