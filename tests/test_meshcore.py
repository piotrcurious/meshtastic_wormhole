import asyncio
import json
import pytest
from meshcore.region import Region
from meshcore.packet import MeshCorePacket
from meshcore.node import GPSEnabledLightNode
from meshcore.coordinator import CoordinatorNode
from meshcore.simulation import MeshCoreSimulationRunner

def test_haversine_distance_and_region_contains():
    # Test Center: (37.77, -122.41)
    soma = Region("SOMA", 37.7700, -122.4100, 1000.0) # 1000m radius

    # Coordinates extremely close (~220m) should be inside
    assert soma.contains(37.771, -122.411) is True

    # Coordinates far away (>20km) should be outside
    assert soma.contains(37.950, -122.100) is False

def test_mesh_core_packet_geographical_filtering():
    soma = Region("SOMA", 37.7700, -122.4100, 1000.0)
    mission = Region("Mission", 37.7500, -122.4100, 1000.0)
    regions = [soma, mission]

    # Global scope packet should be valid everywhere
    p_global = MeshCorePacket(
        channel="#QR", sender="usr", scope="global", type="command", data={}
    )
    assert p_global.is_valid_for_node(37.771, -122.411, [soma]) is True

    # Regional scope packet targeting 'Mission' should NOT be valid on a SOMA node
    p_regional = MeshCorePacket(
        channel="#QR", sender="usr", scope="regional", type="command", data={}, region="Mission"
    )
    assert p_regional.is_valid_for_node(37.771, -122.411, [soma]) is False
    # But valid on a Mission node
    assert p_regional.is_valid_for_node(37.751, -122.411, [mission]) is True

def test_coordinator_multi_region_grid_sorting():
    soma = Region("SOMA", 37.7700, -122.4100, 1000.0)
    mission = Region("Mission", 37.7500, -122.4100, 1000.0)
    regions = [soma, mission]

    coordinator = CoordinatorNode(node_id=123, regions=regions, udp_port=4990)

    # Manually register some nodes in SOMA and Mission
    # SOMA:
    coordinator.regional_nodes["SOMA"] = {
        "soma_node_2": (37.772, -122.408),
        "soma_node_1": (37.772, -122.412),
    }
    # Mission:
    coordinator.regional_nodes["Mission"] = {
        "mission_node_1": (37.752, -122.412),
        "mission_node_2": (37.752, -122.408),
    }

    coordinator.sort_region_into_grid("SOMA")
    coordinator.sort_region_into_grid("Mission")

    assert coordinator.regional_grids["SOMA"][0] == ["soma_node_1", "soma_node_2"]
    assert coordinator.regional_grids["Mission"][0] == ["mission_node_1", "mission_node_2"]

@pytest.mark.asyncio
async def test_mesh_core_mini_simulation_integration():
    # Run a miniature version of the multi-region simulation
    runner = MeshCoreSimulationRunner(multicast_port=4991)

    sim_task = asyncio.create_task(runner.run())

    # Wait for completion
    await sim_task

    assert len(runner.nodes) == 8
    assert runner.coordinator is not None
    assert len(runner.coordinator.regional_grids["SOMA"]) == 2
    assert len(runner.coordinator.regional_grids["Mission"]) == 2

    for node in runner.nodes:
        assert node.is_running is False
    assert runner.coordinator.is_running is False
