"""Actual server fragment barriers: insufficient ACKs refuse publication."""
import errno
import os

PAYLOAD = bytes(range(256)) * 11 + b'W55'
CASES = ('all-files', 'partial-files', 'all-directories', 'quorum-files')

def exercise(mount, marker, observe):
    results = []
    for name, selection, succeeds in (
        ('all-files', 'all file', False),
        ('partial-files', ',1, ,2, file', False),
        ('all-directories', 'all directory', False),
        ('quorum-files', ',1, file', True),
    ):
        path = mount / ('w55-barrier-' + name)
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        ino = os.fstat(fd).st_ino
        marker.write_text(selection + '\n')
        failure = None
        try:
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
                assert state['base_gen'] == 0 and state['live_spans'] == 0, state
        finally:
            # Keep the one-node fault through the cold read of quorum success.
            if not succeeds:
                marker.unlink(missing_ok=True)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        assert path.read_bytes() == PAYLOAD
        results.append({'case': name, 'selection': selection, 'ino': ino,
                        'bytes': len(PAYLOAD), 'fsync_errno': failure,
                        'state': state, 'recovered_exact_bytes': True})
    return results

def verify_and_delete(mount):
    for name in CASES:
        path = mount / ('w55-barrier-' + name)
        assert path.read_bytes() == PAYLOAD
        path.unlink()
