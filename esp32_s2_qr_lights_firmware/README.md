# ESP32-S2 WiFi Mesh GPS LED Lights Node Firmware

This firmware is a dedicated port of the GPS-enabled LED lights node designed for the `#QR` channel interactive visual grids demonstration.

## Features

- **Mesh Repeat/Extend:** Continues to act as a transparent repeater bridge over standard UDP/multicast or 802.11s native hardware mesh.
- **GPS UART Parsing:** Integrates a zero-dependency, ultra-lightweight NMEA parser for cheap GPS modules (e.g. Neo-6M) on hardware `UART1` (pins RX=17, TX=16 by default).
- **LED Display:** Controls physical LED on `GPIO2` (built-in LED) or any external LED matching the targeting command payloads on `#QR` channel.
- **Periodic Advertisements:** Periodically generates and broadcasts coordinates to register onto the topological grid managed by the coordinator node.
