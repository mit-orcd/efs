"""Actual server fragment barriers: fewer than two durable fragments is EIO, never a success ACK."""
import errno
import os

PAYLOAD = bytes(range(256)) * 11 + b'W55'
CASES = ('quorum-files', 'partial-files', 'all-files')

def exercise(mount, marker, observe):
    """The batched root barrier (syncfs) fails sticky until the daemon restarts, so the
    failing cases run in widening order on one cluster. The span is published before
    fsync proves it (K35): a failed proof is EIO with the bytes retained, not absence."""
    results = []
    for name, selection, succeeds in (
        ('quorum-files', ',1,', True),
        ('partial-files', ',1, ,2,', False),
        ('all-files', 'all', False),
    ):
        path = mount / ('w55-barrier-' + name)
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        ino = os.fstat(fd).st_ino
        marker.write_text(selection + '\n')
        failure = None
        assert os.write(fd, PAYLOAD) == len(PAYLOAD)
        try:
            os.fsync(fd)
        except OSError as error:
            failure = error.errno
        if succeeds:
            assert failure is None, failure
            state = observe(ino)
            assert state['base_gen'] > 0 or state['live_spans'] > 0, state
        else:
            assert failure in (errno.EIO, errno.EBUSY, errno.EAGAIN), failure
            state = observe(ino)
        assert path.read_bytes() == PAYLOAD
        os.close(fd)
        results.append({'case': name, 'selection': selection, 'ino': ino,
                        'bytes': len(PAYLOAD), 'fsync_errno': failure,
                        'state': state, 'recovered_exact_bytes': True})
    marker.unlink(missing_ok=True)
    return results

def verify_and_delete(mount):
    for name in CASES:
        path = mount / ('w55-barrier-' + name)
        assert path.read_bytes() == PAYLOAD
        path.unlink()
