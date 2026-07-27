import asyncio
import argparse
import logging
import sys
import json
from mwb_demo.node import GPSEnabledLightNode
from mwb_demo.coordinator import CoordinatorNode
from mwb.wifi.udp_transport import UDPTransport
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.router import PacketRouter
from mwb.config import Config

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger("mwb_demo.simulation")

class SimulationRunner:
    """
    Simulates a grid of GPS-enabled lights nodes, a coordinator node, and a global scope user.
    """
    def __init__(self, grid_size: int = 4, text: str = "QR", multicast_port: int = 4500):
        self.grid_size = grid_size
        self.text = text
        self.multicast_port = multicast_port

        self.nodes = []
        self.coordinator = None
        self.is_running = False

    async def run(self, duration: float = 12.0):
        self.is_running = True
        logger.info(f"Setting up simulation with a {self.grid_size}x{self.grid_size} grid of lights nodes...")

        # 1. Start the Coordinator Node (using high ID like 0x999)
        self.coordinator = CoordinatorNode(node_id=0x9999, udp_port=self.multicast_port)
        await self.coordinator.start()

        # 2. Start the GPSEnabledLightNode instances
        # Generate them in a grid pattern.
        # We vary the GPS coordinates slightly to form a perfect coordinates grid:
        # Base Lat: 37.7749, Base Lon: -122.4194
        base_lat = 37.7749
        base_lon = -122.4194
        node_id_counter = 1

        for r in range(self.grid_size):
            for c in range(self.grid_size):
                # Latitude decreases as we go South, Longitude increases as we go East
                lat = base_lat - (r * 0.01)
                lon = base_lon + (c * 0.01)

                node = GPSEnabledLightNode(
                    node_id=node_id_counter,
                    lat=lat,
                    lon=lon,
                    udp_port=self.multicast_port
                )
                self.nodes.append(node)
                await node.start()
                node_id_counter += 1

        # Give nodes a few seconds to advertise positions and coordinator to gather/sort them
        logger.info("Waiting for nodes to join and advertise their GPS positions...")
        await asyncio.sleep(4.0)

        # Print current coordinator grid configuration
        self.print_node_positions()

        # 3. Simulate a Global Scope User posting a message to the `#QR` channel
        logger.info(f"Simulating Global Scope User posting message: '{self.text}' to '#QR' channel...")
        await self.post_global_message(self.text)

        # Wait a moment for commands to propagate and execute
        await asyncio.sleep(3.0)

        # Print final lights status grid visualization!
        self.print_lights_grid()

        # Wait for the remaining duration
        remaining = max(0.1, duration - 7.0)
        await asyncio.sleep(remaining)

        # Clean up
        logger.info("Stopping all nodes and coordinator...")
        for node in self.nodes:
            await node.stop()
        await self.coordinator.stop()
        self.is_running = False
        logger.info("Simulation completed.")

    def print_node_positions(self):
        """Prints the grid coordinates configuration of all discovered nodes."""
        print("\n=== Discovered Nodes and Grid Configuration ===")
        if not self.coordinator.grid:
            print("Coordinator hasn't formed any grid yet.")
            return

        for r in range(len(self.coordinator.grid)):
            row_str = []
            for c in range(len(self.coordinator.grid[r])):
                node_id = self.coordinator.grid[r][c]
                if node_id:
                    pos = self.coordinator.node_positions[node_id]
                    row_str.append(f"{node_id}({pos[0]:.4f},{pos[1]:.4f})")
                else:
                    row_str.append("None")
            print("  |  ".join(row_str))
        print("===============================================\n")

    def print_lights_grid(self):
        """Displays a beautiful visualization of the lights grid."""
        print("\n=== LIGHTS GRID STATUS ===")
        if not self.coordinator.grid:
            print("No grid sorted.")
            return

        # Let's map node_id_hex to node object to get the actual live state
        nodes_map = {node.node_id_hex: node for node in self.nodes}

        for r in range(len(self.coordinator.grid)):
            row_chars = []
            for c in range(len(self.coordinator.grid[r])):
                node_id = self.coordinator.grid[r][c]
                if node_id and node_id in nodes_map:
                    is_on = nodes_map[node_id].light_state
                    row_chars.append("[🔴]" if is_on else "[⚫]")
                else:
                    row_chars.append("[  ]")
            print(" ".join(row_chars))
        print("==========================\n")

    async def post_global_message(self, text: str):
        """
        Simulates a user posting a global message to the #QR channel.
        Uses dual MockLoRa interfaces to safely inject transmission.
        """
        # Set up dual isolated MockLoRa interfaces on the same network ID (0xEEEE)
        user_app_lora = MockLoRaInterface(node_id=0xEEEE, network_id=0xEEEE)
        user_router_lora = MockLoRaInterface(node_id=0xEEEE + 100000, network_id=0xEEEE)

        user_wifi = UDPTransport(mode="multicast", port=self.multicast_port)

        # Router to bridge the global user's LoRa transmission to UDP
        user_config = Config()
        user_config.data["id"] = "0000eeee"
        user_config.data["name"] = "global-user"
        user_config.data["wifi"]["udp_port"] = self.multicast_port

        user_router = PacketRouter(config=user_config, lora=user_router_lora, wifi=user_wifi)
        user_router.start()

        await user_app_lora.start()
        await user_router_lora.start()
        await user_wifi.start()

        # Construct and send the global message payload
        msg_dict = {
            "channel": "#QR",
            "sender": "0000eeee",
            "scope": "global",
            "type": "message",
            "data": {
                "text": text
            }
        }
        msg_bytes = json.dumps(msg_dict).encode("utf-8")
        await user_app_lora.transmit(msg_bytes)

        # Keep alive briefly for transmission delivery
        await asyncio.sleep(0.5)

        # Stop user interfaces
        await user_wifi.stop()
        await user_app_lora.stop()
        await user_router_lora.stop()

def main():
    parser = argparse.ArgumentParser(description="MWB Lights Grid & QR Code Simulator")
    parser.add_argument("--grid-size", type=int, default=4, help="Size of grid (e.g. 3 for 3x3, 4 for 4x4)")
    parser.add_argument("--text", type=str, default="HELLO WORLD", help="Text to convert to QR code")
    parser.add_argument("--duration", type=float, default=12.0, help="Duration of simulation in seconds")
    parser.add_argument("--port", type=int, default=4500, help="Multicast port to use")

    args = parser.parse_args()

    runner = SimulationRunner(
        grid_size=args.grid_size,
        text=args.text,
        multicast_port=args.port
    )
    try:
        asyncio.run(runner.run(duration=args.duration))
    except KeyboardInterrupt:
        logger.info("Simulation interrupted by user.")

if __name__ == "__main__":
    main()
