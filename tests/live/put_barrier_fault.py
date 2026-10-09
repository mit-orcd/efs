"""Actual server fragment-barrier failure must not acknowledge publication."""
import errno
import os

def exercise(mount, marker, observe):
    path = mount / 'w55-failed-put-barrier'
    payload = bytes(range(256)) * 11 + b'W55'
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    ino = os.fstat(fd).st_ino
    marker.write_text('fail fragment durability\n')
    failure = None
    try:
        assert os.write(fd, payload) == len(payload)
        try:
            os.fsync(fd)
        except OSError as error:
            failure = error.errno
        assert failure in (errno.EIO, errno.EBUSY, errno.EAGAIN), failure
        state = observe(ino)
        assert state['base_gen'] == 0 and state['live_spans'] == 0, state
    finally:
        marker.unlink(missing_ok=True)
        try:
            os.fsync(fd)  # recover the retained body before ordinary close
        finally:
            os.close(fd)
    assert path.read_bytes() == payload
    return {'ino': ino, 'bytes': len(payload), 'refused_fsync_errno': failure,
            'unpublished_state': state, 'recovered_exact_bytes': True}

def verify_and_delete(mount):
    path = mount / 'w55-failed-put-barrier'
    assert path.read_bytes() == bytes(range(256)) * 11 + b'W55'
    path.unlink()
