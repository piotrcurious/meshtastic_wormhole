import asyncio
import pytest
from mwb.lora.serial_interface import SerialLoRaInterface

class DummySerial:
    def __init__(self):
        self.written_data = b""
        self.read_buffer = b""
        self.is_open = True

    def write(self, data):
        self.written_data += data
        return len(data)

    def flush(self):
        pass

    def read(self, size):
        chunk = self.read_buffer[:size]
        self.read_buffer = self.read_buffer[size:]
        return chunk

    def close(self):
        self.is_open = False

def test_slip_encoding_decoding():
    raw = b"hello\xc0world\xdbescape"
    encoded = SerialLoRaInterface.encode_slip(raw)

    # Assert starts and ends with SLIP_END (0xC0)
    assert encoded[0] == 0xC0
    assert encoded[-1] == 0xC0

    # Decode step by step
    interface = SerialLoRaInterface(port="DUMMY")
    frames = interface.decode_slip_step(encoded)
    assert len(frames) == 1
    assert frames[0] == raw

def test_slip_boundary_splits():
    """Verify SLIP decoding works when the escape byte and target are split across chunks."""
    raw = b"boundary_test"
    encoded = SerialLoRaInterface.encode_slip(raw)

    # Intentionally split right in the middle or escape sequence if there was any.
    # In 'boundary_test', let's manually construct a split that cuts an escape sequence:
    # 0xC0 [data] 0xDB | 0xDC [data] 0xC0
    chunk1 = bytes([0xC0, 0x41, 0xDB])
    chunk2 = bytes([0xDC, 0x42, 0xC0])

    interface = SerialLoRaInterface(port="DUMMY")

    # Decode first chunk
    frames1 = interface.decode_slip_step(chunk1)
    assert len(frames1) == 0
    assert interface._in_escape is True

    # Decode second chunk
    frames2 = interface.decode_slip_step(chunk2)
    assert len(frames2) == 1
    # 0xDB + 0xDC should decode to 0xC0
    assert frames2[0] == bytes([0x41, 0xC0, 0x42])

@pytest.mark.asyncio
async def test_serial_interface_with_dummy():
    dummy = DummySerial()
    interface = SerialLoRaInterface(port="DUMMY", serial_obj=dummy)

    received_frames = []
    interface.register_rx_callback(lambda f: received_frames.append(f))

    await interface.start()

    # Test transmit
    test_msg = b"Meshtastic data"
    success = await interface.transmit(test_msg)
    assert success is True
    assert len(dummy.written_data) > 0
    assert SerialLoRaInterface.encode_slip(test_msg) in dummy.written_data

    # Test receive via read buffer
    dummy.read_buffer = SerialLoRaInterface.encode_slip(b"incoming raw serial slip")

    # Let asyncio process read loop cycle
    await asyncio.sleep(0.1)

    assert len(received_frames) == 1
    assert received_frames[0] == b"incoming raw serial slip"

    await interface.stop()
