#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include "packet.h"
#include "deduplicator.h"
#include "multicast_mesh_impl.h"
#include "esp_mesh_wifi.h"

// Configuration parameters
const char* WIFI_SSID = "WormholeMeshBackbone";
const char* WIFI_PASS = "wormhole_secure_mesh";
uint64_t esp_node_id = 0;

// Power Saving Configuration for ESP32-S2
#define ENABLE_POWER_SAVING true

// Mesh instances
MulticastMeshImpl wifi_mesh("239.10.10.10", 4403);
EspMeshWifi native_mesh("MWBMES", 1);
Deduplicator dedup(120);

// Set default routing backend
bool use_native_mesh = false;

// Configure ESP32-S2 specific Power Management (DFS and low-power Modem sleep)
void setup_power_management() {
#if ENABLE_POWER_SAVING
    // Configure WiFi Modem Sleep
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // ESP32-S2 Dynamic Frequency Scaling configuration
    esp_pm_config_esp32s2_t pm_config;
    pm_config.max_freq_mhz = 240; // Max frequency for processing packets on S2
    pm_config.min_freq_mhz = 80;  // Minimum 80MHz to keep Wi-Fi stack and peripherals active safely
    pm_config.light_sleep_enable = true; // Auto light sleep triggers

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        Serial.println("ESP32-S2 Power Management (DFS) configured successfully.");
    } else {
        Serial.printf("ESP32-S2 Power Management configuration failed: %d\n", err);
    }
#endif
}

// Receive from WiFi Mesh -> Check, Register Hop/Visit, and forward to repeat/extend coverage
void on_mesh_rx(const uint8_t* payload, size_t length, uint64_t from_node) {
    WormholePacket pkt;
    if (!WormholePacket::unpack(payload, length, pkt)) {
        Serial.println("Extender WiFi RX Error: failed to unpack wormhole packet");
        return;
    }

    Serial.printf("Extender WiFi RX: received packet %d from source %llx\n", pkt.packet_id, pkt.source_id);

    // 1. Loop prevention check
    if (pkt.has_visited(esp_node_id)) {
        Serial.printf("Extender Loop Prevention: Dropped packet %d - already visited this node\n", pkt.packet_id);
        return;
    }

    // 2. Deduplication check
    if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
        Serial.printf("Extender Deduplication: Dropped packet %d - already processed\n", pkt.packet_id);
        return;
    }

    // 3. Register hop visit
    pkt.add_hop(esp_node_id);

    // 4. Forward updated packet back to WiFi mesh to repeat/extend range
    std::vector<uint8_t> updated_packed = pkt.pack();

    bool sent = false;
    if (use_native_mesh) {
        sent = native_mesh.broadcast(updated_packed.data(), updated_packed.size());
    } else {
        sent = wifi_mesh.broadcast(updated_packed.data(), updated_packed.size());
    }

    if (sent) {
        Serial.printf("Extender Mesh RX: Successfully repeated/extended packet %d\n", pkt.packet_id);
    } else {
        Serial.println("Extender Mesh Error: failed to repeat/extend packet");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Initializing ESP32-S2 WiFi Mesh Extender...");

    // Get ESP MAC address to use as unique 64-bit Node ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    esp_node_id = 0;
    for (int i = 0; i < 6; i++) {
        esp_node_id |= ((uint64_t)mac[i] << (8 * i));
    }
    Serial.printf("Extender Node ID: %llx\n", esp_node_id);

    // Register event listeners
    wifi_mesh.register_rx_callback(on_mesh_rx);
    native_mesh.register_rx_callback(on_mesh_rx);

    // FIX: Initialize the Wi-Fi stack and configure network mode FIRST before configuring/starting MESH APIs
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Try starting 802.11s native hardware mesh first
    if (native_mesh.start()) {
        use_native_mesh = true;
        Serial.println("Native 802.11s ESP-MESH initialized and active on Extender.");
    } else {
        Serial.println("Native ESP-MESH failed on Extender. Falling back to SoftAP IP Multicast mode...");

        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 15) {
            delay(500);
            Serial.print(".");
            retries++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Extender WiFi connected. Local IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("Extender WiFi timed out, continuing in softAP multicast fallback mode.");
        }

        wifi_mesh.start();
    }

    // Configure power saving & dynamic power scaling
    setup_power_management();

    Serial.println("ESP32-S2 Extender Running.");
}

void loop() {
    if (use_native_mesh) {
        native_mesh.update();
    } else {
        wifi_mesh.update();
    }
    delay(1); // yield to background task processor and allow background light sleep to enter safely
}
