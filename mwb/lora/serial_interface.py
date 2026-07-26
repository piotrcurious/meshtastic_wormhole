import asyncio
import logging
import serial
from typing import Optional
from mwb.lora.base import LoRaInterface

logger = logging.getLogger("mwb.lora.serial")

class SerialLoRaInterface(LoRaInterface):
    """
    Physical Serial LoRa driver. Communicates with hardware over a USB serial connection.
    Assuming a line-buffered/packetized protocol or simple raw frames over USB serial.
    Many USB LoRa modules or custom ESP32/Pi hats support sending/receiving raw binary frames.
    Here we expect frames to be framed with a small prefix: standard SLIP framing or simpler:
    [Length (2 bytes)][Payload]
    """
    def __init__(self, port: str, baudrate: int = 115200):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.serial: Optional[serial.Serial] = None
        self.is_running = False
        self._rx_task: Optional[asyncio.Task] = None

    async def start(self):
        try:
            # open serial port in non-blocking/threaded manner, or use loop executor for read
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
        if self.serial and self.serial.is_open:
            self.serial.close()
            logger.info(f"Serial port {self.port} closed.")

    async def transmit(self, payload: bytes) -> bool:
        if not self.is_running or not self.serial or not self.serial.is_open:
            logger.warning("Serial port is not open. Cannot transmit.")
            return False

        try:
            # We pack with [length (2 bytes)][payload] for standard serial framing
            frame = len(payload).to_bytes(2, byteorder='big') + payload

            # Write in an executor to avoid blocking the asyncio event loop
            loop = asyncio.get_running_loop()
            await loop.run_in_executor(None, self.serial.write, frame)
            await loop.run_in_executor(None, self.serial.flush)
            return True
        except Exception as e:
            logger.error(f"Error transmitting over serial: {e}")
            return False

    async def _read_loop(self):
        loop = asyncio.get_running_loop()
        buffer = b""

        while self.is_running:
            try:
                # Read incoming serial bytes. Since pyserial serial.read is blocking,
                # we run it in the executor to avoid blocking the main event loop thread.
                data = await loop.run_in_executor(None, self.serial.read, 1024)
                if data:
                    buffer += data
                    while len(buffer) >= 2:
                        # Extract length (big-endian 2 bytes)
                        length = int.from_bytes(buffer[:2], byteorder='big')
                        if len(buffer) >= 2 + length:
                            payload = buffer[2:2+length]
                            buffer = buffer[2+length:]
                            if self.rx_callback:
                                try:
                                    self.rx_callback(payload)
                                except Exception as e:
                                    logger.error(f"Error in Serial RX callback: {e}")
                        else:
                            break
                else:
                    # No data, yield back control
                    await asyncio.sleep(0.05)
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in serial read loop: {e}")
                await asyncio.sleep(1)
