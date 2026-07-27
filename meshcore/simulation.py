import asyncio
import argparse
import logging
import sys
from typing import List
from meshcore.region import Region
from meshcore.node import GPSEnabledLightNode
from meshcore.coordinator import CoordinatorNode
from mwb.wifi.udp_transport import UDPTransport
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.router import PacketRouter
from mwb.config import Config
from meshcore.packet import MeshCorePacket

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger("meshcore.simulation")

class MeshCoreSimulationRunner:
    """
    Simulates a multi-region spatial layout of nodes (SOMA vs Mission) to demonstrate
    region-based scope filtering and localized QR code lights grids control.
    """
    def __init__(self, multicast_port: int = 4501):
        self.multicast_port = multicast_port

        # Define Geographical Regions
        # SOMA center near 37.77, -122.41
        self.soma_region = Region("SOMA", 37.7700, -122.4100, 1500.0)
        # Mission center near 37.75, -122.41 (distance SOMA-Mission ~2.2km)
        self.mission_region = Region("Mission", 37.7500, -122.4100, 1500.0)
        self.regions = [self.soma_region, self.mission_region]

        self.nodes: List[GPSEnabledLightNode] = []
        self.coordinator = None

    async def run(self, duration: float = 15.0):
        logger.info("Initializing MeshCore Multi-Region Simulation...")

        # 1. Start Coordinator Node
        self.coordinator = CoordinatorNode(node_id=0x9999, regions=self.regions, udp_port=self.multicast_port)
        await self.coordinator.start()

        # 2. Spawn SOMA region nodes (Grid of 4 nodes, IDs 1 to 4)
        logger.info("Spawning SOMA Region nodes...")
        soma_positions = [
            (37.7720, -122.4120), (37.7720, -122.4080),
            (37.7680, -122.4120), (37.7680, -122.4080)
        ]
        for i, pos in enumerate(soma_positions):
            node_id = 1 + i
            node = GPSEnabledLightNode(
                node_id=node_id,
                lat=pos[0],
                lon=pos[1],
                candidate_regions=self.regions,
                udp_port=self.multicast_port
            )
            self.nodes.append(node)
            await node.start()

        # 3. Spawn Mission region nodes (Grid of 4 nodes, IDs 11 to 14)
        logger.info("Spawning Mission Region nodes...")
        mission_positions = [
            (37.7520, -122.4120), (37.7520, -122.4080),
            (37.7480, -122.4120), (37.7480, -122.4080)
        ]
        for i, pos in enumerate(mission_positions):
            node_id = 11 + i
            node = GPSEnabledLightNode(
                node_id=node_id,
                lat=pos[0],
                lon=pos[1],
                candidate_regions=self.regions,
                udp_port=self.multicast_port
            )
            self.nodes.append(node)
            await node.start()

        # Give nodes time to join and advertise position
        logger.info("Waiting for nodes to join and advertise coordinates geographically...")
        await asyncio.sleep(4.0)

        # Print current dynamic regional grids
        self.print_regional_configurations()

        # 4. Trigger localized message targeting ONLY SOMA region!
        logger.info("=== STEP 1: Sending 'HELLO SOMA' targeting 'SOMA' region (Regional Scope) ===")
        await self.post_message(text="HELLO SOMA", scope="regional", region="SOMA")
        await asyncio.sleep(3.0)
        self.print_lights_grid()

        # Reset all lights for next step
        self.reset_all_lights()

        # 5. Trigger localized message targeting ONLY Mission region!
        logger.info("=== STEP 2: Sending 'HELLO MISSION' targeting 'Mission' region (Regional Scope) ===")
        await self.post_message(text="HELLO MISSION", scope="regional", region="Mission")
        await asyncio.sleep(3.0)
        self.print_lights_grid()

        # Reset all lights for next step
        self.reset_all_lights()

        # 6. Trigger global scope message targeting all nodes!
        logger.info("=== STEP 3: Sending 'GLOBAL NEWS' to all nodes (Global Scope) ===")
        await self.post_message(text="GLOBAL NEWS", scope="global")
        await asyncio.sleep(3.0)
        self.print_lights_grid()

        # Stop everything
        logger.info("Stopping all nodes and coordinator...")
        for node in self.nodes:
            await node.stop()
        await self.coordinator.stop()
        logger.info("MeshCore Simulation Complete.")

    def reset_all_lights(self):
        for node in self.nodes:
            node.light_state = False

    def print_regional_configurations(self):
        print("\n=== MeshCore Discovered Regional Configurations ===")
        for r_name in ["SOMA", "Mission"]:
            grid = self.coordinator.regional_grids.get(r_name, [])
            print(f" Region: {r_name} ({len(self.coordinator.regional_nodes[r_name])} registered nodes)")
            if not grid:
                print("   [No grid formed]")
                continue
            for r in range(len(grid)):
                row_str = []
                for c in range(len(grid[r])):
                    node_id = grid[r][c]
                    if node_id:
                        pos = self.coordinator.regional_nodes[r_name][node_id]
                        row_str.append(f"{node_id}({pos[0]:.4f},{pos[1]:.4f})")
                    else:
                        row_str.append("None")
                print("     " + "  |  ".join(row_str))
        print("===================================================\n")

    def print_lights_grid(self):
        print("\n=== REGIONAL LIGHTS GRID STATUS ===")
        nodes_map = {node.node_id_hex: node for node in self.nodes}

        for r_name in ["SOMA", "Mission"]:
            grid = self.coordinator.regional_grids.get(r_name, [])
            print(f" Region: {r_name}")
            if not grid:
                print("   [No grid]")
                continue
            for r in range(len(grid)):
                row_chars = []
                for c in range(len(grid[r])):
                    node_id = grid[r][c]
                    if node_id and node_id in nodes_map:
                        is_on = nodes_map[node_id].light_state
                        row_chars.append("[🔴]" if is_on else "[⚫]")
                    else:
                        row_chars.append("[  ]")
                print("     " + " ".join(row_chars))
        print("===================================\n")

    async def post_message(self, text: str, scope: str, region: str = None):
        """
        Simulates a user injecting a message with specific scope/region parameters.
        """
        user_app_lora = MockLoRaInterface(node_id=0xEEEE, network_id=0xEEEE)
        user_router_lora = MockLoRaInterface(node_id=0xEEEE + 100000, network_id=0xEEEE)
        user_wifi = UDPTransport(mode="multicast", port=self.multicast_port)

        user_config = Config()
        user_config.data["id"] = "0000eeee"
        user_config.data["name"] = "meshcore-user"
        user_config.data["wifi"]["udp_port"] = self.multicast_port

        user_router = PacketRouter(config=user_config, lora=user_router_lora, wifi=user_wifi)
        user_router.start()

        await user_app_lora.start()
        await user_router_lora.start()
        await user_wifi.start()

        # Build MeshCorePacket
        packet = MeshCorePacket(
            channel="#QR",
            sender="0000eeee",
            scope=scope,
            type="message",
            data={"text": text},
            region=region
        )
        payload_bytes = packet.to_json().encode("utf-8")
        await user_app_lora.transmit(payload_bytes)

        await asyncio.sleep(0.5)

        await user_wifi.stop()
        await user_app_lora.stop()
        await user_router_lora.stop()

def main():
    parser = argparse.ArgumentParser(description="MeshCore Multi-Region Simulation")
    parser.add_argument("--port", type=int, default=4501, help="Multicast port to use")
    args = parser.parse_args()

    runner = MeshCoreSimulationRunner(multicast_port=args.port)
    try:
        asyncio.run(runner.run())
    except KeyboardInterrupt:
        logger.info("Simulation interrupted.")

if __name__ == "__main__":
    main()
