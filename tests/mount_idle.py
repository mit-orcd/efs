"""Establish an unused synthetic mount before timing a clean stop.

Desktop volume monitors may briefly inspect a newly mounted filesystem.
Never stop those processes, ignore their references or retry a failed stop.
"""
import os
import time
from pathlib import Path


def mount_users(mount, proc=Path('/proc')):
    mount = os.path.realpath(mount)
    found = []
    for p in proc.iterdir():
        if not p.name.isdigit() or int(p.name) == os.getpid():
            continue
        try:
            comm = (p / 'comm').read_text().strip()
        except OSError:
            continue
        if comm == 'efs-fuse':
            continue
        paths = [p / 'cwd', p / 'root']
        try:
            paths += list((p / 'fd').iterdir())
        except OSError:
            pass
        for f in paths:
            try:
                target = os.readlink(f)
            except OSError:
                continue
            if target == mount or target.startswith(mount + '/'):
                found.append(f'{comm}({p.name})')
                break
    return found


def wait_fixture_unused(mount, timeout=5, proc=Path('/proc')):
    end = time.monotonic() + timeout
    quiet = 0
    observed = set()
    while True:
        users = mount_users(mount, proc)
        observed.update(users)
        quiet = 0 if users else quiet + 1
        if quiet == 2:
            if observed:
                print(f'fixture idle: transient users closed: {sorted(observed)}', flush=True)
            return
        assert time.monotonic() < end, f'fixture remains in use before stop: {users}'
        time.sleep(0.1)
