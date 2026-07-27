import asyncio
import json
import pytest
from mwb_demo.node import GPSEnabledLightNode
from mwb_demo.coordinator import CoordinatorNode
from mwb_demo.simulation import SimulationRunner

def test_node_properties():
    node = GPSEnabledLightNode(node_id=42, lat=37.123, lon=-122.456, udp_port=4999)
    assert node.node_id == 42
    assert node.node_id_hex == "0000002a"
    assert node.lat == 37.123
    assert node.lon == -122.456
    assert node.light_state is False
    assert node.is_running is False

def test_coordinator_grid_sorting():
    coordinator = CoordinatorNode(node_id=999, udp_port=4999)

    # Let's register 4 nodes in a 2x2 layout, but register them in chaotic order
    # (r0, c0) -> lat=37.8, lon=-122.8
    # (r0, c1) -> lat=37.8, lon=-122.2
    # (r1, c0) -> lat=37.2, lon=-122.8
    # (r1, c1) -> lat=37.2, lon=-122.2

    coordinator.node_positions = {
        "node_c": (37.2, -122.8), # r1, c0
        "node_a": (37.8, -122.8), # r0, c0
        "node_d": (37.2, -122.2), # r1, c1
        "node_b": (37.8, -122.2), # r0, c1
    }

    coordinator.sort_nodes_into_grid()

    assert coordinator.grid_rows == 2
    assert coordinator.grid_cols == 2

    # First row (top-most latitude 37.8, sorted West to East lon: -122.8 < -122.2)
    assert coordinator.grid[0] == ["node_a", "node_b"]
    # Second row (bottom-most latitude 37.2, sorted West to East lon: -122.8 < -122.2)
    assert coordinator.grid[1] == ["node_c", "node_d"]

@pytest.mark.asyncio
async def test_coordinator_qr_command_generation():
    coordinator = CoordinatorNode(node_id=999, udp_port=4999)

    # Register 4 nodes in a 2x2 grid
    coordinator.node_positions = {
        "node_1": (37.8, -122.8),
        "node_2": (37.8, -122.2),
        "node_3": (37.2, -122.8),
        "node_4": (37.2, -122.2),
    }
    coordinator.sort_nodes_into_grid()

    # Intercept commands sent over LoRa
    sent_commands = []

    async def mock_transmit(payload: bytes) -> bool:
        sent_commands.append(json.loads(payload.decode("utf-8")))
        return True

    coordinator.app_lora.transmit = mock_transmit

    # Display message "A"
    await coordinator.display_message_as_qr("A")

    assert len(sent_commands) == 4
    for cmd in sent_commands:
        assert cmd["channel"] == "#QR"
        assert cmd["sender"] == "000003e7"
        assert cmd["type"] == "command"
        assert cmd["data"]["target"] in ["node_1", "node_2", "node_3", "node_4"]
        assert cmd["data"]["light"] in ["on", "off"]

@pytest.mark.asyncio
async def test_mini_simulation_integration():
    # Run a miniature version of the simulation (2x2 grid, duration of 3.0 seconds)
    # on a non-default port to avoid interference with other local tests.
    runner = SimulationRunner(grid_size=2, text="OK", multicast_port=4998)

    # We will run the simulation for a short duration
    sim_task = asyncio.create_task(runner.run(duration=5.0))

    # Wait for the simulation to run and shut down
    await sim_task

    assert len(runner.nodes) == 4
    assert runner.coordinator is not None
    assert runner.coordinator.grid_rows == 2
    assert runner.coordinator.grid_cols == 2

    # Ensure all nodes and coordinator are cleanly stopped
    for node in runner.nodes:
        assert node.is_running is False
    assert runner.coordinator.is_running is False
