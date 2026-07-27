#ifndef HARDWARE_LORA_H
#define HARDWARE_LORA_H

#include "lora_interface.h"

#ifdef ARDUINO
#include <Arduino.h>

class HardwareLora : public LoraInterface {
private:
    HardwareSerial& serial_port;
    uint32_t baud;
    std::vector<uint8_t> rx_buffer;
    bool in_escape;

    const uint8_t SLIP_END = 0xC0;
    const uint8_t SLIP_ESC = 0xDB;
    const uint8_t SLIP_ESC_END = 0xDC;
    const uint8_t SLIP_ESC_ESC = 0xDD;

public:
    HardwareLora(HardwareSerial& port, uint32_t baudrate = 115200)
        : serial_port(port), baud(baudrate), in_escape(false) {}

    bool start() override {
        serial_port.begin(baud);
        return true;
    }

    bool transmit(const uint8_t* data, size_t length) override {
        // Standard SLIP frame encoding
        serial_port.write(SLIP_END);
        for (size_t i = 0; i < length; i++) {
            uint8_t byte = data[i];
            if (byte == SLIP_END) {
                serial_port.write(SLIP_ESC);
                serial_port.write(SLIP_ESC_END);
            } else if (byte == SLIP_ESC) {
                serial_port.write(SLIP_ESC);
                serial_port.write(SLIP_ESC_ESC);
            } else {
                serial_port.write(byte);
            }
        }
        serial_port.write(SLIP_END);
        return true;
    }

    void update() override {
        while (serial_port.available()) {
            uint8_t byte = serial_port.read();
            if (byte == SLIP_END) {
                if (!rx_buffer.empty()) {
                    trigger_rx(rx_buffer.data(), rx_buffer.size());
                    rx_buffer.clear();
                }
                in_escape = false;
            } else if (byte == SLIP_ESC) {
                in_escape = true;
            } else if (in_escape) {
                // Safeguard: Cap buffer size to prevent out-of-memory under physical serial noise
                if (rx_buffer.size() < 1024) {
                    if (byte == SLIP_ESC_END) {
                        rx_buffer.push_back(SLIP_END);
                    } else if (byte == SLIP_ESC_ESC) {
                        rx_buffer.push_back(SLIP_ESC);
                    } else {
                        rx_buffer.push_back(byte);
                    }
                } else {
                    rx_buffer.clear(); // Overflow, discard corrupted frame
                }
                in_escape = false;
            } else {
                if (rx_buffer.size() < 1024) {
                    rx_buffer.push_back(byte);
                } else {
                    rx_buffer.clear(); // Overflow, discard corrupted frame
                }
            }
        }
    }
};

#else
// Desktop mock implementation for unit test suites
class HardwareLora : public LoraInterface {
public:
    HardwareLora() {}
    bool start() override { return true; }
    bool transmit(const uint8_t* data, size_t length) override { return true; }
    void update() override {}
    // Trigger mock RX helper for tests
    void mock_receive(const uint8_t* data, size_t length) {
        trigger_rx(data, length);
    }
};
#endif

#endif // HARDWARE_LORA_H
