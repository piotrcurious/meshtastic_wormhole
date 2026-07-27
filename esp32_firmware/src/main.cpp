#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <map>
#include <vector>
#include "packet.h"
#include "deduplicator.h"
#include "hardware_lora.h"
#include "multicast_mesh_impl.h"
#include "esp_mesh_wifi.h" // Added native 802.11s hardware mesh config

// Configuration parameters
const char* WIFI_SSID = "WormholeMeshBackbone";
const char* WIFI_PASS = "wormhole_secure_mesh";
uint64_t esp_node_id = 0;
uint32_t next_packet_sequence = 1;

// Power Saving Configuration
#define ENABLE_POWER_SAVING true

// Interface instances
HardwareLora lora_driver1(Serial1, 115200); // Main hardware serial LoRa endpoint
// Support multi-LoRa endpoint setup (e.g., we can register additional LoRa endpoints in the gateway list)
std::vector<LoraInterface*> lora_endpoints;

MulticastMeshImpl wifi_mesh("239.10.10.10", 4403);
EspMeshWifi native_mesh("MWBMES", 1);      // 802.11s hardware mesh fallback
Deduplicator dedup(120);

// Set default routing backend
bool use_native_mesh = false;

// Smart Routing Directory: Maps Meshtastic Node ID (uint32_t) -> Wormhole Node ID (uint64_t)
std::map<uint32_t, uint64_t> routing_table;

// Parse outer Meshtastic packet header (destination and source)
bool parse_meshtastic_header(const uint8_t* payload, size_t length, uint32_t& to_node, uint32_t& from_node) {
    if (length < 12) return false;
    // Unpack first 12 bytes: to_node (4 bytes), from_node (4 bytes), packet_id (4 bytes)
    to_node = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    from_node = payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24);
    return true;
}

// Configure Espressif Power Management API for dynamic frequency scaling (DFS)
void setup_power_management() {
#if ENABLE_POWER_SAVING
    // Configure WiFi Modem Sleep to save power during idle periods
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // Maximize responsive mesh power-save state

    // Dynamic frequency scaling (DFS) configuration
    esp_pm_config_esp32c3_t pm_config;
    pm_config.max_freq_mhz = 160; // Max frequency for processing packets
    pm_config.min_freq_mhz = 80;  // Minimum 80MHz to keep Wi-Fi stack and peripherals active safely
    pm_config.light_sleep_enable = true; // Allow automatic background light sleep

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        Serial.println("Power Management configured successfully (DFS enabled).");
    } else {
        Serial.printf("Power Management configuration failed: %d\n", err);
    }
#endif
}

// Receive from local LoRa antenna -> Forward to WiFi Mesh
void on_lora_rx(const uint8_t* payload, size_t length) {
    Serial.printf("LoRa RX: received payload size %d\n", length);

    uint32_t to_node = 0, from_node = 0;
    uint64_t target_wormhole_id = 0;

    // Smart Routing Discovery
    if (parse_meshtastic_header(payload, length, to_node, from_node)) {
        routing_table[from_node] = esp_node_id;
        Serial.printf("Smart Routing: Mapped Meshtastic Node %x to Local Wormhole %llx\n", from_node, esp_node_id);

        if (routing_table.find(to_node) != routing_table.end()) {
            target_wormhole_id = routing_table[to_node];
            Serial.printf("Smart Routing: Direct path known! Target Wormhole Node %llx\n", target_wormhole_id);
        }
    }

    uint32_t pkt_id = next_packet_sequence++;

    WormholePacket pkt;
    pkt.version = 1;
    pkt.flags = 0;
    pkt.source_id = esp_node_id;
    pkt.packet_id = pkt_id;
    pkt.timestamp = millis() / 1000;
    pkt.payload_length = length;
    pkt.payload.insert(pkt.payload.end(), payload, payload + length);

    // Hops setup
    pkt.add_hop(esp_node_id);
    if (target_wormhole_id != 0) {
        pkt.flags |= 0x02; // Mark as Targeted Routing
        pkt.add_hop(target_wormhole_id);
    }

    // Pack and broadcast over WiFi Mesh
    std::vector<uint8_t> packed = pkt.pack();

    bool sent = false;
    if (use_native_mesh) {
        sent = native_mesh.broadcast(packed.data(), packed.size());
    } else {
        sent = wifi_mesh.broadcast(packed.data(), packed.size());
    }

    if (sent) {
        Serial.printf("WiFi TX: Successfully bridged packet %d to WiFi Mesh\n", pkt_id);
    } else {
        Serial.println("WiFi TX Error: failed to broadcast packet");
    }
}

// Receive from WiFi Mesh -> Decode, Deduplicate & Retransmit on local LoRa
void on_mesh_rx(const uint8_t* payload, size_t length, uint64_t from_node) {
    WormholePacket pkt;
    if (!WormholePacket::unpack(payload, length, pkt)) {
        Serial.println("WiFi RX Error: failed to unpack wormhole packet");
        return;
    }

    Serial.printf("WiFi RX: received packet %d from source %llx\n", pkt.packet_id, pkt.source_id);

    // Smart Routing Discovery
    uint32_t to_node = 0, from_node_mesh = 0;
    if (parse_meshtastic_header(pkt.payload.data(), pkt.payload.size(), to_node, from_node_mesh)) {
        routing_table[from_node_mesh] = pkt.source_id;
        Serial.printf("Smart Routing: Mapped Meshtastic Node %x to Wormhole %llx\n", from_node_mesh, pkt.source_id);
    }

    // 1. Loop prevention check
    if (pkt.has_visited(esp_node_id)) {
        Serial.printf("Loop Prevention: Dropped packet %d - already visited this node\n", pkt.packet_id);
        return;
    }

    // 2. Targeted Routing check
    bool is_targeted = (pkt.flags & 0x02) != 0;
    if (is_targeted && pkt.hops.size() >= 2) {
        uint64_t target_id = pkt.hops[1];
        if (target_id != esp_node_id && pkt.source_id != esp_node_id) {
            Serial.printf("Smart Routing: Bypassing packet %d - targeted for Wormhole %llx\n", pkt.packet_id, target_id);
            return;
        }
    }

    // 3. Deduplication check
    if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
        Serial.printf("Deduplication: Dropped packet %d - already processed\n", pkt.packet_id);
        return;
    }

    // 4. Register hop visit
    pkt.add_hop(esp_node_id);

    // 5. Retransmit payload over all LoRa endpoints
    bool success_any = false;
    for (LoraInterface* lora : lora_endpoints) {
        if (lora->transmit(pkt.payload.data(), pkt.payload.size())) {
            success_any = true;
        }
    }

    if (success_any) {
        Serial.printf("LoRa TX: Successfully retransmitted payload %d on local LoRa network\n", pkt.packet_id);

        // Forward updated packet back to WiFi mesh so other nodes know we visited
        std::vector<uint8_t> updated_packed = pkt.pack();
        if (use_native_mesh) {
            native_mesh.broadcast(updated_packed.data(), updated_packed.size());
        } else {
            wifi_mesh.broadcast(updated_packed.data(), updated_packed.size());
        }
    } else {
        Serial.println("LoRa TX Error: failed to transmit on any local LoRa endpoint");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Initializing ESP32-C3 Meshtastic Wormhole Bridge...");

    // Register active endpoints
    lora_endpoints.push_back(&lora_driver1);

    // Get ESP MAC address to use as unique 64-bit Node ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    esp_node_id = 0;
    for (int i = 0; i < 6; i++) {
        esp_node_id |= ((uint64_t)mac[i] << (8 * i));
    }
    Serial.printf("My Node ID: %llx\n", esp_node_id);

    // Register event listeners on all LoRa endpoints
    for (LoraInterface* lora : lora_endpoints) {
        lora->register_rx_callback(on_lora_rx);
    }
    wifi_mesh.register_rx_callback(on_mesh_rx);
    native_mesh.register_rx_callback(on_mesh_rx);

    // FIX: Initialize the Wi-Fi stack and configure network mode FIRST before configuring/starting MESH APIs
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Try starting 802.11s native hardware mesh
    if (native_mesh.start()) {
        use_native_mesh = true;
        Serial.println("Native 802.11s ESP-MESH initialized and active.");
    } else {
        Serial.println("Native ESP-MESH initialization failed. Falling back to softAP IP Multicast mode...");

        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 15) {
            delay(500);
            Serial.print(".");
            retries++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi connected. Local IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("WiFi connection timed out, continuing in standalone/AP Mesh mode.");
        }

        wifi_mesh.start();
    }

    // Configure power saving & dynamic power scaling
    setup_power_management();

    // Start all lora interfaces
    for (LoraInterface* lora : lora_endpoints) {
        lora->start();
    }

    Serial.println("ESP32-C3 Wormhole Bridge Running.");
}

void loop() {
    for (LoraInterface* lora : lora_endpoints) {
        lora->update();
    }
    if (use_native_mesh) {
        native_mesh.update();
    } else {
        wifi_mesh.update();
    }
    delay(1); // yield to background task processor and allow background light sleep to enter safely
}
