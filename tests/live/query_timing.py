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

def wait_query_nodes_indexed(root_ino, timeout):
    """Warm each catalog that can serve a routed request, not just one helper.

    NODATA is allowed only during this bounded fresh-fixture precondition.
    Concurrent-reader assertions remain unchanged after it completes.
    """
    import os
    from query_rpc import query, page_request
    configured = os.environ.get('EFS_QUERY_TEST_NODES')
    nodes = json.loads(configured) if configured else {
        str(i+1): [os.environ.get('EFS_QUERY_TEST_HOST', '127.0.0.1'),
                   int(os.environ['EFS_QUERY_TEST_PORT'])+i] for i in range(4)}
    deadline = time.monotonic()+timeout
    while True:
        serving = {}
        redirects = {}
        for node, (host, port) in nodes.items():
            remaining = deadline-time.monotonic()
            assert remaining > 0, ('query catalogs never warmed', root_ino, serving)
            fields = dict(operation=1, root=root_ino, uid=os.getuid(), gid=os.getgid(),
                          match=3, max_output=4<<20)
            reply, text = query(port, page_request(**fields, limit=10000),
                host=host, timeout=min(7, remaining))
            if reply.status == 0:
                page = json.loads(text)['page']  # malformed success is not a warm catalog
                if int(page['token']):  # release this node's page session
                    query(port, page_request(**fields, token=int(page['token']),
                          cursor=int(page['cursor']), limit=0), host=host, timeout=7)
                serving[node] = dict(boot_id=reply.boot_id, generation=reply.generation)
            else:
                assert not text and reply.status in (-13, -15, -21), ('warm query refused', node, reply.status)
                if reply.status == -15:
                    owner = str(reply.primary_id)
                    assert owner in nodes and owner != node, ('invalid warm redirect', node, owner)
                    redirects[node] = owner
        # A redirect is sufficient only if its target was queried successfully
        # in this round. Every directly serving node must have the fixture.
        if serving and len(serving)+len(redirects)==len(nodes) and all(owner in serving for owner in redirects.values()):
            return serving
        assert time.monotonic() < deadline, ('query catalogs never warmed', root_ino, serving)
        time.sleep(min(1, max(0, deadline-time.monotonic())))
