import time
from mwb.duplicate import Deduplicator

def test_deduplicator():
    dedup = Deduplicator(ttl=1.0) # 1 second TTL for test

    # First time should not be duplicate
    assert dedup.is_duplicate(0x1234, 1) is False

    # Second time immediately after should be duplicate
    assert dedup.is_duplicate(0x1234, 1) is True

    # Different packet id should not be duplicate
    assert dedup.is_duplicate(0x1234, 2) is False

    # Different source id should not be duplicate
    assert dedup.is_duplicate(0x5678, 1) is False

    # Wait for TTL expiration
    time.sleep(1.1)

    # After TTL, it should not be duplicate again
    assert dedup.is_duplicate(0x1234, 1) is False
