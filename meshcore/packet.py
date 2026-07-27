import json
from typing import Optional, Dict, Any

class MeshCorePacket:
    """
    Standardized payload formatting for MeshCore spatial and regional packet routing.
    Encapsulates channel, sender, scope, regional constraints, and application payloads.
    """
    def __init__(self, channel: str, sender: str, scope: str, type: str, data: Dict[str, Any], region: Optional[str] = None):
        self.channel = channel
        self.sender = sender
        self.scope = scope  # 'limited', 'regional', 'global'
        self.type = type    # 'advertise', 'message', 'command'
        self.data = data
        self.region = region

    def to_json(self) -> str:
        payload = {
            "channel": self.channel,
            "sender": self.sender,
            "scope": self.scope,
            "type": self.type,
            "data": self.data
        }
        if self.region:
            payload["region"] = self.region
        return json.dumps(payload)

    @classmethod
    def from_json(cls, json_str: str) -> 'MeshCorePacket':
        data_dict = json.loads(json_str)
        return cls(
            channel=data_dict.get("channel", ""),
            sender=data_dict.get("sender", ""),
            scope=data_dict.get("scope", "limited"),
            type=data_dict.get("type", ""),
            data=data_dict.get("data", {}),
            region=data_dict.get("region")
        )

    def is_valid_for_node(self, node_lat: float, node_lon: float, node_regions: list) -> bool:
        """
        Validates whether this packet is in-scope for a node based on geographical/regional rules.
        """
        if self.scope == "global":
            return True

        if self.scope == "regional" and self.region:
            # Check if node belongs to the target region
            return self.region in [r.name for r in node_regions]

        # For 'limited' or default, always process at application layer but limit hop repeats
        return True

    def __repr__(self):
        return f"<MeshCorePacket channel={self.channel} type={self.type} scope={self.scope} region={self.region}>"
