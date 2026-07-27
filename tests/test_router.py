import asyncio
import pytest
import urllib.request
import json
import struct
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from mwb.diagnostics import DiagnosticsServer

@pytest.mark.asyncio
async def test_router_and_diagnostics():
    # 1. Setup config
    cfg = Config()
    cfg.data["diagnostics"]["port"] = 9091
    cfg.data["wifi"]["udp_port"] = 4600

    # 2. Setup interfaces
    lora = MockLoRaInterface(node_id=cfg.int_id)
    wifi = UDPTransport(mode="multicast", port=cfg.udp_port)

    # 3. Setup router
    router = PacketRouter(config=cfg, lora=lora, wifi=wifi)
    router.start()

    # 4. Setup diagnostics server
    diag = DiagnosticsServer(router=router)

    await lora.start()
    await wifi.start()
    await diag.start()

    # Allow servers to bind and start
    await asyncio.sleep(0.2)

    # 5. Fetch and verify diagnostics response
    def fetch_status():
        with urllib.request.urlopen("http://127.0.0.1:9091/status") as res:
            return json.loads(res.read().decode())

    loop = asyncio.get_running_loop()
    status = await loop.run_in_executor(None, fetch_status)

    assert status["name"] == cfg.name
    assert status["id"] == cfg.id
    assert status["stats"]["lora_rx"] == 0

    # 6. Stop all
    await diag.stop()
    await wifi.stop()
    await lora.stop()

@pytest.mark.asyncio
async def test_multi_lora_and_targeted_routing():
    cfg = Config()
    cfg.data["id"] = "00000000000a"

    # Setup two mock LoRa interfaces representing a multi-radio setup
    lora1 = MockLoRaInterface(node_id=cfg.int_id, network_id=1)
    lora2 = MockLoRaInterface(node_id=cfg.int_id, network_id=2)
    wifi = UDPTransport(mode="multicast", port=4900)

    router = PacketRouter(config=cfg, lora_interfaces=[lora1, lora2], wifi=wifi)
    router.start()

    await lora1.start()
    await lora2.start()
    await wifi.start()

    # Create a simulated raw Meshtastic packet header (12 bytes)
    # dest = 0x88888888, src = 0x99999999, id = 42
    meshtastic_packet = struct.pack("<III", 0x88888888, 0x99999999, 42) + b"hello"

    # Simulate packet arrival from lora1
    lora1.rx_callback(meshtastic_packet)
    await asyncio.sleep(0.1)

    # Verify that from_node has been mapped in the routing directory
    assert 0x99999999 in router.routing_table
    assert router.routing_table[0x99999999] == router.node_id

    # Test targeted bypass (simulating incoming Wi-Fi packet from a peer targeting someone else)
    # Construct a packet marked as targeted (flags bit 1 / 0x02) targeting node 0xBBBBBBBBBBBBBBBB
    from mwb.packet import WormholePacket
    peer_pkt = WormholePacket(
        source_id=0xCCCCCCCCCCCCCCCC,
        packet_id=123,
        payload=meshtastic_packet,
        flags=0x02,
        hops=[0xCCCCCCCCCCCCCCCC, 0xBBBBBBBBBBBBBBBB] # targeted to 0xBBBBBBBBBBBBBBBB (not us!)
    )

    # Inject packet to UDP handler
    router.handle_wifi_packet(peer_pkt.pack(), ("127.0.0.1", 4900))
    await asyncio.sleep(0.1)

    # Should have been bypassed since it's targeted for another wormhole node
    assert router.stats["targeted_bypass"] == 1

    await lora1.stop()
    await lora2.stop()
    await wifi.stop()
