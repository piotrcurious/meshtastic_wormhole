#ifndef WIFI_MESH_INTERFACE_H
#define WIFI_MESH_INTERFACE_H

#include <vector>
#include <cstddef>
#include <cstdint>

typedef void (*MeshRxCallback)(const uint8_t* payload, size_t length, uint64_t from_node);

class WifiMeshInterface {
protected:
    MeshRxCallback rx_callback;

public:
    WifiMeshInterface() : rx_callback(nullptr) {}

    void register_rx_callback(MeshRxCallback cb) {
        rx_callback = cb;
    }

    virtual bool start() = 0;
    virtual bool broadcast(const uint8_t* data, size_t length) = 0;
    virtual void update() = 0;

protected:
    void trigger_rx(const uint8_t* data, size_t length, uint64_t from_node) {
        if (rx_callback) {
            rx_callback(data, length, from_node);
        }
    }
};

#endif // WIFI_MESH_INTERFACE_H
