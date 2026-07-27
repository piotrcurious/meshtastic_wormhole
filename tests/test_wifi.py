import asyncio
import pytest
from mwb.wifi.udp_transport import UDPTransport

@pytest.mark.asyncio
async def test_udp_multicast():
    # Setup two UDP Transports on the same machine/ports
    transport1 = UDPTransport(mode="multicast", port=4500, multicast_group="239.10.10.10")
    transport2 = UDPTransport(mode="multicast", port=4500, multicast_group="239.10.10.10")

    received_packets = []
    transport2.register_rx_callback(lambda data, addr: received_packets.append((data, addr)))

    await transport1.start()
    await transport2.start()

    test_payload = b"Tunnel packet data!"
    transport1.transmit(test_payload)

    # Allow asyncio loop to process the packet
    await asyncio.sleep(0.2)

    assert len(received_packets) >= 1
    # Check that at least one packet matches the test_payload
    payloads = [p[0] for p in received_packets]
    assert test_payload in payloads

    await transport1.stop()
    await transport2.stop()


@pytest.mark.asyncio
async def test_udp_unicast():
    transport1 = UDPTransport(mode="unicast", port=4501, peers=["127.0.0.1:4502"])
    transport2 = UDPTransport(mode="unicast", port=4502, peers=["127.0.0.1:4501"])

    received_packets = []
    transport2.register_rx_callback(lambda data, addr: received_packets.append((data, addr)))

    await transport1.start()
    await transport2.start()

    test_payload = b"Unicast test data!"
    transport1.transmit(test_payload)

    await asyncio.sleep(0.1)

    assert len(received_packets) == 1
    assert received_packets[0][0] == test_payload

    await transport1.stop()
    await transport2.stop()
