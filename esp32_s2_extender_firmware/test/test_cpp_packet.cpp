#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <string>
#include <cmath>

// Lightweight mocks for Arduino environments so we can compile and test under standard desktop g++
#define Arduino_h
class String {
public:
    String() {}
    String(const char*) {}
    const char* c_str() const { return ""; }
};
inline uint32_t millis() { return 1000; }

// Inject packet.h
#include "packet.h"

// Copy the NMEA Parser classes here so they are directly unit testable on desktop
struct GPSData {
    bool valid = false;
    double latitude = 37.7749;
    double longitude = -122.4194;
};

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

void test_cpp_packet_serialization() {
    WormholePacket pkt;
    pkt.version = 1;
    pkt.flags = 0;
    pkt.source_id = 0x1122334455667788ULL;
    pkt.packet_id = 12345;
    pkt.timestamp = 987654321;
    pkt.payload_length = 5;
    pkt.payload = {'H', 'e', 'l', 'l', 'o'};
    pkt.hops = {0xAAAABBBBCCCCDDDDUll};

    std::vector<uint8_t> packed = pkt.pack();
    assert(packed.size() == 26 + 2 + 8 + 5);

    WormholePacket unpacked;
    bool success = WormholePacket::unpack(packed.data(), packed.size(), unpacked);
    assert(success == true);

    assert(unpacked.version == pkt.version);
    assert(unpacked.flags == pkt.flags);
    assert(unpacked.source_id == pkt.source_id);
    assert(unpacked.packet_id == pkt.packet_id);
    assert(unpacked.timestamp == pkt.timestamp);
    assert(unpacked.payload_length == pkt.payload_length);
    assert(unpacked.payload == pkt.payload);
    assert(unpacked.hops == pkt.hops);

    std::cout << "test_cpp_packet_serialization: PASSED" << std::endl;
}

void test_cpp_packet_bounds_checks() {
    uint8_t malformed[27];
    memset(malformed, 0, sizeof(malformed));
    malformed[0] = 'W'; malformed[1] = 'H'; malformed[2] = 'O'; malformed[3] = 'l';

    WormholePacket unpacked;
    bool success = WormholePacket::unpack(malformed, sizeof(malformed), unpacked);
    assert(success == false);

    malformed[3] = 'L';
    success = WormholePacket::unpack(malformed, 20, unpacked);
    assert(success == false);

    malformed[6] = 40;
    success = WormholePacket::unpack(malformed, 27, unpacked);
    assert(success == false);

    std::cout << "test_cpp_packet_bounds_checks: PASSED" << std::endl;
}

void test_nmea_gps_parser() {
    NmeaParser parser;
    GPSData gps;

    // 1. Test standard valid $GPRMC NMEA sentence
    std::string sentence_rmc = "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\n";
    for (char c : sentence_rmc) {
        parser.encode(c, gps);
    }
    assert(gps.valid == true);
    // 49 degrees + 16.45 minutes = 49 + 16.45/60 = 49.274167
    assert(std::abs(gps.latitude - 49.274167) < 1e-4);
    // 123 degrees + 11.12 minutes = 123 + 11.12/60 = 123.185333 (West hemisphire -> negative)
    assert(std::abs(gps.longitude - (-123.185333)) < 1e-4);

    // 2. Test standard valid $GPGGA NMEA sentence
    std::string sentence_gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\n";
    for (char c : sentence_gga) {
        parser.encode(c, gps);
    }
    assert(gps.valid == true);
    // 48 + 7.038/60 = 48.1173
    assert(std::abs(gps.latitude - 48.1173) < 1e-4);
    // 11 + 31/60 = 11.516667
    assert(std::abs(gps.longitude - 11.516667) < 1e-4);

    // 3. Test invalid RMC sentence status
    std::string sentence_rmc_invalid = "$GPRMC,225446,V,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68\n";
    gps.valid = false;
    for (char c : sentence_rmc_invalid) {
        parser.encode(c, gps);
    }
    assert(gps.valid == false);

    std::cout << "test_nmea_gps_parser: PASSED" << std::endl;
}

int main() {
    test_cpp_packet_serialization();
    test_cpp_packet_bounds_checks();
    test_nmea_gps_parser();
    std::cout << "All C++ verification tests on ESP32-S2 Extender PASSED!" << std::endl;
    return 0;
}
