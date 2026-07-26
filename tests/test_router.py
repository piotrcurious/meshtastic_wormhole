import asyncio
import pytest
import urllib.request
import json
from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from mwb.diagnostics import DiagnosticsServer

@pytest.mark.asyncio
async def test_router_and_diagnostics():
    # 1. Setup config
    cfg = Config()
    cfg.data["diagnostics"]["port"] = 9091
    cfg.data["wifi"]["udp_port"] = 4600

    # 2. Setup interfaces
    lora = MockLoRaInterface(node_id=cfg.int_id)
    wifi = UDPTransport(mode="multicast", port=cfg.udp_port)

    # 3. Setup router
    router = PacketRouter(config=cfg, lora=lora, wifi=wifi)
    router.start()

    # 4. Setup diagnostics server
    diag = DiagnosticsServer(router=router)

    await lora.start()
    await wifi.start()
    await diag.start()

    # Allow servers to bind and start
    await asyncio.sleep(0.2)

    # 5. Fetch and verify diagnostics response
    def fetch_status():
        with urllib.request.urlopen("http://127.0.0.1:9091/status") as res:
            return json.loads(res.read().decode())

    loop = asyncio.get_running_loop()
    status = await loop.run_in_executor(None, fetch_status)

    assert status["name"] == cfg.name
    assert status["id"] == cfg.id
    assert status["stats"]["lora_rx"] == 0

    # 6. Stop all
    await diag.stop()
    await wifi.stop()
    await lora.stop()
