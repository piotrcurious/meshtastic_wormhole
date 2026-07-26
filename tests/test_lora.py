import asyncio
import pytest
from mwb.lora.mock_interface import MockLoRaInterface

@pytest.mark.asyncio
async def test_mock_lora_loopback():
    MockLoRaInterface.clear_instances()
    node1 = MockLoRaInterface(node_id=0x01, network_id=1)
    node2 = MockLoRaInterface(node_id=0x02, network_id=1)
    node3 = MockLoRaInterface(node_id=0x03, network_id=2) # different network, shouldn't receive node1 packets

    received_payloads_2 = []
    received_payloads_3 = []

    node2.register_rx_callback(lambda p: received_payloads_2.append(p))
    node3.register_rx_callback(lambda p: received_payloads_3.append(p))

    await node1.start()
    await node2.start()
    await node3.start()

    test_message = b"Meshtastic Test Packet"
    success = await node1.transmit(test_message)
    assert success is True

    # Allow asyncio to schedule the delivery callback
    await asyncio.sleep(0.1)

    assert len(received_payloads_2) == 1
    assert received_payloads_2[0] == test_message

    assert len(received_payloads_3) == 0

    await node1.stop()
    await node2.stop()
    await node3.stop()
