import asyncio
import logging
from mwb.lora.base import LoRaInterface

logger = logging.getLogger("mwb.lora.mock")

class MockLoRaInterface(LoRaInterface):
    """
    A simulated/mock LoRa interface to allow software loopback or multi-node testing on a single machine.
    Uses a shared register of instances to allow communication between mock interfaces.
    """
    _instances = []

    def __init__(self, node_id: int, network_id: int = 1):
        super().__init__()
        self.node_id = node_id
        self.network_id = network_id
        self.is_running = False
        MockLoRaInterface._instances.append(self)

    @classmethod
    def clear_instances(cls):
        cls._instances.clear()

    async def start(self):
        self.is_running = True
        logger.info(f"Mock LoRa Interface started for node_id: {self.node_id:012x} on network: {self.network_id}")

    async def stop(self):
        self.is_running = False
        if self in MockLoRaInterface._instances:
            MockLoRaInterface._instances.remove(self)
        logger.info(f"Mock LoRa Interface stopped for node_id: {self.node_id:012x}")

    async def transmit(self, payload: bytes) -> bool:
        if not self.is_running:
            logger.warning("Mock LoRa Interface is not running. Transmit ignored.")
            return False

        logger.debug(f"Mock LoRa (Node {self.node_id:012x}) transmitting {len(payload)} bytes")

        # Simulating transmission to other mock interfaces on the SAME mock network ID
        loop = asyncio.get_event_loop()
        for instance in list(MockLoRaInterface._instances):
            if instance != self and instance.is_running and instance.network_id == self.network_id:
                # Dispatch delivery in a separate event loop iteration to mimic network delay
                loop.call_soon(instance._receive_simulated, payload)
        return True

    def _receive_simulated(self, payload: bytes):
        if self.is_running and self.rx_callback:
            try:
                self.rx_callback(payload)
            except Exception as e:
                logger.exception(f"Error in mock rx_callback: {e}")
