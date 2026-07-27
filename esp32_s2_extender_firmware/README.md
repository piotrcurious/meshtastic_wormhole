# ESP32-S2 WiFi Mesh Extender Firmware

This directory contains the production-ready C++ firmware port of the **Meshtastic Wormhole WiFi Mesh Extender** optimized for ESP32-S2 typical node hardware boards.

## Project Structure

- `src/main.cpp`: Extender entrypoint, handles multi-mesh forwarding, activity watchdog, and low-power idle states.
- `src/packet.h`: Custom 'WHOL' binary packet parser matching exact spec byte offsets, support for hops counts and lists.
- `src/deduplicator.h`: TTL-based map packet cache for loop deduplication.
- `src/wifi_mesh_interface.h`: Abstract WiFi Mesh layer base definition.
- `src/painless_mesh_impl.h`: AP-mode softAP fallback + IP Multicast UDP subscriber mesh network transport.
- `src/esp_mesh_wifi.h`: Native 802.11s ESP-MESH hardware API wrapper for routerless decentralized mesh networks.

## Features

1. **Range Extension / Repeating**:
   - Explicitly designed for range extension in-between WiFi-LoRa endpoints.
   - Automatically unpacks incoming `WormholePacket` packets, registers visited node hops list, and repeats them to extend cover.
2. **Dual WiFi Backends**: Attempts to initialize native 802.11s ESP-MESH hardware routing. If hardware fails, gracefully falls back to softAP mode with IP Multicast UDP packets on `239.10.10.10:4403`.
3. **Advanced ESP32-S2 Power Saving**:
   - Dynamic Frequency Scaling (DFS): Automatically throttles CPU core frequency down to 10MHz when idle, returning to 240MHz instantly on packet arrival.
   - WiFi Modem Sleep: Configured `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` to minimize idle power.
   - Configurable at compile time via `ENABLE_POWER_SAVING` define.

## Compile & Deploy

```bash
# Using PlatformIO core CLI
pio run -e esp32-s2
```
