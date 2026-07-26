import logging
import random
import time
from typing import Dict, Any
from mwb.config import Config
from mwb.packet import WormholePacket
from mwb.duplicate import Deduplicator
from mwb.lora.base import LoRaInterface
from mwb.wifi.udp_transport import UDPTransport

logger = logging.getLogger("mwb.router")

class PacketRouter:
    """
    Coordinates flow of packets:
    - LoRa RX -> Encapsulate into WormholePacket -> UDP TX
    - UDP RX -> Decapsulate -> Deduplicate & Loop filter -> LoRa TX
    """
    def __init__(self, config: Config, lora: LoRaInterface, wifi: UDPTransport):
        self.config = config
        self.lora = lora
        self.wifi = wifi

        self.dedup = Deduplicator(ttl=120.0)
        self.node_id = config.int_id

        # Diagnostics stats
        self.stats = {
            "lora_rx": 0,
            "lora_tx": 0,
            "wifi_rx": 0,
            "wifi_tx": 0,
            "duplicates": 0,
            "loop_prevented": 0,
            "errors": 0,
            "start_time": time.time()
        }

        # Monotonically increasing sequence/packet ID for outgoing packets originating here
        self._next_packet_id = random.randint(1, 100000)

    def start(self):
        self.lora.register_rx_callback(self.handle_lora_packet)
        self.wifi.register_rx_callback(self.handle_wifi_packet)
        logger.info(f"Packet Router initialized for node: {self.config.name} (ID: {self.config.id})")

    def handle_lora_packet(self, payload: bytes):
        """
        Triggered when a raw Meshtastic packet is received from LoRa.
        We encapsulate it into a WormholePacket and transmit it over UDP (WiFi).
        """
        self.stats["lora_rx"] += 1
        logger.info(f"LoRa RX: received raw frame of size {len(payload)}")

        # For packets originating from the local LoRa, we check for deduplication just in case
        packet_id = self._next_packet_id
        self._next_packet_id = (self._next_packet_id + 1) & 0xFFFFFFFF

        if self.dedup.is_duplicate(self.node_id, packet_id):
            self.stats["duplicates"] += 1
            logger.debug(f"LoRa RX: dropped duplicate local packet {packet_id}")
            return

        # Create the Wormhole encapsulation
        pkt = WormholePacket(
            source_id=self.node_id,
            packet_id=packet_id,
            payload=payload,
            hops=[] # start with empty hops
        )

        # Add ourselves as a visited hop
        pkt.add_hop(self.node_id)

        # Serialize and send
        packed_data = pkt.pack()
        self.wifi.transmit(packed_data)
        self.stats["wifi_tx"] += 1
        logger.info(f"LoRa -> WiFi: Encapsulated & forwarded packet {packet_id} to WiFi")

    def handle_wifi_packet(self, data: bytes, addr: tuple):
        """
        Triggered when a packet is received via UDP (WiFi).
        Decapsulates, performs loop prevention, deduplication, and schedules LoRa transmission.
        """
        self.stats["wifi_rx"] += 1
        try:
            pkt = WormholePacket.unpack(data)
        except Exception as e:
            self.stats["errors"] += 1
            logger.warning(f"WiFi RX: failed to unpack packet from {addr}: {e}")
            return

        logger.info(f"WiFi RX: received {pkt} from {addr}")

        # 1. Loop Prevention: check if we have already visited this packet
        if pkt.has_visited(self.node_id):
            self.stats["loop_prevented"] += 1
            logger.info(f"Loop Prevention: dropped packet {pkt.packet_id} from {pkt.source_id:x} - already visited this node")
            return

        # 2. Duplicate Prevention: check if we have already processed this exact packet
        if self.dedup.is_duplicate(pkt.source_id, pkt.packet_id):
            self.stats["duplicates"] += 1
            logger.info(f"Deduplication: dropped duplicate packet {pkt.packet_id} from {pkt.source_id:x}")
            return

        # 3. Add ourselves to the visited list (hop list)
        pkt.add_hop(self.node_id)

        # 4. Forward onto local LoRa network
        # Since we are transmitting over LoRa, we run it as an async task in the background
        import asyncio
        loop = asyncio.get_event_loop()
        loop.create_task(self._transmit_lora_and_update(pkt))

    async def _transmit_lora_and_update(self, pkt: WormholePacket):
        success = await self.lora.transmit(pkt.payload)
        if success:
            self.stats["lora_tx"] += 1
            logger.info(f"WiFi -> LoRa: Successfully retransmitted payload of packet {pkt.packet_id} on LoRa")

            # Since we retransmitted and updated the hop list, we also forward the packet
            # back to WiFi so downstream WiFi nodes know we have visited this node.
            packed_data = pkt.pack()
            self.wifi.transmit(packed_data)
            self.stats["wifi_tx"] += 1
        else:
            self.stats["errors"] += 1
            logger.error(f"WiFi -> LoRa: Failed to transmit payload of packet {pkt.packet_id} on LoRa")

    def get_status(self) -> Dict[str, Any]:
        uptime = time.time() - self.stats["start_time"]
        return {
            "name": self.config.name,
            "id": self.config.id,
            "uptime_seconds": int(uptime),
            "stats": self.stats,
            "config": {
                "wifi_mode": self.config.wifi_mode,
                "udp_port": self.config.udp_port,
                "lora_mode": self.config.lora_mode,
                "lora_region": self.config.lora_region
            }
        }
