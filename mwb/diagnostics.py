import asyncio
import json
import logging
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
from mwb.router import PacketRouter

logger = logging.getLogger("mwb.diagnostics")

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Handle requests in a separate thread."""
    allow_reuse_address = True

class DiagnosticsHTTPRequestHandler(BaseHTTPRequestHandler):
    router: PacketRouter = None

    def log_message(self, format, *args):
        # Override to suppress standard logging to stderr
        logger.debug(format % args)

    def do_GET(self):
        if self.path == "/status" or self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            status_data = self.router.get_status()
            self.wfile.write(json.dumps(status_data, indent=4).encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not Found")

class DiagnosticsServer:
    """
    HTTP Diagnostics Server. Runs on a background thread/task to avoid blocking
    the asyncio event loop.
    """
    def __init__(self, router: PacketRouter):
        self.router = router
        self.host = router.config.diagnostics_host
        self.port = router.config.diagnostics_port
        self.server = None
        self._thread = None

    async def start(self):
        # Setup handler class with router reference
        handler_class = type(
            'CustomDiagnosticsHTTPRequestHandler',
            (DiagnosticsHTTPRequestHandler,),
            {'router': self.router}
        )

        loop = asyncio.get_running_loop()
        try:
            self.server = ThreadedHTTPServer((self.host, self.port), handler_class)
            logger.info(f"Diagnostics HTTP server starting on http://{self.host}:{self.port}")
            # Run server in asyncio thread executor to prevent blocking
            loop.run_in_executor(None, self.server.serve_forever)
        except Exception as e:
            logger.error(f"Failed to start Diagnostics server on port {self.port}: {e}")

    async def stop(self):
        if self.server:
            loop = asyncio.get_running_loop()
            await loop.run_in_executor(None, self.server.shutdown)
            await loop.run_in_executor(None, self.server.server_close)
            self.server = None
            logger.info("Diagnostics HTTP server stopped.")
