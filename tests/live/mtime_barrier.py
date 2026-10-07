#!/usr/bin/env python3
"""Exercise real FUSE SETATTR across sparse lanes; remove only this fixture."""
import argparse
import os
import re
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mount')
p.add_argument('mgmt')
p.add_argument('address')
p.add_argument('--chunk-size', type=int, default=128 * 1024)
a = p.parse_args()
fd, path = tempfile.mkstemp(prefix='.mtime-barrier-', dir=a.mount)
try:
    # Adjacent and distant chunk indices exercise multiple publication lanes.
    for ci in (0, 1, 17, 63):
        assert os.pwrite(fd, bytes([ci + 1]) * 4096, ci * a.chunk_size) == 4096
    os.fsync(fd)
    ino = os.fstat(fd).st_ino
    expected_size = 63 * a.chunk_size + 4096
    def server_stat():
        for _ in range(40):
            out = subprocess.check_output([a.mgmt, 'raft-getattr', a.address, str(ino)], text=True)
            values = {k: int(v) for k, v in re.findall(r'(status|primary|size|mtime)=(\d+)', out)}
            if values.get('status') == 0:
                return values
            # Follow explicit primary hints via the NUC's known four ports.
            # This harness is NUC-specific; production routing is independent.
            if values.get('primary'):
                a.address = '127.0.0.1:' + str(17431 + values['primary'])
            time.sleep(0.05)
        raise AssertionError(out)
    for value in (100, 50, 150, 25):
        os.utime(fd, ns=(value * 10**9, value * 10**9))
        st = server_stat()
        assert st['mtime'] == value and st['size'] == expected_size, st
        for ci in (0, 1, 17, 63):
            assert os.pread(fd, 4096, ci * a.chunk_size) == bytes([ci + 1]) * 4096
        print('server mtime=%d size=%d data preserved' % (value, st['size']), flush=True)
    assert os.pwrite(fd, b'after-fence', 17 * a.chunk_size) == 11
    os.fsync(fd)
    st = server_stat()
    assert st['mtime'] > 150 and st['size'] == expected_size, st
    assert os.pread(fd, 11, 17 * a.chunk_size) == b'after-fence'
    print('live multi-lane mtime barrier PASS', flush=True)
finally:
    os.close(fd)
    os.unlink(path)
