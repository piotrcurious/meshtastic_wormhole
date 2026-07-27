from mwb.config import Config
from mwb.packet import WormholePacket, MAGIC_BYTES
import struct

def test_config_generation():
    cfg = Config()
    assert cfg.name.startswith("wormhole-") or cfg.name == "default-wormhole"
    assert len(cfg.id) == 12
    assert isinstance(cfg.int_id, int)
    assert cfg.wifi_mode == "multicast"
    assert cfg.udp_port == 4403

def test_packet_pack_unpack_no_hops():
    payload = b"Hello Meshtastic!"
    pkt = WormholePacket(
        source_id=0x1122334455667788,
        packet_id=1024,
        payload=payload,
        hops=[]
    )

    packed = pkt.pack()
    assert len(packed) == 26 + len(payload)
    assert packed.startswith(MAGIC_BYTES)

    unpacked = WormholePacket.unpack(packed)
    assert unpacked.source_id == 0x1122334455667788
    assert unpacked.packet_id == 1024
    assert unpacked.payload == payload
    assert len(unpacked.hops) == 0

def test_packet_pack_unpack_with_hops():
    payload = b"Hello Meshtastic!"
    pkt = WormholePacket(
        source_id=0x1122334455667788,
        packet_id=1024,
        payload=payload,
        hops=[0x9999, 0x8888]
    )

    packed = pkt.pack()
    # 26 (base) + 2 (hops count) + 2 * 8 (hops list) + payload
    assert len(packed) == 26 + 2 + 16 + len(payload)
    assert packed.startswith(MAGIC_BYTES)

    unpacked = WormholePacket.unpack(packed)
    assert unpacked.source_id == 0x1122334455667788
    assert unpacked.packet_id == 1024
    assert unpacked.payload == payload
    assert unpacked.hops == [0x9999, 0x8888]
    assert unpacked.has_visited(0x9999) is True
    assert unpacked.has_visited(0x1122334455667788) is True
    assert unpacked.has_visited(0x1234) is False

def test_packet_offset_verification():
    """Verify that specific fields exist at the exact offsets from design spec."""
    payload = b"test"
    pkt = WormholePacket(
        source_id=0x1122334455667788,
        packet_id=1024,
        payload=payload,
        timestamp=12345678,
        version=1,
        flags=0
    )
    packed = pkt.pack()

    magic = packed[0:4]
    version = packed[4]
    flags = packed[5]
    header_size, = struct.unpack("<H", packed[6:8])
    source_id, = struct.unpack("<Q", packed[8:16])
    packet_id, = struct.unpack("<I", packed[16:20])
    timestamp, = struct.unpack("<I", packed[20:24])
    payload_len, = struct.unpack("<H", packed[24:26])

    assert magic == MAGIC_BYTES
    assert version == 1
    assert flags == 0
    assert header_size == 26
    assert source_id == 0x1122334455667788
    assert packet_id == 1024
    assert timestamp == 12345678
    assert payload_len == len(payload)

def test_packet_unsigned_overflow():
    """Verify that large 64-bit source IDs and 32-bit packet IDs do not crash the packing."""
    payload = b"overflow_test"
    pkt = WormholePacket(
        source_id=0xFFFFFFFFFFFFFFFF, # Large 64-bit unsigned ID
        packet_id=0xFFFFFFFF,         # Large 32-bit unsigned packet ID
        payload=payload,
        hops=[0xEEEEEEEEEEEEEEEE]
    )
    packed = pkt.pack()
    unpacked = WormholePacket.unpack(packed)

    assert unpacked.source_id == 0xFFFFFFFFFFFFFFFF
    assert unpacked.packet_id == 0xFFFFFFFF
    assert unpacked.hops == [0xEEEEEEEEEEEEEEEE]
