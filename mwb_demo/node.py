import json
import logging
import asyncio
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter

logger = logging.getLogger("mwb_demo.node")

class GPSEnabledLightNode:
    """
    A GPS-enabled WiFi light node that participates in the `#QR` channel.
    It runs an MWB stack (MockLoRa + UDPTransport + PacketRouter) to communicate.
    Uses dual-LoRa interfaces: one for the app, one for the router, on the same network ID.
    """
    def __init__(self, node_id: int, lat: float, lon: float, udp_port: int = 4500, multicast_group: str = "239.10.10.10"):
        self.node_id = node_id
        self.node_id_hex = f"{node_id:08x}"
        self.lat = lat
        self.lon = lon
        self.light_state = False  # False for off, True for on
        self.is_running = False

        # Set up dual isolated MockLoRa interfaces on the same network ID (self.node_id)
        # app_lora represents the node's local user interface
        self.app_lora = MockLoRaInterface(node_id=self.node_id, network_id=self.node_id)
        # router_lora represents the bridge/router LoRa module side
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
        self.config.data["name"] = f"light-node-{self.node_id_hex}"
        self.config.data["wifi"]["udp_port"] = udp_port
        self.config.data["wifi"]["multicast_group"] = multicast_group

        # Initialize router with router_lora
        self.router = PacketRouter(config=self.config, lora=self.router_lora, wifi=self.wifi)

        # Register callback on app_lora to process incoming application packets
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
        logger.info(f"Light Node {self.node_id_hex} started at GPS ({self.lat}, {self.lon})")

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
        logger.info(f"Light Node {self.node_id_hex} stopped")

    def handle_incoming_lora(self, payload: bytes):
        """
        Processes incoming application layer messages.
        """
        try:
            message = json.loads(payload.decode("utf-8"))
        except Exception:
            # Ignore non-JSON packets
            return

        if message.get("channel") != "#QR":
            return

        msg_type = message.get("type")
        if msg_type == "command":
            data = message.get("data", {})
            target = data.get("target")
            if target == self.node_id_hex:
                new_state = data.get("light") == "on"
                if self.light_state != new_state:
                    self.light_state = new_state
                    logger.info(f"Node {self.node_id_hex} light changed to: {'ON' if self.light_state else 'OFF'}")

    async def _advertise_loop(self):
        """
        Periodically advertises GPS position over the `#QR` channel with limited scope.
        """
        # Small random initial delay to desynchronize transmissions
        await asyncio.sleep(0.1)
        while self.is_running:
            try:
                await self.advertise_position()
            except Exception as e:
                logger.error(f"Error in advertise loop for {self.node_id_hex}: {e}")
            await asyncio.sleep(5.0)  # Advertise every 5 seconds

    async def advertise_position(self):
        """
        Sends an advertise packet with limited scope on the `#QR` channel.
        """
        payload_dict = {
            "channel": "#QR",
            "sender": self.node_id_hex,
            "scope": "limited",
            "type": "advertise",
            "data": {
                "lat": self.lat,
                "lon": self.lon
            }
        }
        payload_bytes = json.dumps(payload_dict).encode("utf-8")
        # Transmit via our local application interface.
        # The local router_lora will receive it and PacketRouter will forward it over UDP.
        await self.app_lora.transmit(payload_bytes)
        logger.debug(f"Node {self.node_id_hex} advertised position ({self.lat}, {self.lon})")
