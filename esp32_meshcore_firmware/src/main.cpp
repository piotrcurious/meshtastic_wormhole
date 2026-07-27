#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <vector>
#include <string>
#include "packet.h"
#include "deduplicator.h"
#include "multicast_mesh_impl.h"
#include "esp_mesh_wifi.h"
#include "region.h"
#include "meshcore_packet.h"

// ESP32 MeshCore GPS Light Node Firmware Port

// Configuration parameters
const char* WIFI_SSID = "WormholeMeshBackbone";
const char* WIFI_PASS = "wormhole_secure_mesh";
uint64_t esp_node_id = 0;
uint32_t next_packet_id = 1;

// Power Saving Configuration for ESP32
#define ENABLE_POWER_SAVING true

// Hardware Pins Configuration
#define LED_PIN 2           // Built-in LED
#define GPS_RX_PIN 17       // GPS TX connected to GPIO 17
#define GPS_TX_PIN 16       // GPS RX connected to GPIO 16
#define GPS_BAUDRATE 9600   // GPS baud rate

// Hardware Serial 1 for GPS Module
HardwareSerial GpsSerial(1);

// Mesh transport backends
MulticastMeshImpl wifi_mesh("239.10.10.10", 4403);
EspMeshWifi native_mesh("MWBMES", 1);
Deduplicator dedup(120);

// Default routing backend
bool use_native_mesh = false;

// Geographical regions matching meshcore specification
std::vector<Region> candidate_regions;
Region primary_region;
bool has_primary_region = false;

// GPS Parser and tracking
struct GPSData {
    bool valid = false;
    double latitude = 37.7749;   // Default coordinates (San Francisco)
    double longitude = -122.4194;
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

// Resolve regions based on current coordinates
void resolve_active_regions() {
    has_primary_region = false;
    for (const auto& r : candidate_regions) {
        if (r.contains(current_gps.latitude, current_gps.longitude)) {
            primary_region = r;
            has_primary_region = true;
            break;
        }
    }
}

// Process incoming commands based on scope and region filtering
void check_meshcore_command(const MeshCorePacket& pkt) {
    if (pkt.channel != "#QR") return;

    // Resolve which of our candidate regions we reside in
    std::vector<Region> active_regions;
    for (const auto& r : candidate_regions) {
        if (r.contains(current_gps.latitude, current_gps.longitude)) {
            active_regions.push_back(r);
        }
    }

    // Filter geographically and check scope
    if (!pkt.is_valid_for_node(current_gps.latitude, current_gps.longitude, active_regions)) {
        Serial.printf("MeshCore Node %08x: Dropped packet out of region/scope: %s\n",
                      (uint32_t)(esp_node_id & 0xFFFFFFFF), pkt.region.c_str());
        return;
    }

    if (pkt.type == "command") {
        char hex_buf[16];
        sprintf(hex_buf, "%08x", (uint32_t)(esp_node_id & 0xFFFFFFFF));
        std::string node_id_hex(hex_buf);

        if (pkt.data_target == node_id_hex) {
            if (pkt.data_light == "on") {
                digitalWrite(LED_PIN, HIGH);
                Serial.printf("MeshCore Node %s: LED command -> ON (Region: %s, Scope: %s)\n",
                              node_id_hex.c_str(), pkt.region.c_str(), pkt.scope.c_str());
            } else if (pkt.data_light == "off") {
                digitalWrite(LED_PIN, LOW);
                Serial.printf("MeshCore Node %s: LED command -> OFF (Region: %s, Scope: %s)\n",
                              node_id_hex.c_str(), pkt.region.c_str(), pkt.scope.c_str());
            }
        }
    }
}

// Setup power management
void setup_power_management() {
#if ENABLE_POWER_SAVING
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_pm_config_esp32_t pm_config;
    pm_config.max_freq_mhz = 160;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        Serial.println("MeshCore Node: Power Management (DFS) configured successfully.");
    } else {
        Serial.printf("MeshCore Node: Power Management configuration failed: %d\n", err);
    }
#endif
}

// Handle packet from Mesh
void on_mesh_rx(const uint8_t* payload, size_t length, uint64_t from_node) {
    WormholePacket pkt;
    if (!WormholePacket::unpack(payload, length, pkt)) {
        Serial.println("MeshCore Node WiFi RX Error: failed to unpack wormhole packet");
        return;
    }

    Serial.printf("MeshCore Node WiFi RX: received packet %d from source %llx\n", pkt.packet_id, pkt.source_id);

    // Parse payload into MeshCorePacket
    std::string payload_str((char*)pkt.payload.data(), pkt.payload.size());
    MeshCorePacket mc_pkt = MeshCorePacket::from_json(payload_str);

    check_meshcore_command(mc_pkt);

    // 1. Loop prevention check
    if (pkt.has_visited(esp_node_id)) {
        Serial.printf("MeshCore Node Loop Prevention: Dropped packet %d - already visited this node\n", pkt.packet_id);
        return;
    }

    // 2. Deduplication check
    if (dedup.is_duplicate(pkt.source_id, pkt.packet_id)) {
        Serial.printf("MeshCore Node Deduplication: Dropped packet %d - already processed\n", pkt.packet_id);
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
        Serial.printf("MeshCore Node Mesh RX: Successfully repeated/extended packet %d\n", pkt.packet_id);
    } else {
        Serial.println("MeshCore Node Mesh Error: failed to repeat/extend packet");
    }
}

// Advertise coordinates on #QR channel with limited scope
void advertise_gps_position() {
    resolve_active_regions();

    char sender_hex[16];
    sprintf(sender_hex, "%08x", (uint32_t)(esp_node_id & 0xFFFFFFFF));

    MeshCorePacket mc_pkt("#QR", sender_hex, "limited", "advertise");
    mc_pkt.data_lat = current_gps.latitude;
    mc_pkt.data_lon = current_gps.longitude;
    if (has_primary_region) {
        mc_pkt.data_region = primary_region.name;
        mc_pkt.region = primary_region.name;
    } else {
        mc_pkt.data_region = "unknown";
        mc_pkt.region = "";
    }

    std::string json_payload = mc_pkt.to_json();

    WormholePacket out_pkt;
    out_pkt.version = 1;
    out_pkt.flags = 0;
    out_pkt.source_id = esp_node_id;
    out_pkt.packet_id = next_packet_id++;
    out_pkt.timestamp = millis() / 1000;
    out_pkt.payload.assign(json_payload.begin(), json_payload.end());
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
        Serial.printf("MeshCore Node S2: Advertised position (%.6f, %.6f) in region '%s' on #QR channel\n",
                      current_gps.latitude, current_gps.longitude, has_primary_region ? primary_region.name.c_str() : "unknown");
    } else {
        Serial.println("MeshCore Node S2: Failed to broadcast GPS advertisement");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Initializing ESP32 MeshCore GPS LED Node...");

    // Initialize candidate regions
    candidate_regions.push_back(Region("SOMA", 37.7700, -122.4100, 1500.0));
    candidate_regions.push_back(Region("Mission", 37.7500, -122.4100, 1500.0));

    // Initialize LED pin
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED OFF initially

    // Initialize Hardware UART for GPS module
    GpsSerial.begin(GPS_BAUDRATE, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("GPS UART Serial started on RX=%d, TX=%d at %d baud.\n", GPS_RX_PIN, GPS_TX_PIN, GPS_BAUDRATE);

    // Get ESP Node ID from MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    esp_node_id = 0;
    for (int i = 0; i < 6; i++) {
        esp_node_id |= ((uint64_t)mac[i] << (8 * i));
    }
    Serial.printf("Node ID: %llx\n", esp_node_id);

    // Register callbacks
    wifi_mesh.register_rx_callback(on_mesh_rx);
    native_mesh.register_rx_callback(on_mesh_rx);

    // Initialize WiFi
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Start ESP-MESH
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

        wifi_mesh.start();
    }

    setup_power_management();
    Serial.println("MeshCore GPS Light Node fully running.");
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
            Serial.printf("GPS Update: Lat=%.6f, Lon=%.6f (Fix=%s)\n",
                          current_gps.latitude, current_gps.longitude, current_gps.valid ? "YES" : "NO");
        }
    }

    // Periodically advertise position every 5 seconds
    unsigned long now = millis();
    if (now - last_gps_adv_time >= 5000) {
        advertise_gps_position();
        last_gps_adv_time = now;
    }

    delay(1);
}
