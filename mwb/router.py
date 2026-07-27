import logging
import random
import time
import struct
from typing import Dict, Any, List, Union, Optional
from mwb.config import Config
from mwb.packet import WormholePacket
from mwb.duplicate import Deduplicator
from mwb.lora.base import LoRaInterface
from mwb.wifi.udp_transport import UDPTransport

logger = logging.getLogger("mwb.router")

class PacketRouter:
    """
    Coordinates flow of packets:
    - Supports multiple LoRa endpoints (e.g. multi-radio gateways)
    - LoRa RX -> Encapsulate into WormholePacket -> UDP TX
    - UDP RX -> Decapsulate -> Deduplicate & Loop filter -> LoRa TX (on all endpoints)

    Implements a smart Meshtastic routing strategy:
    - Parses outer Meshtastic headers to discover node mappings.
    - Dynamically maps Meshtastic Node IDs to Wormhole Peer IDs.
    - Uses targeted routing to reduce mesh congestion by bypassing floods when the path is known.
    """
    def __init__(self, config: Config, lora: Union[LoRaInterface, List[LoRaInterface]] = None, wifi: UDPTransport = None, lora_interfaces: Union[LoRaInterface, List[LoRaInterface]] = None):
        self.config = config
        self.wifi = wifi

        # Determine the correct LoRa interface(s) parameter (backward compatibility with 'lora')
        selected_lora = lora_interfaces if lora_interfaces is not None else lora
        if selected_lora is None:
            raise ValueError("Must provide at least one LoRa interface.")

        if isinstance(selected_lora, list):
            self.lora_list = selected_lora
        else:
            self.lora_list = [selected_lora]

        self.dedup = Deduplicator(ttl=120.0)
        self.node_id = config.int_id

        # Smart Routing Directory: maps Meshtastic Node ID (uint32) -> Wormhole Peer ID (uint64)
        self.routing_table: Dict[int, int] = {}
        # Keep track of when mappings were last updated (TTL/cleanup support)
        self.routing_table_ts: Dict[int, float] = {}
        self.routing_ttl = 300.0 # Mappings expire after 5 minutes of inactivity

        # Diagnostics stats
        self.stats = {
            "lora_rx": 0,
            "lora_tx": 0,
            "wifi_rx": 0,
            "wifi_tx": 0,
            "duplicates": 0,
            "loop_prevented": 0,
            "targeted_bypass": 0,
            "errors": 0,
            "start_time": time.time()
        }

        # Monotonically increasing sequence/packet ID for outgoing packets originating here
        self._next_packet_id = random.randint(1, 100000)

    def start(self):
        for lora in self.lora_list:
            lora.register_rx_callback(self.handle_lora_packet)
        self.wifi.register_rx_callback(self.handle_wifi_packet)
        logger.info(f"Packet Router initialized with {len(self.lora_list)} LoRa endpoint(s) for node: {self.config.name} (ID: {self.config.id})")

    def _parse_meshtastic_header(self, payload: bytes) -> Optional[tuple]:
        """
        Parses unencrypted outer Meshtastic packet header.
        Structure:
        - to: uint32 (4 bytes)
        - from: uint32 (4 bytes)
        - id: uint32 (4 bytes)
        - flags: uint8 (1 byte)
        """
        if len(payload) < 12:
            return None
        try:
            # Unpack first 12 bytes of raw meshtastic packet header: to, from, packet_id
            to_node, from_node, _ = struct.unpack("<III", payload[:12])
            return (to_node, from_node)
        except Exception:
            return None

    def handle_lora_packet(self, payload: bytes):
        """
        Triggered when a raw Meshtastic packet is received from one of our LoRa endpoints.
        """
        self.stats["lora_rx"] += 1
        logger.info(f"LoRa RX: received raw frame of size {len(payload)}")

        # Clean up stale routing table entries
        self._cleanup_routing_table()

        # Parse Meshtastic packet to discover nodes
        header = self._parse_meshtastic_header(payload)
        target_wormhole_id = 0

        if header:
            to_node, from_node = header
            # Dynamic Discovery: we now know that 'from_node' is directly reachable from this wormhole node
            self.routing_table[from_node] = self.node_id
            self.routing_table_ts[from_node] = time.time()
            logger.info(f"Smart Routing: Mapped Meshtastic Node {from_node:x} to Local Wormhole Node {self.node_id:x}")

            # Smart Routing: Check if we know where the 'to_node' is located
            if to_node in self.routing_table:
                target_wormhole_id = self.routing_table[to_node]
                logger.info(f"Smart Routing: Direct path known! Routing packet directly to Wormhole Node {target_wormhole_id:x}")

        packet_id = self._next_packet_id
        self._next_packet_id = (self._next_packet_id + 1) & 0xFFFFFFFF

        if self.dedup.is_duplicate(self.node_id, packet_id):
            self.stats["duplicates"] += 1
            logger.debug(f"LoRa RX: dropped duplicate local packet {packet_id}")
            return

        # Define: flags bit 1 (value 0x02) = Targeted Routing.
        # If targeted, hops[0] is source, hops[1] is target.
        flags = 0
        hops = [self.node_id]
        if target_wormhole_id != 0:
            flags |= 0x02 # mark as targeted
            hops.append(target_wormhole_id)

        # Create the Wormhole encapsulation
        pkt = WormholePacket(
            source_id=self.node_id,
            packet_id=packet_id,
            payload=payload,
            timestamp=int(time.time()),
            flags=flags,
            hops=hops
        )

        # Serialize and send
        packed_data = pkt.pack()
        self.wifi.transmit(packed_data)
        self.stats["wifi_tx"] += 1
        logger.info(f"LoRa -> WiFi: Encapsulated & forwarded packet {packet_id} to WiFi")

    def handle_wifi_packet(self, data: bytes, addr: tuple):
        """
        Triggered when a packet is received via UDP (WiFi).
        """
        self.stats["wifi_rx"] += 1
        try:
            pkt = WormholePacket.unpack(data)
        except Exception as e:
            self.stats["errors"] += 1
            logger.warning(f"WiFi RX: failed to unpack packet from {addr}: {e}")
            return

        logger.info(f"WiFi RX: received {pkt} from {addr}")

        # Smart Routing: Parse Meshtastic packet header from payload to register sender mapping
        header = self._parse_meshtastic_header(pkt.payload)
        if header:
            _, from_node = header
            # Dynamic Discovery: from_node is behind the packet's source_id wormhole
            self.routing_table[from_node] = pkt.source_id
            self.routing_table_ts[from_node] = time.time()
            logger.info(f"Smart Routing: Mapped Meshtastic Node {from_node:x} to Wormhole Node {pkt.source_id:x}")

        # 1. Loop Prevention: check if we have already visited this packet
        if pkt.has_visited(self.node_id):
            self.stats["loop_prevented"] += 1
            logger.info(f"Loop Prevention: dropped packet {pkt.packet_id} from {pkt.source_id:x} - already visited this node")
            return

        # 2. Targeted Routing check: if targeted, only process if we are the destination or the source
        is_targeted = (pkt.flags & 0x02) != 0
        if is_targeted and len(pkt.hops) >= 2:
            target_id = pkt.hops[1]
            if target_id != self.node_id and pkt.source_id != self.node_id:
                self.stats["targeted_bypass"] += 1
                logger.info(f"Smart Routing: Bypassing packet {pkt.packet_id} - targeted for Wormhole {target_id:x}")
                return

        # 3. Duplicate Prevention
        if self.dedup.is_duplicate(pkt.source_id, pkt.packet_id):
            self.stats["duplicates"] += 1
            logger.info(f"Deduplication: dropped duplicate packet {pkt.packet_id} from {pkt.source_id:x}")
            return

        # 4. Add ourselves to the visited list (hop list)
        pkt.add_hop(self.node_id)

        # 5. Forward onto all registered LoRa endpoints
        import asyncio
        loop = asyncio.get_event_loop()
        loop.create_task(self._transmit_all_lora_and_update(pkt))

    async def _transmit_all_lora_and_update(self, pkt: WormholePacket):
        success_any = False
        for lora in self.lora_list:
            success = await lora.transmit(pkt.payload)
            if success:
                success_any = True
                self.stats["lora_tx"] += 1

        if success_any:
            logger.info(f"WiFi -> LoRa: Successfully retransmitted payload of packet {pkt.packet_id} on LoRa")
            # Forward updated packet back to WiFi mesh so other nodes know we visited
            packed_data = pkt.pack()
            self.wifi.transmit(packed_data)
            self.stats["wifi_tx"] += 1
        else:
            self.stats["errors"] += 1
            logger.error(f"WiFi -> LoRa: Failed to transmit payload of packet {pkt.packet_id} on any LoRa endpoint")

    def _cleanup_routing_table(self):
        now = time.time()
        expired_keys = [
            node_id for node_id, ts in self.routing_table_ts.items()
            if now - ts > self.routing_ttl
        ]
        for key in expired_keys:
            del self.routing_table[key]
            del self.routing_table_ts[key]

    def get_status(self) -> Dict[str, Any]:
        uptime = time.time() - self.stats["start_time"]
        return {
            "name": self.config.name,
            "id": self.config.id,
            "uptime_seconds": int(uptime),
            "stats": self.stats,
            "routing_table_size": len(self.routing_table),
            "config": {
                "wifi_mode": self.config.wifi_mode,
                "udp_port": self.config.udp_port,
                "lora_endpoints_count": len(self.lora_list),
                "lora_mode": self.config.lora_mode,
                "lora_region": self.config.lora_region
            }
        }
