#ifndef HARDWARE_LORA_H
#define HARDWARE_LORA_H

#include "lora_interface.h"

// Hardware driver assuming SPI or standard Serial module for ESP32-C3
// For standard ESP32-C3 + LoRa layouts (e.g. Heltec or custom board),
// we listen to incoming messages over standard HardwareSerial SLIP protocol or SPI.
// We implement a dual Serial-based or stub SPI SX1262 LoRa interface here.
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
                if (byte == SLIP_ESC_END) {
                    rx_buffer.push_back(SLIP_END);
                } else if (byte == SLIP_ESC_ESC) {
                    rx_buffer.push_back(SLIP_ESC);
                } else {
                    rx_buffer.push_back(byte);
                }
                in_escape = false;
            } else {
                rx_buffer.push_back(byte);
            }
        }
    }
};

#endif // HARDWARE_LORA_H
