import json
import logging
import asyncio
import qrcode
import math
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter

logger = logging.getLogger("mwb_demo.coordinator")

class CoordinatorNode:
    """
    Coordinator node that joins `#QR` channel, gathers GPS positions of the nodes,
    sorts them in a grid, and translates global scope messages to QR code control commands.
    Uses dual-LoRa interfaces: one for the app, one for the router, on the same network ID.
    """
    def __init__(self, node_id: int, udp_port: int = 4500, multicast_group: str = "239.10.10.10"):
        self.node_id = node_id
        self.node_id_hex = f"{node_id:08x}"
        self.is_running = False

        # Node positions registry: map of node_id_hex -> (lat, lon)
        self.node_positions = {}
        # 2D Grid of node_id_hex values: list of lists
        self.grid = []
        self.grid_rows = 0
        self.grid_cols = 0

        # Set up dual MockLoRa interfaces on the same network ID (self.node_id)
        self.app_lora = MockLoRaInterface(node_id=self.node_id, network_id=self.node_id)
        self.router_lora = MockLoRaInterface(node_id=self.node_id + 100000, network_id=self.node_id)

        self.wifi = UDPTransport(
            mode="multicast",
            port=udp_port,
            multicast_group=multicast_group
        )

        self.config = Config()
        self.config.data["id"] = self.node_id_hex
        self.config.data["name"] = f"coordinator-{self.node_id_hex}"
        self.config.data["wifi"]["udp_port"] = udp_port
        self.config.data["wifi"]["multicast_group"] = multicast_group

        self.router = PacketRouter(config=self.config, lora=self.router_lora, wifi=self.wifi)

        # Register callback on app_lora to receive and process packets
        self.app_lora.register_rx_callback(self.handle_incoming_lora)

    async def start(self):
        self.is_running = True
        self.router.start()
        await self.app_lora.start()
        await self.router_lora.start()
        await self.wifi.start()
        logger.info(f"Coordinator Node {self.node_id_hex} started")

    async def stop(self):
        self.is_running = False
        await self.wifi.stop()
        await self.app_lora.stop()
        await self.router_lora.stop()
        logger.info(f"Coordinator Node {self.node_id_hex} stopped")

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
        sender = message.get("sender")

        if msg_type == "advertise":
            data = message.get("data", {})
            lat = data.get("lat")
            lon = data.get("lon")
            if lat is not None and lon is not None:
                self.node_positions[sender] = (lat, lon)
                logger.info(f"Coordinator registered/updated node {sender} at ({lat:.4f}, {lon:.4f})")
                self.sort_nodes_into_grid()

        elif msg_type == "message":
            scope = message.get("scope")
            if scope == "global":
                text = message.get("data", {}).get("text", "")
                logger.info(f"Coordinator received Global Message from {sender}: '{text}'")
                # Translate message to QR and display on grid
                asyncio.create_task(self.display_message_as_qr(text))

    def sort_nodes_into_grid(self):
        """
        Sorts registered nodes into a 2D grid based on GPS positions.
        - Sorts by latitude descending (North to South) to establish rows.
        - Divides rows and sorts each row by longitude ascending (West to East).
        """
        num_nodes = len(self.node_positions)
        if num_nodes == 0:
            self.grid = []
            self.grid_rows = 0
            self.grid_cols = 0
            return

        # Dynamically calculate grid dimensions: try to form a square-ish grid
        # For example, if we have 9 nodes, grid size is 3x3. If 16, 4x4.
        # If we have 12 nodes, we can do 3x4 or 4x3.
        # Let's define: cols = ceil(sqrt(N)), rows = ceil(N / cols)
        self.grid_cols = int(math.ceil(math.sqrt(num_nodes)))
        self.grid_rows = int(math.ceil(num_nodes / self.grid_cols))

        # Sort all nodes by latitude descending (North to South)
        sorted_by_lat = sorted(
            self.node_positions.items(),
            key=lambda item: item[1][0],
            reverse=True
        )

        grid = []
        for r in range(self.grid_rows):
            # Extract nodes belonging to this row
            start_idx = r * self.grid_cols
            end_idx = min(start_idx + self.grid_cols, num_nodes)
            row_items = sorted_by_lat[start_idx:end_idx]

            # Sort this row by longitude ascending (West to East)
            sorted_row = sorted(row_items, key=lambda item: item[1][1])
            row_node_ids = [item[0] for item in sorted_row]

            # Pad row if it has fewer elements than grid_cols
            while len(row_node_ids) < self.grid_cols:
                row_node_ids.append(None)

            grid.append(row_node_ids)

        self.grid = grid
        logger.debug(f"Updated grid sorting: {self.grid_rows}x{self.grid_cols} grid.")

    async def display_message_as_qr(self, text: str):
        """
        Generates a QR code for `text`, maps it onto the sorted grid of lights nodes,
        and transmits commands to individual nodes to control their lights.
        """
        if not self.grid or len(self.node_positions) == 0:
            logger.warning("No nodes registered. Cannot display QR code.")
            return

        # 1. Generate QR code matrix using `qrcode` library
        qr = qrcode.QRCode(version=1, box_size=1, border=0)
        qr.add_data(text)
        qr.make(fit=True)
        qr_matrix = qr.modules
        qr_height = len(qr_matrix)
        qr_width = len(qr_matrix[0]) if qr_height > 0 else 0

        if qr_height == 0 or qr_width == 0:
            logger.warning("Failed to generate QR code.")
            return

        logger.info(f"Generated QR Code matrix of size {qr_width}x{qr_height} for text '{text}'")

        # 2. Iterate through each cell of our grid and command the corresponding node
        for r in range(self.grid_rows):
            for c in range(self.grid_cols):
                node_id = self.grid[r][c]
                if not node_id:
                    continue

                # Map grid coordinate (r, c) to QR matrix module coordinate
                qr_r = int(r * qr_height / self.grid_rows)
                qr_c = int(c * qr_width / self.grid_cols)

                # Fetch pixel state: True means black/dark module, False means white/light
                # We turn the node light ON for dark pixels and OFF for light pixels
                pixel_state = qr_matrix[qr_r][qr_c]
                light_command = "on" if pixel_state else "off"

                # Send command to node
                cmd_dict = {
                    "channel": "#QR",
                    "sender": self.node_id_hex,
                    "scope": "limited",
                    "type": "command",
                    "data": {
                        "target": node_id,
                        "light": light_command
                    }
                }
                cmd_bytes = json.dumps(cmd_dict).encode("utf-8")
                await self.app_lora.transmit(cmd_bytes)
                # Small delay between commands to avoid bursting too fast
                await asyncio.sleep(0.02)

        logger.info("Successfully dispatched light control commands to nodes matching the QR code.")
