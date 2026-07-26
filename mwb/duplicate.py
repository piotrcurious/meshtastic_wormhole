import time

class Deduplicator:
    """
    Tracks seen packet identifiers (source_id + packet_id) to prevent duplicate processing.
    """
    def __init__(self, ttl: float = 120.0):
        self.ttl = ttl
        # Key: (source_id, packet_id), Value: timestamp (float)
        self.cache = {}

    def is_duplicate(self, source_id: int, packet_id: int) -> bool:
        current_time = time.time()
        key = (source_id, packet_id)

        # Clean up expired entries on access to keep memory footprint bounded
        self.cleanup()

        if key in self.cache:
            # Check if expired just in case
            if current_time - self.cache[key] < self.ttl:
                return True
            else:
                # Expired, we can remove it
                del self.cache[key]

        # Not a duplicate, mark as seen
        self.cache[key] = current_time
        return False

    def cleanup(self):
        current_time = time.time()
        expired_keys = [
            key for key, timestamp in self.cache.items()
            if current_time - timestamp >= self.ttl
        ]
        for key in expired_keys:
            del self.cache[key]

    def clear(self):
        self.cache.clear()
