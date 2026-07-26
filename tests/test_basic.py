from mwb.config import Config
from mwb.packet import WormholePacket, MAGIC_BYTES

def test_config_generation():
    cfg = Config()
    assert cfg.name.startswith("wormhole-") or cfg.name == "default-wormhole"
    assert len(cfg.id) == 12
    assert isinstance(cfg.int_id, int)
    assert cfg.wifi_mode == "multicast"
    assert cfg.udp_port == 4403

def test_packet_pack_unpack():
    payload = b"Hello Meshtastic!"
    pkt = WormholePacket(
        source_id=0x1122334455667788,
        packet_id=1024,
        payload=payload,
        hops=[0x9999, 0x8888]
    )

    packed = pkt.pack()
    assert packed.startswith(MAGIC_BYTES)

    unpacked = WormholePacket.unpack(packed)
    assert unpacked.source_id == 0x1122334455667788
    assert unpacked.packet_id == 1024
    assert unpacked.payload == payload
    assert unpacked.hops == [0x9999, 0x8888]
    assert unpacked.has_visited(0x9999) is True
    assert unpacked.has_visited(0x1122334455667788) is True
    assert unpacked.has_visited(0x1234) is False
