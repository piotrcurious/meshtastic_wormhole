from abc import ABC, abstractmethod
from typing import Callable, Optional

class LoRaInterface(ABC):
    """
    Abstract interface for LoRa radio communication.
    Supports asynchronous packet reception callback and synchronous/asynchronous sending.
    """
    def __init__(self):
        self.rx_callback: Optional[Callable[[bytes], None]] = None

    def register_rx_callback(self, callback: Callable[[bytes], None]):
        """Register a callback for when a raw Meshtastic frame is received."""
        self.rx_callback = callback

    @abstractmethod
    async def start(self):
        """Start the interface listeners/tasks."""
        pass

    @abstractmethod
    async def stop(self):
        """Stop the interface and clean up resources."""
        pass

    @abstractmethod
    async def transmit(self, payload: bytes) -> bool:
        """Transmit a raw Meshtastic frame over LoRa."""
        pass
