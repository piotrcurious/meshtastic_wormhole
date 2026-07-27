#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include "packet.h"
#include "deduplicator.h"
#include "hardware_lora.h"
#include "painless_mesh_impl.h"
#include "esp_mesh_wifi.h" // Added native 802.11s hardware mesh config

// Configuration parameters
const char* WIFI_SSID = "WormholeMeshBackbone";
const char* WIFI_PASS = "wormhole_secure_mesh";
uint64_t esp_node_id = 0;
uint32_t next_packet_sequence = 1;

// Power Saving Configuration
#define ENABLE_POWER_SAVING true
unsigned long last_activity_time = 0;
const unsigned long IDLE_TIMEOUT_MS = 10000; // Go to ultra-low frequency after 10s idle

// Interface instances
HardwareLora lora_driver(Serial1, 115200); // ESP32-C3 standard hardware serial
PainlessMeshImpl wifi_mesh("239.10.10.10", 4403);
EspMeshWifi native_mesh("MWBMES", 1);      // 802.11s hardware mesh fallback
Deduplicator dedup(120);

// Set default routing backend
bool use_native_mesh = false;

// Register activity to postpone sleep/power down
void register_activity() {
    last_activity_time = millis();
}

// Configure Espressif Power Management API for dynamic frequency scaling
void setup_power_management() {
#if ENABLE_POWER_SAVING
    // Configure WiFi Modem Sleep to save power during idle periods
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // Maximize responsive mesh power-save state

    // Dynamic frequency scaling (DFS) configuration
    esp_pm_config_esp32c3_t pm_config;
    pm_config.max_freq_mhz = 160; // Max frequency for processing packets
    pm_config.min_freq_mhz = 10;  // Minimum frequency for ultra low power idle
    pm_config.light_sleep_enable = true; // Allow automatic light sleep

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
    register_activity();
    Serial.printf("LoRa RX: received payload size %d\n", length);

    uint32_t pkt_id = next_packet_sequence++;

    WormholePacket pkt;
    pkt.version = 1;
    pkt.flags = 0;
    pkt.source_id = esp_node_id;
    pkt.packet_id = pkt_id;
    pkt.timestamp = millis() / 1000;
    pkt.payload_length = length;
    pkt.payload.insert(pkt.payload.end(), payload, payload + length);

    // Add local hop
    pkt.add_hop(esp_node_id);

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
    register_activity();
    WormholePacket pkt;
    if (!WormholePacket::unpack(payload, length, pkt)) {
        Serial.println("WiFi RX Error: failed to unpack wormhole packet");
        return;
    }

    Serial.printf("WiFi RX: received packet %d from source %llx\n", pkt.packet_id, pkt.source_id);

    // 1. Loop prevention check
    if (pkt.has_visited(esp_node_id)) {
        Serial.printf("Loop Prevention: Dropped packet %d - already visited this node\n", pkt.packet_id);
        return;
    }

    // 2. Deduplication check
    if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
        Serial.printf("Deduplication: Dropped packet %d - already processed\n", pkt.packet_id);
        return;
    }

    // 3. Register hop visit
    pkt.add_hop(esp_node_id);

    // 4. Retransmit payload over LoRa
    if (lora_driver.transmit(pkt.payload.data(), pkt.payload.size())) {
        Serial.printf("LoRa TX: Successfully retransmitted payload %d on local LoRa network\n", pkt.packet_id);

        // Forward updated packet back to WiFi mesh so other nodes know we visited
        std::vector<uint8_t> updated_packed = pkt.pack();
        if (use_native_mesh) {
            native_mesh.broadcast(updated_packed.data(), updated_packed.size());
        } else {
            wifi_mesh.broadcast(updated_packed.data(), updated_packed.size());
        }
    } else {
        Serial.println("LoRa TX Error: failed to transmit on local LoRa");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Initializing ESP32-C3 Meshtastic Wormhole Bridge...");

    // Get ESP MAC address to use as unique 64-bit Node ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    esp_node_id = 0;
    for (int i = 0; i < 6; i++) {
        esp_node_id |= ((uint64_t)mac[i] << (8 * i));
    }
    Serial.printf("My Node ID: %llx\n", esp_node_id);

    // Register event listeners
    lora_driver.register_rx_callback(on_lora_rx);
    wifi_mesh.register_rx_callback(on_mesh_rx);
    native_mesh.register_rx_callback(on_mesh_rx);

    // Try starting 802.11s native hardware mesh first
    if (native_mesh.start()) {
        use_native_mesh = true;
        Serial.println("Native 802.11s ESP-MESH initialized and active.");
    } else {
        Serial.println("Native ESP-MESH initialization failed. Falling back to softAP IP Multicast mode...");

        // Setup AP-Mesh / Station WiFi connections
        Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(WIFI_SSID, WIFI_PASS);
        WiFi.begin(WIFI_SSID, WIFI_PASS);

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

    // Start lora interfaces
    lora_driver.start();

    register_activity();
    Serial.println("ESP32-C3 Wormhole Bridge Running.");
}

void loop() {
    lora_driver.update();
    if (use_native_mesh) {
        native_mesh.update();
    } else {
        wifi_mesh.update();
    }

    // Handle optional power saving steps when idle
#if ENABLE_POWER_SAVING
    unsigned long now = millis();
    if (now - last_activity_time > IDLE_TIMEOUT_MS) {
        // Drop CPU frequency temporarily to save power
        setCpuFrequencyMhz(10);
        delay(50); // Yield to lower core operations and light sleep triggers
    } else {
        // Return to standard 160MHz for high-speed operation
        setCpuFrequencyMhz(160);
        delay(1);
    }
#else
    delay(1); // yield to background task processor
#endif
}
