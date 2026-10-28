#!/usr/bin/env python3
"""A fixture precondition waits for references; persistent users fail closed."""
import tempfile
import threading
import time
from pathlib import Path
from mount_idle import mount_users, wait_fixture_unused

with tempfile.TemporaryDirectory(prefix='efs-mount-idle-') as tmp:
    root = Path(tmp).resolve()
    mount = root / 'mnt'
    mount.mkdir()
    proc = root / 'proc'
    p = proc / '999999999'
    (p / 'fd').mkdir(parents=True)
    (p / 'comm').write_text('desktop-monitor\n')
    ref = p / 'fd/3'
    ref.symlink_to(mount / 'file')
    assert mount_users(mount, proc) == ['desktop-monitor(999999999)']
    try:
        wait_fixture_unused(mount, timeout=0.05, proc=proc)
    except AssertionError as e:
        assert 'remains in use' in str(e)
    else:
        raise AssertionError('a persistent owner was ignored')
    assert ref.is_symlink(), 'the owner must never be changed'
    closer = threading.Timer(0.25, ref.unlink)
    closer.start()
    start = time.monotonic()
    wait_fixture_unused(mount, timeout=2, proc=proc)
    closer.join()
    assert time.monotonic() - start >= 0.25
    ref.symlink_to(root / 'mnt-other/file')
    assert mount_users(mount, proc) == [], 'path prefix is not mount ownership'
    ref.unlink()
    ref.symlink_to(mount)
    assert mount_users(mount, proc), 'mount root references count too'
    (p / 'comm').write_text('efs-fuse\n')
    assert mount_users(mount, proc) == [], 'controller follows production exclusion'
print('fixture idle: transient reference awaited, persistent reference refused, no user signalled PASS')
