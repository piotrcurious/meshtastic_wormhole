import asyncio
import logging
import socket
import struct
from typing import Callable, Optional, List

logger = logging.getLogger("mwb.wifi.udp")

class UDPTransportProtocol(asyncio.DatagramProtocol):
    def __init__(self, on_packet_received: Callable[[bytes, tuple], None]):
        self.on_packet_received = on_packet_received
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        try:
            self.on_packet_received(data, addr)
        except Exception as e:
            logger.exception(f"Error handling received UDP packet: {e}")

class UDPTransport:
    """
    Manages IP-level packet communication over UDP.
    Supports either UDP multicast (decentralized mesh) or Unicast Peer-to-Peer mode.
    """
    def __init__(self, mode: str, port: int, multicast_group: str = "239.10.10.10", peers: List[str] = None):
        self.mode = mode  # 'multicast' or 'unicast'
        self.port = port
        self.multicast_group = multicast_group
        self.peers = peers or []

        self.transport = None
        self.protocol = None
        self.rx_callback: Optional[Callable[[bytes, tuple], None]] = None
        self._sock = None

    def register_rx_callback(self, callback: Callable[[bytes, tuple], None]):
        self.rx_callback = callback

    async def start(self):
        loop = asyncio.get_running_loop()

        # Create socket depending on configuration mode
        if self.mode == "multicast":
            # For multicast, configure socket to join the multicast group
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            # Bind to wildcard address and target port
            sock.bind(("", self.port))

            # Join the multicast group
            mreq = struct.pack("4s4s", socket.inet_aton(self.multicast_group), socket.inet_aton("0.0.0.0"))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

            # Enable multicast loopback so nodes on same machine can communicate if needed
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            # Set TTL (time-to-live) for multicast packets
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)

            self._sock = sock
            self.transport, self.protocol = await loop.create_datagram_endpoint(
                lambda: UDPTransportProtocol(self._on_packet),
                sock=self._sock
            )
            logger.info(f"UDP Transport started in MULTICAST mode on port {self.port}, group {self.multicast_group}")
        else:
            # Unicast mode
            self.transport, self.protocol = await loop.create_datagram_endpoint(
                lambda: UDPTransportProtocol(self._on_packet),
                local_addr=("0.0.0.0", self.port)
            )
            logger.info(f"UDP Transport started in UNICAST mode on port {self.port}. Configured peers: {self.peers}")

    async def stop(self):
        if self.transport:
            self.transport.close()
            self.transport = None
        if self._sock:
            self._sock.close()
            self._sock = None
        logger.info("UDP Transport stopped.")

    def _on_packet(self, data: bytes, addr: tuple):
        if self.rx_callback:
            self.rx_callback(data, addr)

    def transmit(self, payload: bytes):
        """Send the packet to all configured targets (multicast group or unicast peers)."""
        if not self.transport:
            logger.warning("UDP Transport is not started. Transmit ignored.")
            return

        if self.mode == "multicast":
            # Transmit to multicast group
            try:
                self.transport.sendto(payload, (self.multicast_group, self.port))
            except Exception as e:
                logger.error(f"Failed to transmit UDP multicast: {e}")
        else:
            # Transmit to each configured unicast peer
            for peer in self.peers:
                try:
                    # peer can be host:port or just host
                    if ":" in peer:
                        host, port_str = peer.rsplit(":", 1)
                        port = int(port_str)
                    else:
                        host, port = peer, self.port

                    self.transport.sendto(payload, (host, port))
                except Exception as e:
                    logger.error(f"Failed to transmit UDP unicast to peer {peer}: {e}")
