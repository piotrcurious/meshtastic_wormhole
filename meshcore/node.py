import json
import logging
import asyncio
from typing import List
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from meshcore.region import Region
from meshcore.packet import MeshCorePacket

logger = logging.getLogger("meshcore.node")

class GPSEnabledLightNode:
    """
    A region-aware, GPS-enabled WiFi light node that participates in the `#QR` channel.
    Leverages spatial `Region` groupings to limit message propagation and execution scope.
    """
    def __init__(self, node_id: int, lat: float, lon: float, candidate_regions: List[Region], udp_port: int = 4500, multicast_group: str = "239.10.10.10"):
        self.node_id = node_id
        self.node_id_hex = f"{node_id:08x}"
        self.lat = lat
        self.lon = lon
        self.light_state = False  # False for off, True for on
        self.is_running = False

        # Resolve active regions this node resides in
        self.regions = [r for r in candidate_regions if r.contains(self.lat, self.lon)]
        self.primary_region = self.regions[0].name if self.regions else "unknown"

        # Set up dual MockLoRa interfaces on the same network ID (self.node_id)
        self.app_lora = MockLoRaInterface(node_id=self.node_id, network_id=self.node_id)
        self.router_lora = MockLoRaInterface(node_id=self.node_id + 100000, network_id=self.node_id)

        # Configure UDP WiFi mesh transport
        self.wifi = UDPTransport(
            mode="multicast",
            port=udp_port,
            multicast_group=multicast_group
        )

        # Build custom Config for PacketRouter
        self.config = Config()
        self.config.data["id"] = self.node_id_hex
        self.config.data["name"] = f"meshcore-node-{self.node_id_hex}"
        self.config.data["wifi"]["udp_port"] = udp_port
        self.config.data["wifi"]["multicast_group"] = multicast_group

        # Initialize router
        self.router = PacketRouter(config=self.config, lora=self.router_lora, wifi=self.wifi)

        # Register callback on app_lora to process incoming packets
        self.app_lora.register_rx_callback(self.handle_incoming_lora)

        self._advertise_task = None

    async def start(self):
        self.is_running = True
        self.router.start()
        await self.app_lora.start()
        await self.router_lora.start()
        await self.wifi.start()

        # Start periodic GPS position advertising
        self._advertise_task = asyncio.create_task(self._advertise_loop())
        logger.info(f"MeshCore Node {self.node_id_hex} started at GPS ({self.lat}, {self.lon}) in region: {self.primary_region}")

    async def stop(self):
        self.is_running = False
        if self._advertise_task:
            self._advertise_task.cancel()
            try:
                await self._advertise_task
            except asyncio.CancelledError:
                pass
        await self.wifi.stop()
        await self.app_lora.stop()
        await self.router_lora.stop()
        logger.info(f"MeshCore Node {self.node_id_hex} stopped")

    def handle_incoming_lora(self, payload: bytes):
        """
        Processes incoming application layer messages.
        Filters based on geographical/regional rules.
        """
        try:
            packet = MeshCorePacket.from_json(payload.decode("utf-8"))
        except Exception:
            return

        if packet.channel != "#QR":
            return

        # Perform Spatial Scope/Region Filtering!
        if not packet.is_valid_for_node(self.lat, self.lon, self.regions):
            logger.debug(f"MeshCore Node {self.node_id_hex} dropped packet (out of region scope: {packet.region})")
            return

        msg_type = packet.type
        if msg_type == "command":
            target = packet.data.get("target")
            if target == self.node_id_hex:
                new_state = packet.data.get("light") == "on"
                if self.light_state != new_state:
                    self.light_state = new_state
                    logger.info(f"MeshCore Node {self.node_id_hex} light changed to: {'ON' if self.light_state else 'OFF'}")

    async def _advertise_loop(self):
        await asyncio.sleep(0.1)
        while self.is_running:
            try:
                await self.advertise_position()
            except Exception as e:
                logger.error(f"Error in advertise loop for {self.node_id_hex}: {e}")
            await asyncio.sleep(5.0)

    async def advertise_position(self):
        """
        Sends an advertise packet with regional coordinates on the `#QR` channel.
        """
        packet = MeshCorePacket(
            channel="#QR",
            sender=self.node_id_hex,
            scope="limited",
            type="advertise",
            data={
                "lat": self.lat,
                "lon": self.lon,
                "region": self.primary_region
            },
            region=self.primary_region
        )
        payload_bytes = packet.to_json().encode("utf-8")
        await self.app_lora.transmit(payload_bytes)
        logger.debug(f"MeshCore Node {self.node_id_hex} advertised position ({self.lat}, {self.lon}) in {self.primary_region}")
