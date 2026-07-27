#ifndef MESHCORE_PACKET_H
#define MESHCORE_PACKET_H

#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include "region.h"

class MeshCorePacket {
public:
    std::string channel;
    std::string sender;
    std::string scope; // "limited", "regional", "global"
    std::string type;  // "advertise", "message", "command"
    std::string region; // Optional

    // Data payload fields
    double data_lat = 0.0;
    double data_lon = 0.0;
    std::string data_region;
    std::string data_text;
    std::string data_target;
    std::string data_light;

    MeshCorePacket() : scope("limited") {}

    MeshCorePacket(std::string channel, std::string sender, std::string scope, std::string type, std::string region = "")
        : channel(channel), sender(sender), scope(scope), type(type), region(region) {}

    std::string to_json() const {
        std::string json = "{";
        json += "\"channel\":\"" + channel + "\",";
        json += "\"sender\":\"" + sender + "\",";
        json += "\"scope\":\"" + scope + "\",";
        json += "\"type\":\"" + type + "\",";

        // Build data object
        json += "\"data\":{";
        if (type == "advertise") {
            char coords[64];
            snprintf(coords, sizeof(coords), "\"lat\":%.6f,\"lon\":%.6f", data_lat, data_lon);
            json += coords;
            if (!data_region.empty()) {
                json += ",\"region\":\"" + data_region + "\"";
            }
        } else if (type == "message") {
            json += "\"text\":\"" + data_text + "\"";
        } else if (type == "command") {
            json += "\"target\":\"" + data_target + "\",\"light\":\"" + data_light + "\"";
        }
        json += "}";

        if (!region.empty()) {
            json += ",\"region\":\"" + region + "\"";
        }
        json += "}";
        return json;
    }

    static std::string extract_value(const std::string& json, const std::string& key) {
        size_t key_pos = json.find("\"" + key + "\"");
        if (key_pos == std::string::npos) return "";
        size_t colon_pos = json.find(":", key_pos);
        if (colon_pos == std::string::npos) return "";

        // Find value start
        size_t val_start = colon_pos + 1;
        while (val_start < json.length() && (json[val_start] == ' ' || json[val_start] == '\t')) {
            val_start++;
        }

        if (val_start >= json.length()) return "";

        if (json[val_start] == '\"') {
            // String value
            size_t val_end = json.find("\"", val_start + 1);
            if (val_end == std::string::npos) return "";
            return json.substr(val_start + 1, val_end - val_start - 1);
        } else {
            // Numeric or object/boolean value
            size_t val_end = val_start;
            while (val_end < json.length() && json[val_end] != ',' && json[val_end] != '}' && json[val_end] != ']') {
                val_end++;
            }
            return json.substr(val_start, val_end - val_start);
        }
    }

    static MeshCorePacket from_json(const std::string& json_str) {
        MeshCorePacket pkt;
        pkt.channel = extract_value(json_str, "channel");
        pkt.sender = extract_value(json_str, "sender");
        pkt.scope = extract_value(json_str, "scope");
        pkt.type = extract_value(json_str, "type");
        pkt.region = extract_value(json_str, "region");

        // Parse inner data object if present
        size_t data_pos = json_str.find("\"data\"");
        if (data_pos != std::string::npos) {
            size_t sub_start = json_str.find("{", data_pos);
            size_t sub_end = json_str.find("}", data_pos);
            if (sub_start != std::string::npos && sub_end != std::string::npos && sub_end > sub_start) {
                std::string data_sub = json_str.substr(sub_start, sub_end - sub_start + 1);
                pkt.data_text = extract_value(data_sub, "text");
                pkt.data_target = extract_value(data_sub, "target");
                pkt.data_light = extract_value(data_sub, "light");
                pkt.data_region = extract_value(data_sub, "region");

                std::string lat_val = extract_value(data_sub, "lat");
                if (!lat_val.empty()) pkt.data_lat = atof(lat_val.c_str());

                std::string lon_val = extract_value(data_sub, "lon");
                if (!lon_val.empty()) pkt.data_lon = atof(lon_val.c_str());
            }
        }

        if (pkt.scope.empty()) pkt.scope = "limited";
        return pkt;
    }

    bool is_valid_for_node(double node_lat, double node_lon, const std::vector<Region>& node_regions) const {
        if (scope == "global") {
            return true;
        }

        if (scope == "regional" && !region.empty()) {
            // Check if node belongs to the target region
            for (const auto& r : node_regions) {
                if (r.name == region) {
                    return true;
                }
            }
            return false;
        }

        // For 'limited' or default, always process
        return true;
    }
};

#endif // MESHCORE_PACKET_H
