#ifndef LORA_INTERFACE_H
#define LORA_INTERFACE_H

#include <vector>
#include <cstddef>
#include <cstdint>

typedef void (*LoraRxCallback)(const uint8_t* payload, size_t length);

class LoraInterface {
protected:
    LoraRxCallback rx_callback;

public:
    LoraInterface() : rx_callback(nullptr) {}

    void register_rx_callback(LoraRxCallback cb) {
        rx_callback = cb;
    }

    virtual bool start() = 0;
    virtual bool transmit(const uint8_t* data, size_t length) = 0;
    virtual void update() = 0;

protected:
    void trigger_rx(const uint8_t* data, size_t length) {
        if (rx_callback) {
            rx_callback(data, length);
        }
    }
};

#endif // LORA_INTERFACE_H
