#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <string>
#include <cmath>
#include <map>
#include <algorithm>

// Define Arduino-specific mocks so headers can compile cleanly on desktop
#define Arduino_h

#include "../src/region.h"
#include "../src/meshcore_packet.h"
#include "../src/packet.h"
#include "../src/deduplicator.h"
#include "../src/lora_bridge.h"
#include "../src/extender.h"

// Test double implementations to capture bridge behavior
class TestWifi : public WifiMeshInterface {
public:
    std::vector<std::vector<uint8_t>> broadcasts;

    bool start() override { return true; }
    bool broadcast(const uint8_t* data, size_t length) override {
        broadcasts.push_back(std::vector<uint8_t>(data, data + length));
        return true;
    }
    void update() override {}

    void simulate_rx(const uint8_t* data, size_t length, uint64_t from_node) {
        trigger_rx(data, length, from_node);
    }
};

class TestLora : public LoraInterface {
public:
    std::vector<std::string> transmissions;

    bool start() override { return true; }
    bool transmit(const uint8_t* data, size_t length) override {
        transmissions.push_back(std::string((char*)data, length));
        return true;
    }
    void update() override {}

    void simulate_rx(const uint8_t* data, size_t length) {
        trigger_rx(data, length);
    }
};

// C++ Implementation of CoordinatorNode for geographical grid sorting and routing verification
class TestCoordinator {
public:
    std::vector<Region> regions;
    // Map of region name -> map of node ID (hex string) -> pair of (latitude, longitude)
    std::map<std::string, std::map<std::string, std::pair<double, double>>> regional_nodes;
    // Map of region name -> 2D grid of node IDs
    std::map<std::string, std::vector<std::vector<std::string>>> regional_grids;

    TestCoordinator(const std::vector<Region>& r_list) : regions(r_list) {
        for (const auto& r : regions) {
            regional_nodes[r.name] = std::map<std::string, std::pair<double, double>>();
            regional_grids[r.name] = std::vector<std::vector<std::string>>();
        }
        regional_nodes["unknown"] = std::map<std::string, std::pair<double, double>>();
    }

    void register_node(const std::string& node_id, double lat, double lon) {
        std::string assigned_region = "unknown";
        for (const auto& r : regions) {
            if (r.contains(lat, lon)) {
                assigned_region = r.name;
                break;
            }
        }
        regional_nodes[assigned_region][node_id] = {lat, lon};
        sort_region_into_grid(assigned_region);
    }

    void sort_region_into_grid(const std::string& region_name) {
        const auto& nodes_map = regional_nodes[region_name];
        size_t num_nodes = nodes_map.size();
        if (num_nodes == 0) {
            regional_grids[region_name] = std::vector<std::vector<std::string>>();
            return;
        }

        // Calculate grid size
        size_t cols = std::ceil(std::sqrt(num_nodes));
        size_t rows = std::ceil((double)num_nodes / cols);

        // Sort all nodes by latitude descending (North to South)
        std::vector<std::pair<std::string, std::pair<double, double>>> sorted_by_lat(nodes_map.begin(), nodes_map.end());
        std::sort(sorted_by_lat.begin(), sorted_by_lat.end(), [](const auto& a, const auto& b) {
            return a.second.first > b.second.first;
        });

        std::vector<std::vector<std::string>> grid;
        for (size_t r = 0; r < rows; r++) {
            size_t start_idx = r * cols;
            size_t end_idx = std::min(start_idx + cols, num_nodes);

            std::vector<std::pair<std::string, std::pair<double, double>>> row_items;
            for (size_t i = start_idx; i < end_idx; i++) {
                row_items.push_back(sorted_by_lat[i]);
            }

            // Sort row items by longitude ascending (West to East)
            std::sort(row_items.begin(), row_items.end(), [](const auto& a, const auto& b) {
                return a.second.second < b.second.second;
            });

            std::vector<std::string> row_node_ids;
            for (const auto& item : row_items) {
                row_node_ids.push_back(item.first);
            }

            // Pad row
            while (row_node_ids.size() < cols) {
                row_node_ids.push_back("");
            }
            grid.push_back(row_node_ids);
        }

        regional_grids[region_name] = grid;
    }
};

void test_haversine_distance_and_region_contains() {
    Region soma("SOMA", 37.7700, -122.4100, 1000.0); // 1000m radius
    assert(soma.contains(37.771, -122.411) == true);
    assert(soma.contains(37.950, -122.100) == false);
    std::cout << "test_haversine_distance_and_region_contains: PASSED" << std::endl;
}

void test_meshcore_packet_geographical_filtering() {
    Region soma("SOMA", 37.7700, -122.4100, 1000.0);
    Region mission("Mission", 37.7500, -122.4100, 1000.0);

    MeshCorePacket p_global("#QR", "0000eeee", "global", "message");
    p_global.data_text = "GLOBAL CHAT";
    assert(p_global.is_valid_for_node(37.771, -122.411, {soma}) == true);

    MeshCorePacket p_regional("#QR", "0000eeee", "regional", "message", "Mission");
    p_regional.data_text = "MISSION CHAT";
    assert(p_regional.is_valid_for_node(37.771, -122.411, {soma}) == false);
    assert(p_regional.is_valid_for_node(37.751, -122.411, {mission}) == true);

    std::cout << "test_meshcore_packet_geographical_filtering: PASSED" << std::endl;
}

void test_meshcore_packet_serialization_deserialization() {
    MeshCorePacket pkt("#QR", "00001234", "regional", "command", "SOMA");
    pkt.data_target = "00005678";
    pkt.data_light = "on";

    std::string json_str = pkt.to_json();
    assert(json_str.find("\"channel\":\"#QR\"") != std::string::npos);
    assert(json_str.find("\"sender\":\"00001234\"") != std::string::npos);
    assert(json_str.find("\"scope\":\"regional\"") != std::string::npos);
    assert(json_str.find("\"type\":\"command\"") != std::string::npos);
    assert(json_str.find("\"region\":\"SOMA\"") != std::string::npos);

    MeshCorePacket unpacked = MeshCorePacket::from_json(json_str);
    assert(unpacked.channel == "#QR");
    assert(unpacked.sender == "00001234");
    assert(unpacked.scope == "regional");
    assert(unpacked.type == "command");
    assert(unpacked.region == "SOMA");
    assert(unpacked.data_target == "00005678");
    assert(unpacked.data_light == "on");

    std::cout << "test_meshcore_packet_serialization_deserialization: PASSED" << std::endl;
}

void test_coordinator_multi_region_grid_sorting() {
    Region soma("SOMA", 37.7700, -122.4100, 1500.0);
    Region mission("Mission", 37.7500, -122.4100, 1500.0);
    TestCoordinator coordinator({soma, mission});

    coordinator.register_node("soma_node_2", 37.772, -122.408);
    coordinator.register_node("soma_node_1", 37.772, -122.412);

    coordinator.register_node("mission_node_1", 37.752, -122.412);
    coordinator.register_node("mission_node_2", 37.752, -122.408);

    const auto& soma_grid = coordinator.regional_grids["SOMA"];
    assert(soma_grid.size() == 1);
    assert(soma_grid[0][0] == "soma_node_1");
    assert(soma_grid[0][1] == "soma_node_2");

    const auto& mission_grid = coordinator.regional_grids["Mission"];
    assert(mission_grid.size() == 1);
    assert(mission_grid[0][0] == "mission_node_1");
    assert(mission_grid[0][1] == "mission_node_2");

    std::cout << "test_coordinator_multi_region_grid_sorting: PASSED" << std::endl;
}

void test_lora_bridge_routing() {
    Region soma("SOMA", 37.7700, -122.4100, 1500.0);
    Region mission("Mission", 37.7500, -122.4100, 1500.0);
    std::vector<Region> regions = {soma, mission};

    TestLora lora;
    TestWifi wifi;
    Deduplicator dedup(120);
    uint32_t packet_counter = 1;

    // Bridge node 0xABCD located in SOMA
    LoraBridge bridge(lora, wifi, dedup, 0xABCD, packet_counter, 37.7720, -122.4120, regions);
    bool ok = bridge.start();
    assert(ok == true);

    // 1. Simulate local LoRa message transmit -> Bridge should encapsulate in MeshCorePacket and send over WiFi mesh
    bridge.handle_lora_rx((const uint8_t*)"Hello Bridge", 12);
    assert(wifi.broadcasts.size() == 1);

    WormholePacket pkt;
    bool unpacked = WormholePacket::unpack(wifi.broadcasts[0].data(), wifi.broadcasts[0].size(), pkt);
    assert(unpacked == true);
    assert(pkt.source_id == 0xABCD);

    std::string payload_str((char*)pkt.payload.data(), pkt.payload.size());
    MeshCorePacket mc_pkt = MeshCorePacket::from_json(payload_str);
    assert(mc_pkt.channel == "#QR");
    assert(mc_pkt.scope == "limited");
    assert(mc_pkt.data_text == "Hello Bridge");
    assert(mc_pkt.region == "SOMA"); // Bridge correctly resolved itself in SOMA

    // 2. Simulate incoming regional packet on WiFi targeting "SOMA" -> Bridge should forward to local LoRa serial
    MeshCorePacket in_soma_pkt("#QR", "0000eeee", "regional", "message", "SOMA");
    in_soma_pkt.data_text = "WELCOME TO SOMA";
    std::string soma_json = in_soma_pkt.to_json();

    WormholePacket wifi_in_pkt;
    wifi_in_pkt.version = 1;
    wifi_in_pkt.flags = 0;
    wifi_in_pkt.source_id = 0x9999;
    wifi_in_pkt.packet_id = 456;
    wifi_in_pkt.timestamp = 1000;
    wifi_in_pkt.payload.assign(soma_json.begin(), soma_json.end());
    wifi_in_pkt.payload_length = wifi_in_pkt.payload.size();
    wifi_in_pkt.hops.push_back(0x9999);

    std::vector<uint8_t> packed_wifi_in = wifi_in_pkt.pack();
    wifi.simulate_rx(packed_wifi_in.data(), packed_wifi_in.size(), 0x9999);

    assert(lora.transmissions.size() == 1);
    assert(lora.transmissions[0] == "WELCOME TO SOMA");

    // 3. Simulate incoming regional packet on WiFi targeting "Mission" -> SOMA Bridge should ignore / filter it
    MeshCorePacket in_mission_pkt("#QR", "0000eeee", "regional", "message", "Mission");
    in_mission_pkt.data_text = "WELCOME TO MISSION";
    std::string mission_json = in_mission_pkt.to_json();

    WormholePacket wifi_in_pkt2;
    wifi_in_pkt2.version = 1;
    wifi_in_pkt2.flags = 0;
    wifi_in_pkt2.source_id = 0x9999;
    wifi_in_pkt2.packet_id = 457; // New ID
    wifi_in_pkt2.timestamp = 1000;
    wifi_in_pkt2.payload.assign(mission_json.begin(), mission_json.end());
    wifi_in_pkt2.payload_length = wifi_in_pkt2.payload.size();
    wifi_in_pkt2.hops.push_back(0x9999);

    std::vector<uint8_t> packed_wifi_in2 = wifi_in_pkt2.pack();
    wifi.simulate_rx(packed_wifi_in2.data(), packed_wifi_in2.size(), 0x9999);

    // Transmissions count should remain 1 (ignored the Mission packet)
    assert(lora.transmissions.size() == 1);

    std::cout << "test_lora_bridge_routing: PASSED" << std::endl;
}

void test_extender_routing() {
    Region soma("SOMA", 37.7700, -122.4100, 1500.0);
    Region mission("Mission", 37.7500, -122.4100, 1500.0);
    std::vector<Region> regions = {soma, mission};

    TestWifi wifi;
    Deduplicator dedup(120);

    // Extender located in SOMA
    Extender extender(wifi, dedup, 0x5555, 37.7720, -122.4120, regions);
    bool ok = extender.start();
    assert(ok == true);

    // 1. Simulate receiving regional "SOMA" packet from WiFi -> Extender in SOMA should repeat it
    MeshCorePacket soma_pkt("#QR", "0000eeee", "regional", "message", "SOMA");
    soma_pkt.data_text = "SOMA ALERT";
    std::string soma_json = soma_pkt.to_json();

    WormholePacket wifi_pkt;
    wifi_pkt.version = 1;
    wifi_pkt.flags = 0;
    wifi_pkt.source_id = 0x1111;
    wifi_pkt.packet_id = 777;
    wifi_pkt.timestamp = 2000;
    wifi_pkt.payload.assign(soma_json.begin(), soma_json.end());
    wifi_pkt.payload_length = wifi_pkt.payload.size();
    wifi_pkt.hops.push_back(0x1111);

    std::vector<uint8_t> packed_wifi = wifi_pkt.pack();
    wifi.simulate_rx(packed_wifi.data(), packed_wifi.size(), 0x1111);

    assert(wifi.broadcasts.size() == 1); // Repeated!

    WormholePacket repeated_pkt;
    bool unpacked = WormholePacket::unpack(wifi.broadcasts[0].data(), wifi.broadcasts[0].size(), repeated_pkt);
    assert(unpacked == true);
    assert(repeated_pkt.has_visited(0x5555) == true); // Hop added!

    // 2. Simulate receiving regional "Mission" packet -> SOMA Extender should ignore/filter it
    MeshCorePacket mission_pkt("#QR", "0000eeee", "regional", "message", "Mission");
    mission_pkt.data_text = "MISSION ALERT";
    std::string mission_json = mission_pkt.to_json();

    WormholePacket wifi_pkt2;
    wifi_pkt2.version = 1;
    wifi_pkt2.flags = 0;
    wifi_pkt2.source_id = 0x1111;
    wifi_pkt2.packet_id = 778; // New ID
    wifi_pkt2.timestamp = 2000;
    wifi_pkt2.payload.assign(mission_json.begin(), mission_json.end());
    wifi_pkt2.payload_length = wifi_pkt2.payload.size();
    wifi_pkt2.hops.push_back(0x1111);

    std::vector<uint8_t> packed_wifi2 = wifi_pkt2.pack();
    wifi.simulate_rx(packed_wifi2.data(), packed_wifi2.size(), 0x1111);

    // Broadcasts count should remain 1 (ignored/filtered the Mission packet)
    assert(wifi.broadcasts.size() == 1);

    std::cout << "test_extender_routing: PASSED" << std::endl;
}

int main() {
    std::cout << "Running Desktop C++ MeshCore Verification Tests..." << std::endl;
    test_haversine_distance_and_region_contains();
    test_meshcore_packet_geographical_filtering();
    test_meshcore_packet_serialization_deserialization();
    test_coordinator_multi_region_grid_sorting();
    test_lora_bridge_routing();
    test_extender_routing();
    std::cout << "All Desktop C++ MeshCore Verification Tests PASSED!" << std::endl;
    return 0;
}
