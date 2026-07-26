import json
import os
import uuid

DEFAULT_CONFIG = {
    "name": "default-wormhole",
    "id": "",  # will generate default based on uuid
    "wifi": {
        "mode": "multicast",
        "udp_port": 4403,
        "multicast_group": "239.10.10.10",
        "peers": []
    },
    "lora": {
        "mode": "mock",  # 'mock' or 'serial'
        "serial_port": "/dev/ttyUSB0",
        "baudrate": 115200,
        "region": "US915"
    },
    "security": {
        "wireguard": False
    },
    "diagnostics": {
        "port": 8080,
        "host": "0.0.0.0"
    }
}

class Config:
    def __init__(self, filepath=None):
        self.filepath = filepath
        self.data = DEFAULT_CONFIG.copy()

        if filepath and os.path.exists(filepath):
            self.load(filepath)
        else:
            # Generate a default unique ID based on MAC-like logic or UUID
            if not self.data["id"]:
                node_id = uuid.getnode()
                # format as 12-char hex string
                self.data["id"] = f"{node_id:012x}"
                # Or limit to standard 8-byte hex or similar if preferred
            if not self.data["name"]:
                self.data["name"] = f"wormhole-{self.data['id'][-4:]}"

    def load(self, filepath):
        with open(filepath, "r") as f:
            loaded_data = json.load(f)
            self._update_recursive(self.data, loaded_data)

        # Ensure ID exists
        if not self.data.get("id"):
            node_id = uuid.getnode()
            self.data["id"] = f"{node_id:012x}"

    def _update_recursive(self, d, u):
        for k, v in u.items():
            if isinstance(v, dict):
                d[k] = self._update_recursive(d.get(k, {}), v)
            else:
                d[k] = v
        return d

    def save(self, filepath=None):
        target = filepath or self.filepath
        if not target:
            raise ValueError("No filepath specified to save configuration.")
        with open(target, "w") as f:
            json.dump(self.data, f, indent=4)

    @property
    def name(self) -> str:
        return self.data["name"]

    @property
    def id(self) -> str:
        return self.data["id"]

    @property
    def int_id(self) -> int:
        """Return the ID as an integer for packet packing."""
        try:
            return int(self.data["id"], 16)
        except ValueError:
            # Fallback hash of string if it's not a hex string
            return hash(self.data["id"]) & 0xFFFFFFFFFFFFFFFF

    @property
    def wifi_mode(self) -> str:
        return self.data["wifi"]["mode"]

    @property
    def udp_port(self) -> int:
        return self.data["wifi"]["udp_port"]

    @property
    def multicast_group(self) -> str:
        return self.data["wifi"]["multicast_group"]

    @property
    def peers(self) -> list:
        return self.data["wifi"]["peers"]

    @property
    def lora_mode(self) -> str:
        return self.data["lora"]["mode"]

    @property
    def lora_serial_port(self) -> str:
        return self.data["lora"]["serial_port"]

    @property
    def lora_baudrate(self) -> int:
        return self.data["lora"]["baudrate"]

    @property
    def lora_region(self) -> str:
        return self.data["lora"]["region"]

    @property
    def diagnostics_host(self) -> str:
        return self.data["diagnostics"]["host"]

    @property
    def diagnostics_port(self) -> int:
        return self.data["diagnostics"]["port"]
