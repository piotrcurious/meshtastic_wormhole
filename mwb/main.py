import argparse
import asyncio
import logging
import sys
import os

from mwb.config import Config
from mwb.lora.mock_interface import MockLoRaInterface
from mwb.lora.serial_interface import SerialLoRaInterface
from mwb.wifi.udp_transport import UDPTransport
from mwb.router import PacketRouter
from mwb.diagnostics import DiagnosticsServer

def setup_logging(verbose=False):
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        handlers=[
            logging.StreamHandler(sys.stdout)
        ]
    )

async def main_async():
    parser = argparse.ArgumentParser(description="Meshtastic Wormhole Bridge (MWB)")
    parser.add_argument("-c", "--config", type=str, help="Path to config JSON file")
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose debug logging")
    parser.add_argument("--mock-node-id", type=str, help="Override ID for mock testing (e.g. 001122334455)")
    parser.add_argument("--diagnostics-port", type=int, help="Override HTTP diagnostics server port")
    parser.add_argument("--udp-port", type=int, help="Override UDP transport port")
    parser.add_argument("--lora-mode", type=str, choices=["mock", "serial"], help="Override LoRa interface mode")

    args = parser.parse_args()
    setup_logging(args.verbose)

    logger = logging.getLogger("mwb.main")
    logger.info("Initializing Meshtastic Wormhole Bridge...")

    # Load configuration
    config_path = args.config if args.config else "config.json"
    cfg = Config(filepath=config_path if os.path.exists(config_path) else None)

    # Apply CLI Overrides
    if args.mock_node_id:
        cfg.data["id"] = args.mock_node_id
    if args.diagnostics_port:
        cfg.data["diagnostics"]["port"] = args.diagnostics_port
    if args.udp_port:
        cfg.data["wifi"]["udp_port"] = args.udp_port
    if args.lora_mode:
        cfg.data["lora"]["mode"] = args.lora_mode

    # Automatically save generated/resolved config if it doesn't exist
    if not os.path.exists(config_path) and not args.config:
        try:
            cfg.save(config_path)
            logger.info(f"Saved default configuration to {config_path}")
        except Exception as e:
            logger.warning(f"Could not save default config to {config_path}: {e}")

    # Build LoRa Interface
    if cfg.lora_mode == "serial":
        logger.info(f"Using physical Serial LoRa on {cfg.lora_serial_port}")
        lora = SerialLoRaInterface(port=cfg.lora_serial_port, baudrate=cfg.lora_baudrate)
    else:
        logger.info(f"Using Mock LoRa Interface with ID {cfg.id}")
        lora = MockLoRaInterface(node_id=cfg.int_id)

    # Build Wifi (UDP) Transport
    wifi = UDPTransport(
        mode=cfg.wifi_mode,
        port=cfg.udp_port,
        multicast_group=cfg.multicast_group,
        peers=cfg.peers
    )

    # Build Router and diagnostics
    router = PacketRouter(config=cfg, lora=lora, wifi=wifi)
    router.start()

    diagnostics = DiagnosticsServer(router=router)

    # Start all components
    await lora.start()
    await wifi.start()
    await diagnostics.start()

    logger.info(f"Wormhole node running: Name={cfg.name}, ID={cfg.id}")

    # Run indefinitely or until interrupted
    try:
        while True:
            await asyncio.sleep(3600)
    except (KeyboardInterrupt, asyncio.CancelledError):
        logger.info("Shutdown requested, stopping components...")
    finally:
        await diagnostics.stop()
        await wifi.stop()
        await lora.stop()
        logger.info("Shutdown complete.")

def main():
    try:
        asyncio.run(main_async())
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
