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
    // SOMA center near 37.77, -122.41
    Region soma("SOMA", 37.7700, -122.4100, 1000.0); // 1000m radius

    // Coordinates close (~220m) should be inside SOMA
    assert(soma.contains(37.771, -122.411) == true);

    // Coordinates far away should be outside
    assert(soma.contains(37.950, -122.100) == false);

    std::cout << "test_haversine_distance_and_region_contains: PASSED" << std::endl;
}

void test_meshcore_packet_geographical_filtering() {
    Region soma("SOMA", 37.7700, -122.4100, 1000.0);
    Region mission("Mission", 37.7500, -122.4100, 1000.0);
    std::vector<Region> regions = {soma, mission};

    // Global scope packet should be valid on a SOMA node
    MeshCorePacket p_global("#QR", "0000eeee", "global", "message");
    p_global.data_text = "GLOBAL CHAT";
    assert(p_global.is_valid_for_node(37.771, -122.411, {soma}) == true);

    // Regional scope packet targeting "Mission" should NOT be valid on SOMA coordinates
    MeshCorePacket p_regional("#QR", "0000eeee", "regional", "message", "Mission");
    p_regional.data_text = "MISSION CHAT";
    assert(p_regional.is_valid_for_node(37.771, -122.411, {soma}) == false);

    // But should be valid on Mission coordinates
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
    assert(json_str.find("\"target\":\"00005678\"") != std::string::npos);
    assert(json_str.find("\"light\":\"on\"") != std::string::npos);

    // Parse back
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

    // Register SOMA nodes
    coordinator.register_node("soma_node_2", 37.772, -122.408);
    coordinator.register_node("soma_node_1", 37.772, -122.412);

    // Register Mission nodes
    coordinator.register_node("mission_node_1", 37.752, -122.412);
    coordinator.register_node("mission_node_2", 37.752, -122.408);

    // Verify grid sorting: SOMA row 0 should contain "soma_node_1" then "soma_node_2" (West to East)
    const auto& soma_grid = coordinator.regional_grids["SOMA"];
    assert(soma_grid.size() == 1);
    assert(soma_grid[0][0] == "soma_node_1");
    assert(soma_grid[0][1] == "soma_node_2");

    // Verify grid sorting: Mission row 0 should contain "mission_node_1" then "mission_node_2" (West to East)
    const auto& mission_grid = coordinator.regional_grids["Mission"];
    assert(mission_grid.size() == 1);
    assert(mission_grid[0][0] == "mission_node_1");
    assert(mission_grid[0][1] == "mission_node_2");

    std::cout << "test_coordinator_multi_region_grid_sorting: PASSED" << std::endl;
}

int main() {
    std::cout << "Running Desktop C++ MeshCore Verification Tests..." << std::endl;
    test_haversine_distance_and_region_contains();
    test_meshcore_packet_geographical_filtering();
    test_meshcore_packet_serialization_deserialization();
    test_coordinator_multi_region_grid_sorting();
    std::cout << "All Desktop C++ MeshCore Verification Tests PASSED!" << std::endl;
    return 0;
}
