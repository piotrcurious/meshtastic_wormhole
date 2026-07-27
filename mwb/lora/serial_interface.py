import asyncio
import logging
from typing import Optional, Any
from mwb.lora.base import LoRaInterface

logger = logging.getLogger("mwb.lora.serial")

class SerialLoRaInterface(LoRaInterface):
    """
    Physical Serial LoRa driver. Communicates with hardware over a USB serial connection.
    Uses standard SLIP-style framing (RFC 1055) to frame raw Meshtastic packets safely:
    - SLIP_END (0xC0) starts/ends frames.
    - SLIP_ESC (0xDB) escapes special characters.
    - SLIP_ESC_END (0xDC) represents an escaped 0xC0.
    - SLIP_ESC_ESC (0xDD) represents an escaped 0xDB.
    """
    SLIP_END = 0xC0
    SLIP_ESC = 0xDB
    SLIP_ESC_END = 0xDC
    SLIP_ESC_ESC = 0xDD

    def __init__(self, port: str, baudrate: int = 115200, serial_obj: Optional[Any] = None):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.serial = serial_obj  # Allows injecting mock serial ports during unit testing
        self.is_running = False
        self._rx_task: Optional[asyncio.Task] = None

        # State tracking for SLIP decoding across read chunk boundaries
        self._slip_buffer = bytearray()
        self._in_escape = False

    async def start(self):
        try:
            import serial
            if self.serial is None:
                self.serial = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self.is_running = True
            self._rx_task = asyncio.create_task(self._read_loop())
            logger.info(f"Serial LoRa Interface started on port {self.port} at {self.baudrate}")
        except Exception as e:
            logger.error(f"Failed to open serial port {self.port}: {e}")
            raise e

    async def stop(self):
        self.is_running = False
        if self._rx_task:
            self._rx_task.cancel()
            try:
                await self._rx_task
            except asyncio.CancelledError:
                pass
        if self.serial and hasattr(self.serial, 'is_open') and self.serial.is_open:
            self.serial.close()
            logger.info(f"Serial port {self.port} closed.")

    @classmethod
    def encode_slip(cls, data: bytes) -> bytes:
        """Encode a payload using standard SLIP framing."""
        encoded = bytearray([cls.SLIP_END])
        for byte in data:
            if byte == cls.SLIP_END:
                encoded.append(cls.SLIP_ESC)
                encoded.append(cls.SLIP_ESC_END)
            elif byte == cls.SLIP_ESC:
                encoded.append(cls.SLIP_ESC)
                encoded.append(cls.SLIP_ESC_ESC)
            else:
                encoded.append(byte)
        encoded.append(cls.SLIP_END)
        return bytes(encoded)

    def decode_slip_step(self, raw_bytes: bytes) -> list:
        """
        Parses raw incoming serial bytes and extracts SLIP frames.
        Uses instance-bound buffers to robustly handle read boundary splits.
        Returns a list of decoded frames.
        """
        frames = []

        for byte in raw_bytes:
            if byte == self.SLIP_END:
                if len(self._slip_buffer) > 0:
                    frames.append(bytes(self._slip_buffer))
                    self._slip_buffer.clear()
                self._in_escape = False
            elif byte == self.SLIP_ESC:
                self._in_escape = True
            elif self._in_escape:
                if byte == self.SLIP_ESC_END:
                    self._slip_buffer.append(self.SLIP_END)
                elif byte == self.SLIP_ESC_ESC:
                    self._slip_buffer.append(self.SLIP_ESC)
                else:
                    # SLIP Protocol violation, append as-is or drop
                    self._slip_buffer.append(byte)
                self._in_escape = False
            else:
                self._slip_buffer.append(byte)

        return frames

    async def transmit(self, payload: bytes) -> bool:
        if not self.is_running or not self.serial:
            logger.warning("Serial interface is not running. Cannot transmit.")
            return False

        try:
            frame = self.encode_slip(payload)
            loop = asyncio.get_running_loop()
            await loop.run_in_executor(None, self.serial.write, frame)
            await loop.run_in_executor(None, self.serial.flush)
            return True
        except Exception as e:
            logger.error(f"Error transmitting over serial SLIP frame: {e}")
            return False

    async def _read_loop(self):
        loop = asyncio.get_running_loop()

        while self.is_running:
            try:
                # Read incoming serial bytes
                data = await loop.run_in_executor(None, self.serial.read, 1024)
                if data:
                    decoded_frames = self.decode_slip_step(data)
                    for frame in decoded_frames:
                        if self.rx_callback:
                            try:
                                self.rx_callback(frame)
                            except Exception as e:
                                logger.error(f"Error in Serial SLIP RX callback: {e}")
                else:
                    await asyncio.sleep(0.05)
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in serial read loop: {e}")
                await asyncio.sleep(1)
