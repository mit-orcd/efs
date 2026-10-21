"""Bounded live-test timing matched to the server's advertised refresh policy."""
import json
import subprocess
import sys
import time

def refresh_seconds(mount):
    result = subprocess.run([sys.executable, str(mount / '.efs/stats'), '--json'],
                            capture_output=True, timeout=30, check=True)
    snapshot = json.loads(result.stdout)['snapshot']
    interval = max(snapshot['refresh_ms'], snapshot['refresh_min_ms']) / 1000
    assert 0 < interval <= 3600, snapshot
    return interval

def settle_seconds(interval):
    # Allow two refresh cuts plus a bounded build allowance. This is an
    # acceptance deadline, not permission to ignore permanent refresh errors.
    return max(30, 2 * interval + 60)

def wait_indexed(root, timeout):
    deadline = time.monotonic() + timeout
    while True:
        result = subprocess.run([sys.executable, str(root / '.efs/stats'), '--json'],
                                capture_output=True, timeout=30)
        if result.returncode == 0:
            return json.loads(result.stdout)
        # Only the documented not-indexed-yet response is transient here.
        assert result.returncode == 75, (result.returncode, result.stderr)
        if time.monotonic() >= deadline:
            raise AssertionError(('directory never indexed', str(root), result.stderr))
        time.sleep(1)
