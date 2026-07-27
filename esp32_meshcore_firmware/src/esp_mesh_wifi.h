#ifndef ESP_MESH_WIFI_H
#define ESP_MESH_WIFI_H

#include "wifi_mesh_interface.h"

#ifdef ARDUINO
#include <esp_mesh.h>
#include <WiFi.h>
#include <algorithm>

class EspMeshWifi : public WifiMeshInterface {
private:
    mesh_cfg_t config;
    bool is_started;

public:
    EspMeshWifi(const char* mesh_id = "MWB_MESH", uint8_t channel = 1)
        : is_started(false) {
        memset(&config, 0, sizeof(mesh_cfg_t));
        config.channel = channel;
        size_t id_len = strlen(mesh_id);
        size_t copy_len = (id_len < 6) ? id_len : 6;
        memcpy(config.mesh_id.addr, mesh_id, copy_len);
    }

    bool start() override {
        esp_err_t err = esp_mesh_init();
        if (err != ESP_OK) {
            return false;
        }

        esp_mesh_set_ap_assoc_expire(60);
        esp_mesh_set_cfg(&config);

        err = esp_mesh_start();
        if (err != ESP_OK) {
            return false;
        }

        is_started = true;
        return true;
    }

    bool broadcast(const uint8_t* data, size_t length) override {
        if (!is_started) return false;

        mesh_data_t mesh_packet;
        mesh_packet.data = const_cast<uint8_t*>(data);
        mesh_packet.size = length;
        mesh_packet.proto = MESH_PROTO_BIN;
        mesh_packet.tos = MESH_TOS_DEF;

        mesh_addr_t bcast_mac;
        memset(bcast_mac.addr, 0xFF, 6);
        esp_err_t err = esp_mesh_send(&bcast_mac, &mesh_packet, 0, NULL, 0);
        return err == ESP_OK;
    }

    void update() override {
        if (!is_started) return;

        mesh_data_t rx_packet;
        uint8_t rx_buf[1500];
        rx_packet.data = rx_buf;
        rx_packet.size = sizeof(rx_buf);

        int flag = 0;
        mesh_addr_t from;

        esp_err_t err = esp_mesh_recv(&from, &rx_packet, 0, &flag, NULL, 0);
        if (err == ESP_OK && rx_packet.size > 0) {
            uint64_t from_node = 0;
            for (int i = 0; i < 6; i++) {
                from_node |= ((uint64_t)from.addr[i] << (8 * i));
            }
            trigger_rx(rx_buf, rx_packet.size, from_node);
        }
    }
};

#else
// Desktop mock implementation for unit test suites
class EspMeshWifi : public WifiMeshInterface {
public:
    EspMeshWifi(const char* mesh_id = "MWB_MESH", uint8_t channel = 1) {}
    bool start() override { return true; }
    bool broadcast(const uint8_t* data, size_t length) override { return true; }
    void update() override {}
};
#endif

#endif // ESP_MESH_WIFI_H
