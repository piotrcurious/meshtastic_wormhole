#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <map>
#include <vector>
#include <string>
#include "packet.h"
#include "deduplicator.h"
#include "multicast_mesh_impl.h"
#include "esp_mesh_wifi.h"

// Configuration parameters
const char* WIFI_SSID = "WormholeMeshBackbone";
const char* WIFI_PASS = "wormhole_secure_mesh";
uint64_t esp_node_id = 0;
uint32_t next_packet_id = 1;

// Power Saving Configuration for ESP32-S2
#define ENABLE_POWER_SAVING true

// Hardware Pins Configuration
#define LED_PIN 2           // Built-in LED on ESP32 / ESP32-S2 (or external LED)
#define GPS_RX_PIN 17       // GPS TX connected to S2 GPIO 17
#define GPS_TX_PIN 16       // GPS RX connected to S2 GPIO 16
#define GPS_BAUDRATE 9600   // Standard baud rate for cheap GPS modules (e.g. Neo-6M)

// Hardware Serial 1 for GPS Module
HardwareSerial GpsSerial(1);

// Mesh instances
MulticastMeshImpl wifi_mesh("239.10.10.10", 4403);
EspMeshWifi native_mesh("MWBMES", 1);
Deduplicator dedup(120);

// Set default routing backend
bool use_native_mesh = false;

// Smart Routing Directory: Maps Meshtastic Node ID (uint32_t) -> Wormhole Node ID (uint64_t)
std::map<uint32_t, uint64_t> routing_table;

// GPS Structure & Parser
struct GPSData {
    bool valid = false;
    double latitude = 37.7749;   // Default mock coordinate (San Francisco)
    double longitude = -122.4194; // Default mock coordinate
};

GPSData current_gps;
unsigned long last_gps_adv_time = 0;

class NmeaParser {
private:
    std::string buffer;
public:
    bool encode(char c, GPSData& gps) {
        if (c == '\n' || c == '\r') {
            if (!buffer.empty()) {
                bool res = parse(buffer, gps);
                buffer.clear();
                return res;
            }
            return false;
        }
        if (c == '$') {
            buffer.clear();
        }
        buffer += c;
        if (buffer.length() > 120) { // Safety guard
            buffer.clear();
        }
        return false;
    }

    bool parse(const std::string& line, GPSData& gps) {
        // Parse $GPRMC or $GPGGA sentences
        if (line.rfind("$GPRMC", 0) == 0) {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true) {
                size_t end = line.find(',', start);
                if (end == std::string::npos) {
                    parts.push_back(line.substr(start));
                    break;
                }
                parts.push_back(line.substr(start, end - start));
                start = end + 1;
            }

            if (parts.size() >= 7) {
                std::string status = parts[2];
                if (status == "A") { // Active / Valid Fix
                    gps.latitude = parse_degrees(parts[3], parts[4] == "S");
                    gps.longitude = parse_degrees(parts[5], parts[6] == "W");
                    gps.valid = true;
                    return true;
                }
            }
        } else if (line.rfind("$GPGGA", 0) == 0) {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true) {
                size_t end = line.find(',', start);
                if (end == std::string::npos) {
                    parts.push_back(line.substr(start));
                    break;
                }
                parts.push_back(line.substr(start, end - start));
                start = end + 1;
            }

            if (parts.size() >= 10) {
                std::string fix_quality = parts[6];
                if (fix_quality != "0" && !parts[2].empty()) { // Valid GPS Fix
                    gps.latitude = parse_degrees(parts[2], parts[3] == "S");
                    gps.longitude = parse_degrees(parts[4], parts[5] == "W");
                    gps.valid = true;
                    return true;
                }
            }
        }
        return false;
    }

private:
    double parse_degrees(const std::string& val, bool is_negative) {
        if (val.empty()) return 0.0;
        size_t dot = val.find('.');
        if (dot == std::string::npos || dot < 2) return 0.0;
        std::string deg_str = val.substr(0, dot - 2);
        std::string min_str = val.substr(dot - 2);
        double deg = atof(deg_str.c_str());
        double min = atof(min_str.c_str());
        double total = deg + min / 60.0;
        return is_negative ? -total : total;
    }
};

NmeaParser nmea;

// Parse outer Meshtastic packet header (destination and source)
bool parse_meshtastic_header(const uint8_t* payload, size_t length, uint32_t& to_node, uint32_t& from_node) {
    if (length < 12) return false;
    to_node = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    from_node = payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24);
    return true;
}

// Check incoming JSON packet payload for #QR channel light command targeting this node
void check_light_command(const std::string& msg) {
    if (msg.find("\"channel\"") != std::string::npos && msg.find("#QR") != std::string::npos) {
        char hex_buf[16];
        sprintf(hex_buf, "%08x", (uint32_t)(esp_node_id & 0xFFFFFFFF));
        std::string node_id_hex(hex_buf);

        if (msg.find(node_id_hex) != std::string::npos) {
            if (msg.find("\"light\": \"on\"") != std::string::npos || msg.find("\"light\":\"on\"") != std::string::npos) {
                digitalWrite(LED_PIN, HIGH);
                Serial.println("GPS LED Node S2: Received command -> LED turned ON");
            } else if (msg.find("\"light\": \"off\"") != std::string::npos || msg.find("\"light\":\"off\"") != std::string::npos) {
                digitalWrite(LED_PIN, LOW);
                Serial.println("GPS LED Node S2: Received command -> LED turned OFF");
            }
        }
    }
}

// Configure ESP32-S2 specific Power Management (DFS and low-power Modem sleep)
void setup_power_management() {
#if ENABLE_POWER_SAVING
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_pm_config_esp32s2_t pm_config;
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = true;

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

    // Parse payload as string to inspect for #QR channel light commands
    std::string payload_str((char*)pkt.payload.data(), pkt.payload.size());
    check_light_command(payload_str);

    // Smart Routing Discovery
    uint32_t to_node = 0, from_node_mesh = 0;
    if (parse_meshtastic_header(pkt.payload.data(), pkt.payload.size(), to_node, from_node_mesh)) {
        routing_table[from_node_mesh] = pkt.source_id;
        Serial.printf("Extender Smart Routing: Mapped Meshtastic Node %x to Wormhole %llx\n", from_node_mesh, pkt.source_id);
    }

    // 1. Loop prevention check
    if (pkt.has_visited(esp_node_id)) {
        Serial.printf("Extender Loop Prevention: Dropped packet %d - already visited this node\n", pkt.packet_id);
        return;
    }

    // 2. Targeted Routing check
    bool is_targeted = (pkt.flags & 0x02) != 0;
    if (is_targeted && pkt.hops.size() >= 2) {
        uint64_t target_id = pkt.hops[1];
        if (target_id != esp_node_id && pkt.source_id != esp_node_id) {
            Serial.printf("Extender Smart Routing: Bypassing packet %d - targeted for Wormhole %llx\n", pkt.packet_id, target_id);
            return;
        }
    }

    // 3. Deduplication check
    if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
        Serial.printf("Extender Deduplication: Dropped packet %d - already processed\n", pkt.packet_id);
        return;
    }

    // 4. Register hop visit
    pkt.add_hop(esp_node_id);

    // 5. Forward updated packet back to WiFi mesh to repeat/extend range
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

// Advertise current GPS coordinates on the #QR channel
void advertise_gps_position() {
    char json_buf[256];
    sprintf(json_buf, "{\"channel\":\"#QR\",\"sender\":\"%08x\",\"scope\":\"limited\",\"type\":\"advertise\",\"data\":{\"lat\":%.6f,\"lon\":%.6f}}",
            (uint32_t)(esp_node_id & 0xFFFFFFFF), current_gps.latitude, current_gps.longitude);

    WormholePacket out_pkt;
    out_pkt.version = 1;
    out_pkt.flags = 0;
    out_pkt.source_id = esp_node_id;
    out_pkt.packet_id = next_packet_id++;
    out_pkt.timestamp = millis() / 1000;

    std::string out_str(json_buf);
    out_pkt.payload.assign(out_str.begin(), out_str.end());
    out_pkt.payload_length = out_pkt.payload.size();
    out_pkt.hops.push_back(esp_node_id);

    std::vector<uint8_t> packed = out_pkt.pack();
    bool sent = false;
    if (use_native_mesh) {
        sent = native_mesh.broadcast(packed.data(), packed.size());
    } else {
        sent = wifi_mesh.broadcast(packed.data(), packed.size());
    }

    if (sent) {
        Serial.printf("GPS LED Node S2: Advertised position (%.6f, %.6f) on #QR channel\n", current_gps.latitude, current_gps.longitude);
    } else {
        Serial.println("GPS LED Node S2: Failed to broadcast GPS advertisement");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Initializing ESP32-S2 WiFi Mesh GPS LED Node...");

    // Initialize LED pin
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED OFF initially

    // Initialize Hardware UART for cheap GPS module
    GpsSerial.begin(GPS_BAUDRATE, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("GPS UART Serial started on RX=%d, TX=%d at %d baud.\n", GPS_RX_PIN, GPS_TX_PIN, GPS_BAUDRATE);

    // Get ESP MAC address to use as unique 64-bit Node ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    esp_node_id = 0;
    for (int i = 0; i < 6; i++) {
        esp_node_id |= ((uint64_t)mac[i] << (8 * i));
    }
    Serial.printf("Node ID: %llx\n", esp_node_id);

    // Register event listeners
    wifi_mesh.register_rx_callback(on_mesh_rx);
    native_mesh.register_rx_callback(on_mesh_rx);

    // Initialize the Wi-Fi stack and configure network mode FIRST before configuring/starting MESH APIs
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Try starting 802.11s native hardware mesh first
    if (native_mesh.start()) {
        use_native_mesh = true;
        Serial.println("Native 802.11s ESP-MESH initialized and active.");
    } else {
        Serial.println("Native ESP-MESH failed. Falling back to SoftAP IP Multicast mode...");

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
            Serial.println("WiFi timed out, continuing in softAP multicast fallback mode.");
        }

        wifi_mesh.start();
    }

    // Configure power saving & dynamic power scaling
    setup_power_management();

    Serial.println("ESP32-S2 GPS LED Node Running.");
}

void loop() {
    if (use_native_mesh) {
        native_mesh.update();
    } else {
        wifi_mesh.update();
    }

    // Feed and parse bytes from cheap GPS UART serial
    while (GpsSerial.available() > 0) {
        char c = GpsSerial.read();
        if (nmea.encode(c, current_gps)) {
            Serial.printf("GPS Update parsed: Lat=%.6f, Lon=%.6f (Fix=%s)\n",
                          current_gps.latitude, current_gps.longitude, current_gps.valid ? "YES" : "NO");
        }
    }

    // Periodically advertise position every 5 seconds
    unsigned long now = millis();
    if (now - last_gps_adv_time >= 5000) {
        advertise_gps_position();
        last_gps_adv_time = now;
    }

    delay(1); // yield to background task processor and allow background light sleep to enter safely
}
