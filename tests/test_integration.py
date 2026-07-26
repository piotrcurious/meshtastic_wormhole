import asyncio
import pytest
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from mwb.packet import WormholePacket

@pytest.mark.asyncio
async def test_full_bridge_loopback_and_loop_prevention():
    """
    Simulates three nodes connected via UDP Multicast and Mock LoRa:
    Node A, Node B, and Node C.

    This verifies that a packet traversing the network can reach all nodes
    without causing infinite packet loops (loop prevention).
    """
    MockLoRaInterface.clear_instances()

    # 1. Configuration for three nodes
    cfg_a = Config()
    cfg_a.data["id"] = "00000000000a"
    cfg_a.data["wifi"]["udp_port"] = 4700
    cfg_a.data["diagnostics"]["port"] = 9101

    cfg_b = Config()
    cfg_b.data["id"] = "00000000000b"
    cfg_b.data["wifi"]["udp_port"] = 4700
    cfg_b.data["diagnostics"]["port"] = 9102

    cfg_c = Config()
    cfg_c.data["id"] = "00000000000c"
    cfg_c.data["wifi"]["udp_port"] = 4700
    cfg_c.data["diagnostics"]["port"] = 9103

    # To prevent LoRa loopback locally during mock routing tests,
    # let's put each wormhole node on its own distinct/isolated simulated LoRa network.
    # This represents isolated local LoRa network segments bridged via WiFi.
    lora_a = MockLoRaInterface(node_id=cfg_a.int_id, network_id=1)
    wifi_a = UDPTransport(mode="multicast", port=cfg_a.udp_port)

    lora_b = MockLoRaInterface(node_id=cfg_b.int_id, network_id=2)
    wifi_b = UDPTransport(mode="multicast", port=cfg_b.udp_port)

    lora_c = MockLoRaInterface(node_id=cfg_c.int_id, network_id=3)
    wifi_c = UDPTransport(mode="multicast", port=cfg_c.udp_port)

    # 3. Create Routers
    router_a = PacketRouter(config=cfg_a, lora=lora_a, wifi=wifi_a)
    router_b = PacketRouter(config=cfg_b, lora=lora_b, wifi=wifi_b)
    router_c = PacketRouter(config=cfg_c, lora=lora_c, wifi=wifi_c)

    router_a.start()
    router_b.start()
    router_c.start()

    # 4. Start all
    await lora_a.start()
    await wifi_a.start()

    await lora_b.start()
    await wifi_b.start()

    await lora_c.start()
    await wifi_c.start()

    await asyncio.sleep(0.2)

    # 5. Let's send a packet on LoRa A (e.g. from an end-device connected to A)
    test_meshtastic_packet = b"\x00\x01\x02PayloadData"

    # Simulate receiving the packet from physical/local LoRa antenna on Node A
    lora_a.rx_callback(test_meshtastic_packet)

    # Allow asyncio to process all the routing/transfers
    await asyncio.sleep(0.5)

    # Check status and verify packet transmission counts
    # A received on LoRa, forwarded to WiFi
    assert router_a.stats["lora_rx"] == 1
    assert router_a.stats["wifi_tx"] >= 1

    # B and C should have received via WiFi, retransmitted on LoRa
    # B and C also forward back to WiFi with updated hops, but since it's multicast they also see each other's updates.
    # Because of loop prevention, they should drop packets they already saw or already contain their ID in hops list.
    assert router_b.stats["wifi_rx"] >= 1
    assert router_c.stats["wifi_rx"] >= 1

    # Ensure loop prevention or duplicates count is > 0 on nodes (due to multicast echo and cross-talk)
    total_prevented = router_a.stats["loop_prevented"] + router_b.stats["loop_prevented"] + router_c.stats["loop_prevented"]
    total_duplicates = router_a.stats["duplicates"] + router_b.stats["duplicates"] + router_c.stats["duplicates"]

    assert (total_prevented + total_duplicates) > 0

    # Stop all
    await lora_a.stop()
    await wifi_a.stop()
    await lora_b.stop()
    await wifi_b.stop()
    await lora_c.stop()
    await wifi_c.stop()
