# ESP32-C3 Firmware Port - Meshtastic Wormhole Bridge

This directory contains the production-ready C++ firmware port of the **Meshtastic Wormhole Bridge (MWB)** optimized for ESP32-C3 / ESP32-S3 typical LoRa node hardware boards.

## Project Structure

- `src/main.cpp`: Entrypoint, initializes hardware interfaces, sets up routing, and schedules updates.
- `src/packet.h`: Custom 'WHOL' binary packet parser matching exact spec byte offsets, support for hops counts and lists.
- `src/deduplicator.h`: TTL-based map packet cache for loop deduplication.
- `src/lora_interface.h`: Abstract LoRa antenna base definition.
- `src/hardware_lora.h`: Concrete physical serial driver supporting standard SLIP (RFC 1055) encoding/decoding.
- `src/wifi_mesh_interface.h`: Abstract WiFi Mesh layer base definition.
- `src/painless_mesh_impl.h`: AP-mode softAP fallback + IP Multicast UDP subscriber mesh network transport.
- `src/esp_mesh_wifi.h`: Native 802.11s ESP-MESH hardware API wrapper for routerless decentralized mesh networks.

## Features

1. **Dual WiFi Backends**: Attempts to initialize high-speed native 802.11s ESP-MESH hardware routing. If hardware or environment fails, gracefully falls back to softAP mode with IP Multicast UDP packets on `239.10.10.10:4403`.
2. **Standard SLIP Framing**: Safely frames incoming and outgoing packets over Serial to LoRa transceivers (e.g. SX1262 modules).
3. **Loop & Duplicate Prevention**: Unpacks the wormhole header, registers visited node hops list, and discards loops and duplicate packets within a 120s TTL window.

## Compile & Deploy

This port supports compiling with PlatformIO or Espressif ESP-IDF. To build:

```bash
# Using PlatformIO core CLI
pio run -e esp32-c3
```
