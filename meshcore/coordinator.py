import json
import logging
import asyncio
import qrcode
import math
from typing import Dict, Any, List
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from meshcore.region import Region
from meshcore.packet import MeshCorePacket

logger = logging.getLogger("meshcore.coordinator")

class CoordinatorNode:
    """
    Coordinator node that supports multi-region topological sorting, and regional scope targeting.
    Gathers node positions, assigns them to regions, and outputs targeted QR code grids.
    """
    def __init__(self, node_id: int, regions: List[Region], udp_port: int = 4500, multicast_group: str = "239.10.10.10"):
        self.node_id = node_id
        self.node_id_hex = f"{node_id:08x}"
        self.regions = regions
        self.is_running = False

        # Regional node registry: map of region_name -> { node_id_hex -> (lat, lon) }
        self.regional_nodes: Dict[str, Dict[str, tuple]] = {r.name: {} for r in regions}
        self.regional_nodes["unknown"] = {}

        # Regional grids registry: map of region_name -> 2D list of node_id_hex
        self.regional_grids: Dict[str, list] = {r.name: [] for r in regions}

        # Set up dual MockLoRa interfaces on the same network ID
        self.app_lora = MockLoRaInterface(node_id=self.node_id, network_id=self.node_id)
        self.router_lora = MockLoRaInterface(node_id=self.node_id + 100000, network_id=self.node_id)

        self.wifi = UDPTransport(
            mode="multicast",
            port=udp_port,
            multicast_group=multicast_group
        )

        self.config = Config()
        self.config.data["id"] = self.node_id_hex
        self.config.data["name"] = f"meshcore-coordinator-{self.node_id_hex}"
        self.config.data["wifi"]["udp_port"] = udp_port
        self.config.data["wifi"]["multicast_group"] = multicast_group

        self.router = PacketRouter(config=self.config, lora=self.router_lora, wifi=self.wifi)

        # Register callback
        self.app_lora.register_rx_callback(self.handle_incoming_lora)

    async def start(self):
        self.is_running = True
        self.router.start()
        await self.app_lora.start()
        await self.router_lora.start()
        await self.wifi.start()
        logger.info(f"MeshCore Coordinator Node {self.node_id_hex} started")

    async def stop(self):
        self.is_running = False
        await self.wifi.stop()
        await self.app_lora.stop()
        await self.router_lora.stop()
        logger.info(f"MeshCore Coordinator Node {self.node_id_hex} stopped")

    def handle_incoming_lora(self, payload: bytes):
        """
        Processes incoming MeshCore application packets.
        """
        try:
            packet = MeshCorePacket.from_json(payload.decode("utf-8"))
        except Exception:
            return

        if packet.channel != "#QR":
            return

        msg_type = packet.type
        sender = packet.sender

        if msg_type == "advertise":
            data = packet.data
            lat = data.get("lat")
            lon = data.get("lon")
            assigned_region = "unknown"

            # Determine region geographically
            for r in self.regions:
                if r.contains(lat, lon):
                    assigned_region = r.name
                    break

            if lat is not None and lon is not None:
                # Add node to determined region
                self.regional_nodes[assigned_region][sender] = (lat, lon)
                logger.info(f"MeshCore Coordinator: Registered node {sender} in region '{assigned_region}' at ({lat:.4f}, {lon:.4f})")
                self.sort_region_into_grid(assigned_region)

        elif msg_type == "message":
            # Direct target message display triggered by global scope user
            if packet.scope in ["regional", "global"]:
                text = packet.data.get("text", "")
                target_region = packet.region if packet.scope == "regional" else None
                logger.info(f"MeshCore Coordinator: Received message: '{text}' (Scope: {packet.scope}, Region: {target_region})")
                asyncio.create_task(self.display_message_as_qr(text, target_region))

    def sort_region_into_grid(self, region_name: str):
        """
        Sorts registered nodes of a specific region into an independent 2D grid.
        """
        nodes_dict = self.regional_nodes.get(region_name, {})
        num_nodes = len(nodes_dict)
        if num_nodes == 0:
            self.regional_grids[region_name] = []
            return

        # Calculate grid size
        cols = int(math.ceil(math.sqrt(num_nodes)))
        rows = int(math.ceil(num_nodes / cols))

        # Sort all nodes by latitude descending (North to South)
        sorted_by_lat = sorted(
            nodes_dict.items(),
            key=lambda item: item[1][0],
            reverse=True
        )

        grid = []
        for r in range(rows):
            start_idx = r * cols
            end_idx = min(start_idx + cols, num_nodes)
            row_items = sorted_by_lat[start_idx:end_idx]

            # Sort row by longitude ascending (West to East)
            sorted_row = sorted(row_items, key=lambda item: item[1][1])
            row_node_ids = [item[0] for item in sorted_row]

            while len(row_node_ids) < cols:
                row_node_ids.append(None)
            grid.append(row_node_ids)

        self.regional_grids[region_name] = grid
        logger.debug(f"MeshCore Coordinator: Updated grid for region '{region_name}' to size {rows}x{cols}")

    async def display_message_as_qr(self, text: str, target_region: str = None):
        """
        Generates QR code for text and dispatches light commands.
        If target_region is None, commands are sent to all regions with 'global' scope.
        If target_region is set, commands are sent only to nodes in that region with 'regional' scope.
        """
        # Collect regions to target
        regions_to_target = [target_region] if target_region else list(self.regional_grids.keys())

        # 1. Generate QR Code module matrix
        qr = qrcode.QRCode(version=1, box_size=1, border=0)
        qr.add_data(text)
        qr.make(fit=True)
        qr_matrix = qr.modules
        qr_height = len(qr_matrix)
        qr_width = len(qr_matrix[0]) if qr_height > 0 else 0

        if qr_height == 0 or qr_width == 0:
            logger.warning("Failed to generate QR code.")
            return

        logger.info(f"MeshCore: Generated QR Code matrix {qr_width}x{qr_height} for text '{text}'")

        # 2. Iterate and dispatch regional/global commands
        for region_name in regions_to_target:
            grid = self.regional_grids.get(region_name, [])
            if not grid:
                continue

            grid_rows = len(grid)
            grid_cols = len(grid[0]) if grid_rows > 0 else 0

            for r in range(grid_rows):
                for c in range(grid_cols):
                    node_id = grid[r][c]
                    if not node_id:
                        continue

                    # Map coordinates to QR matrix module
                    qr_r = int(r * qr_height / grid_rows)
                    qr_c = int(c * qr_width / grid_cols)
                    pixel_state = qr_matrix[qr_r][qr_c]
                    light_command = "on" if pixel_state else "off"

                    # Determine command scope
                    scope = "regional" if target_region else "global"

                    packet = MeshCorePacket(
                        channel="#QR",
                        sender=self.node_id_hex,
                        scope=scope,
                        type="command",
                        data={
                            "target": node_id,
                            "light": light_command
                        },
                        region=region_name
                    )
                    payload_bytes = packet.to_json().encode("utf-8")
                    await self.app_lora.transmit(payload_bytes)
                    await asyncio.sleep(0.02)

        logger.info(f"MeshCore: Dispatched targeted lights commands for QR code display (Target Region: {target_region})")
