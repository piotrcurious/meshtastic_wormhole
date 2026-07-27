import struct
import time

MAGIC_BYTES = b"WHOL"

class WormholePacket:
    """
    MWB Wormhole Packet implementation.

    Refined to strictly match the design specification:

    OFFSET SIZE DESCRIPTION
    0       4    MAGIC ('WHOL')
    4       1    VERSION (currently 1)
    5       1    FLAGS (bit 0: loop prevention type, etc. Currently 0)
    6       2    HEADER SIZE (size of the header up to payload start)
    8       8    SOURCE ID (64-bit integer, e.g. from MAC or UUID)
    16      4    PACKET ID (32-bit unique sequence/id)
    20      4    TIMESTAMP (32-bit epoch)
    24      2    PAYLOAD LENGTH (16-bit payload length)

    -- Dynamically appended field if Version 2 loop prevention / hop list is enabled:
    26      2    HOPS COUNT (16-bit count of visited node IDs)
    28      8*H  HOPS LIST (list of 64-bit source IDs representing visited/hop nodes)

    X       N    MESHTASTIC FRAME (payload at HEADER SIZE offset)
    """

    def __init__(self, source_id: int, packet_id: int, payload: bytes, timestamp: int = None, version: int = 1, flags: int = 0, hops: list = None):
        self.source_id = source_id
        self.packet_id = packet_id
        self.payload = payload
        self.timestamp = timestamp if timestamp is not None else int(time.time())
        self.version = version
        self.flags = flags
        self.hops = hops if hops is not None else []

    def pack(self) -> bytes:
        hops_count = len(self.hops)
        # Standard base header size (without hop list metadata) is 26 bytes.
        # If hops list is used, we append a 2-byte hops count and 8*H bytes of hop IDs.
        if hops_count > 0:
            header_size = 26 + 2 + (8 * hops_count)
        else:
            header_size = 26

        payload_length = len(self.payload)

        # Base Format: 4s (MAGIC), B (version), B (flags), H (header_size), Q (source_id), I (packet_id), I (timestamp), H (payload_length)
        # Note: Using Q for unsigned 64-bit source_id and I for unsigned 32-bit packet_id to avoid overflow/sign crashes.
        base_header = struct.pack(
            "<4sBBHQIIH",
            MAGIC_BYTES,
            self.version,
            self.flags,
            header_size,
            self.source_id,
            self.packet_id,
            self.timestamp,
            payload_length
        )

        # Pack the hops list metadata if hops_count > 0
        hops_data = b""
        if hops_count > 0:
            hops_data += struct.pack("<H", hops_count)
            for hop_id in self.hops:
                hops_data += struct.pack("<Q", hop_id)

        return base_header + hops_data + self.payload

    @classmethod
    def unpack(cls, data: bytes) -> 'WormholePacket':
        if len(data) < 26:
            raise ValueError("Packet too short to contain minimal wormhole header.")

        magic, version, flags, header_size, source_id, packet_id, timestamp, payload_length = struct.unpack(
            "<4sBBHQIIH",
            data[:26]
        )

        if magic != MAGIC_BYTES:
            raise ValueError(f"Invalid magic bytes. Expected {MAGIC_BYTES}, got {magic}")

        if len(data) < header_size + payload_length:
            raise ValueError(f"Packet data size {len(data)} is less than header_size ({header_size}) + payload_length ({payload_length})")

        # Parse hops if header_size is greater than 26
        hops = []
        if header_size > 26:
            if len(data) < 28:
                raise ValueError("Packet has header_size > 26 but data is too short to contain hops count.")
            hops_count, = struct.unpack("<H", data[26:28])
            hop_start = 28
            for _ in range(hops_count):
                if len(data) < hop_start + 8:
                    raise ValueError("Packet truncated before reading all hops from hops list.")
                hop_id, = struct.unpack("<Q", data[hop_start:hop_start+8])
                hops.append(hop_id)
                hop_start += 8

        payload = data[header_size:header_size + payload_length]

        return cls(
            source_id=source_id,
            packet_id=packet_id,
            payload=payload,
            timestamp=timestamp,
            version=version,
            flags=flags,
            hops=hops
        )

    def add_hop(self, node_id: int):
        """Append node_id to the hops list if not already present."""
        if node_id not in self.hops:
            self.hops.append(node_id)

    def has_visited(self, node_id: int) -> bool:
        """Check if node_id is in the hops list or is the source_id."""
        return node_id == self.source_id or node_id in self.hops

    def __repr__(self):
        return f"<WormholePacket src={self.source_id:x} id={self.packet_id} hops={len(self.hops)} payload_len={len(self.payload)}>"
