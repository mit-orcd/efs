#!/usr/bin/env python3
"""Discover Linux FUSE clients and prove which process controls a mount.
Retirement requires successful control-channel drain by client.sh first.

Ownership never depends on another user's process: the mount table names
the target mount and its owning uid (`user_id=`), and the controller is
the one process the caller can inspect that holds both a FUSE device
descriptor and the mount's control endpoint (the listening socket that
client.sh drains). Linux does not map a /dev/fuse descriptor to its
connection, so the endpoint, which efs-fuse binds after mounting and closes
before unmounting, is the binding between mount and process. Foreign
processes are only filtered by their world-readable status and comm;
their argv, cwd and descriptors are never read.
"""
import os
from pathlib import Path
import signal
import stat
import sys
import time

# Seconds a client without endpoint ownership and without a stop record may
# take to leave on its own before refusal.
NATURAL_RETIRE_S = 10
# A retiring client reports its stop phases in its stop record (A11 finding
# 1): the wait continues while (phase, step) advances. STALL_S without an
# advance, or DEADLINE_S in all, is a failed stop; nothing is force-killed.
STALL_S = 30
DEADLINE_S = 600
FINAL_PHASES = ('done', 'failed')
FSTYPE = 'fuse.efs-fuse'


def canonical(mount, cwd=None):
    """Absolute lexical form, as efs_stop_path() computes it in efs-fuse."""
    if not mount.startswith('/'):
        mount = (cwd or os.getcwd()) + '/' + mount
    parts = []
    for part in mount.split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return '/' + '/'.join(parts)


def endpoint(mount, tmp='/tmp'):
    value = 14695981039346656037
    for byte in os.fsencode(canonical(mount)):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64)-1)
    return f'{tmp}/efs-control-{value:016x}.sock'


def stop_record_path(mount, tmp='/tmp'):
    """Stop record efs-fuse keeps next to its control endpoint."""
    return endpoint(mount, tmp)[:-len('.sock')] + '.stop'


def stop_record(mount, pid, uid, tmp='/tmp'):
    """{phase, step, total, rc, reason} from the stop record of PID, or None.
    The record must be a regular file owned by uid (any owner when uid is
    None) and name this PID; anything else is not this client's record."""
    path = stop_record_path(mount, tmp)
    try:
        st = os.lstat(path)
        if not stat.S_ISREG(st.st_mode) or (uid is not None and st.st_uid != uid):
            return None
        with open(path, encoding='utf-8', errors='replace') as f:
            line = f.readline().rstrip('\n')
    except FileNotFoundError:
        return None
    head, _, reason = line.partition(' reason=')
    fields = dict(item.partition('=')[::2] for item in head.split())
    if fields.get('pid') != str(pid) or 'phase' not in fields:
        return None
    try:
        return {'phase': fields['phase'], 'step': int(fields.get('step', 0)),
                'total': int(fields.get('total', 0)), 'rc': int(fields.get('rc', 0)),
                'reason': reason}
    except ValueError:
        return None


def describe(record):
    if not record:
        return 'no stop record'
    text = f"phase {record['phase']}"
    if record['step'] or record['total']:
        text += f" step {record['step']}" + (f"/{record['total']}" if record['total'] else '')
    return text


def await_exit(mount, pid, stamp, proc=Path('/proc'), tmp='/tmp', stall_s=None,
               deadline_s=None):
    """Wait for a retiring client to leave while its stop record shows
    progress; then require its final record to say done. Raises RuntimeError
    with the reason otherwise. Never signals."""
    stall_s = STALL_S if stall_s is None else stall_s
    deadline_s = DEADLINE_S if deadline_s is None else deadline_s
    try:
        uid = euid(proc, pid) if alive(pid, stamp, proc) else None
    except (FileNotFoundError, ProcessLookupError):
        uid = None
    if uid is None and os.getuid() != 0:
        uid = os.getuid()  # only own clients are ever retired by non-root
    start = last_change = time.monotonic()
    last = seen = None
    while alive(pid, stamp, proc):
        record = stop_record(mount, pid, uid, tmp)
        key = (record['phase'], record['step']) if record else None
        now = time.monotonic()
        if key != last:
            last, last_change = key, now
        seen = record or seen
        if not record and now - last_change >= NATURAL_RETIRE_S:
            raise RuntimeError(f'PID {pid} keeps no stop record for {mount}; retained, '
                               f'no forced kill attempted')
        if record and now - last_change >= stall_s:
            raise RuntimeError(f'client stop stalled for {mount}: no progress for {stall_s:g} s '
                               f'in {describe(record)}; client remains alive, no forced kill attempted')
        if now - start >= deadline_s:
            raise RuntimeError(f'client stop for {mount} not finished after {deadline_s:g} s '
                               f'({describe(record)}); client remains alive, no forced kill attempted')
        time.sleep(.1)
    record = stop_record(mount, pid, uid, tmp)
    if not record or record['phase'] not in FINAL_PHASES:
        raise RuntimeError(f'efs-fuse PID {pid} for {mount} exited without a final stop record '
                           f'(last seen: {describe(record or seen)})')
    if record['phase'] != 'done':
        raise RuntimeError(f'client stop for {mount} failed: {record["reason"]} (rc={record["rc"]})')
    return record


def unescape(raw):
    for code, char in [('040', ' '), ('011', '\t'), ('012', '\n'), ('134', '\\')]:
        raw = raw.replace('\\'+code, char)
    return raw


def mounts(proc=Path('/proc')):
    """(mount point, device, owning uid) of every efs-fuse mount."""
    result = []
    for line in (proc/'self/mountinfo').read_text().splitlines():
        left, right = line.split(' - ', 1)
        fields, tail = left.split(), right.split()
        if tail[0] != FSTYPE:
            continue
        options = dict(o.partition('=')[::2] for o in tail[2].split(',')) if len(tail) > 2 else {}
        path = unescape(fields[4])
        if '\n' in path or '\r' in path:
            raise RuntimeError('unsupported mount pathname; discovery refused')
        if not options.get('user_id', '').isdigit():
            raise RuntimeError(f'efs-fuse mount {path} has no user_id; ownership unknown')
        result.append((path, fields[2], int(options['user_id'])))
    return result


def stat_fields(proc, pid):
    return (proc/str(pid)/'stat').read_text().rsplit(')', 1)[1].split()


def alive(pid, stamp, proc=Path('/proc')):
    """starttime distinguishes PID reuse."""
    try:
        fields = stat_fields(proc, pid)
    except (FileNotFoundError, ProcessLookupError):
        return False
    return fields[0] not in ('Z', 'X') and fields[19] == stamp


def euid(proc, pid):
    for line in (proc/str(pid)/'status').read_text().splitlines():
        if line.startswith('Uid:'):
            return int(line.split()[2])
    raise RuntimeError(f'no Uid in status of PID {pid}')


def owned_processes(proc, uid):
    """PIDs this caller may inspect: its own (or every one, for root)."""
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if uid == 0 or euid(proc, entry.name) == uid:
                yield int(entry.name)
        except (FileNotFoundError, ProcessLookupError):
            continue


def descriptors(proc, pid):
    links = set()
    for candidate in (proc/str(pid)/'fd').iterdir():
        try:
            links.add(os.readlink(candidate))
        except FileNotFoundError:
            pass  # descriptor closed during exit
    return links


def listening(proc, path):
    """Socket inodes bound to path (unlinked rebinding can leave several)."""
    inodes = set()
    for line in (proc/'net/unix').read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 8 and fields[7] == path:
            inodes.add(fields[6])
    return inodes


def socket_owner(path):
    try:
        return os.lstat(path).st_uid
    except FileNotFoundError:
        return None


def controllers(proc, uid, tmp):
    """{endpoint path: [(pid, stamp, links)]} for inspectable efs-fuse holders."""
    paths = {}
    for line in (proc/'net/unix').read_text().splitlines()[1:]:
        fields = line.split()
        if (len(fields) >= 8 and fields[7].startswith(f'{tmp}/efs-control-')
                and fields[7].endswith('.sock')):
            paths.setdefault(fields[7], set()).add(f'socket:[{fields[6]}]')
    result = {}
    for pid in owned_processes(proc, uid):
        try:
            if (proc/str(pid)/'comm').read_text().strip() != 'efs-fuse':
                continue
            fields = stat_fields(proc, pid)
            links = descriptors(proc, pid)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        for path, sockets in paths.items():
            if links & sockets:
                result.setdefault(path, []).append((pid, fields[19], links))
    return result


def controller(mount, proc=Path('/proc'), tmp='/tmp', uid=None):
    """(pid, stamp) controlling mount, None when no process controls it.
    Raises RuntimeError when ownership is foreign or ambiguous."""
    uid = os.getuid() if uid is None else uid
    mount = canonical(mount)
    listed = [row for row in mounts(proc) if row[0] == mount]
    if len(listed) > 1:
        raise RuntimeError(f'{len(listed)} efs-fuse mounts stacked on {mount}; ownership ambiguous')
    # Root may control any uid's detached client; the endpoint names its owner.
    owner = listed[0][2] if listed else (None if uid == 0 else uid)
    if uid != 0 and owner != uid:
        raise RuntimeError(f'{mount} (device {listed[0][1]}) is mounted by uid {owner}, '
                           f'not uid {uid}; refusing')
    path = endpoint(mount, tmp)
    inodes = listening(proc, path)
    if not inodes:
        return None  # no control endpoint: stale mount or no client
    holder = socket_owner(path)
    if holder is not None and owner is not None and holder != owner:
        raise RuntimeError(f'control endpoint {path} for {mount} belongs to uid {holder}, '
                           f'not uid {owner}; refusing')
    sockets = {f'socket:[{inode}]' for inode in inodes}
    found = []
    for pid in owned_processes(proc, uid):
        try:
            links = descriptors(proc, pid)
            if links & sockets:
                found.append((pid, stat_fields(proc, pid), links))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            continue  # never establishes ownership; refused below if nothing else does
    if not found:
        if not listening(proc, path) & inodes:
            return None  # the controller closed its endpoint during the scan
        raise RuntimeError(f'control endpoint {path} for {mount} is held by no process '
                           f'uid {uid} can inspect; refusing')
    if len(found) > 1:
        pids = ' '.join(str(row[0]) for row in found)
        raise RuntimeError(f'multiple processes ({pids}) hold the control endpoint of {mount}; '
                           f'ownership ambiguous')
    pid, fields, links = found[0]
    try:
        comm = (proc/str(pid)/'comm').read_text().strip()
    except (FileNotFoundError, ProcessLookupError):
        return None
    if comm != 'efs-fuse' or '/dev/fuse' not in links:
        raise RuntimeError(f'PID {pid} ({comm}) holds the control endpoint of {mount} '
                           f'without a FUSE connection; refusing')
    return pid, fields[19]


def name(proc, pid, path):
    """Name a detached own client's mount: one argv entry whose endpoint is path.
    Only ever called for processes this caller may inspect."""
    args = (proc/str(pid)/'cmdline').read_bytes().split(b'\0')
    cwd = os.readlink(proc/str(pid)/'cwd')
    tmp = os.path.dirname(path)
    names = {canonical(os.fsdecode(a), cwd) for a in args[1:] if a and not a.startswith(b'-')}
    names = {n for n in names if endpoint(n, tmp) == path}
    if len(names) != 1:
        raise RuntimeError(f'cannot name the mount of detached efs-fuse PID {pid}')
    return names.pop()


def clients(proc=Path('/proc'), tmp='/tmp', uid=None):
    """(pid, mount, stamp) of every client this caller controls, mounted or
    detached. Mounts of other uids and their processes are ignored."""
    uid = os.getuid() if uid is None else uid
    result, named = [], set()
    for mount, _device, owner in mounts(proc):
        if uid != 0 and owner != uid:
            continue
        found = controller(mount, proc, tmp, uid)
        named.add(endpoint(mount, tmp))
        if found:
            result.append((found[0], mount, found[1]))
    for path, holders in controllers(proc, uid, tmp).items():
        if path in named:
            continue
        for pid, stamp, _links in holders:
            try:
                mount = name(proc, pid, path)
            except (FileNotFoundError, ProcessLookupError):
                continue
            found = controller(mount, proc, tmp, uid)
            if found:
                result.append((found[0], mount, found[1]))
    return sorted(set(result))


def retire(mount, pid, stamp, proc=Path('/proc'), tmp='/tmp', stall_s=None, deadline_s=None):
    """Terminate the drained controller identified by `has` before unmount,
    then wait for its stop to finish (await_exit)."""
    mount = canonical(mount)
    if not alive(pid, stamp, proc):
        return await_exit(mount, pid, stamp, proc, tmp, stall_s, deadline_s)
    current = controller(mount, proc, tmp)
    if current and current != (pid, stamp):
        raise RuntimeError(f'control endpoint of {mount} is now held by PID {current[0]}, '
                           f'not drained PID {pid}; retained')
    if not current:
        # A naturally exiting client closes its control socket before it
        # flushes, settles and revokes. Never signal without endpoint
        # ownership; follow its stop record instead.
        return await_exit(mount, pid, stamp, proc, tmp, stall_s, deadline_s)
    print(f'Retiring drained efs-fuse PID {pid} for {mount}', flush=True)
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('safe retirement requires Linux pidfd support')
    try:
        fd = os.pidfd_open(pid)
    except ProcessLookupError:
        return await_exit(mount, pid, stamp, proc, tmp, stall_s, deadline_s)
    try:
        if alive(pid, stamp, proc):
            try:
                signal.pidfd_send_signal(fd, signal.SIGTERM)
            except ProcessLookupError:
                pass
    finally:
        os.close(fd)
    return await_exit(mount, pid, stamp, proc, tmp, stall_s, deadline_s)


def seconds(args, flag):
    if flag not in args:
        return None
    i = args.index(flag)
    value = args[i+1] if i+1 < len(args) else ''
    del args[i:i+2]
    try:
        return float(value)
    except ValueError:
        raise RuntimeError(f'{flag} needs seconds, not {value!r}')


USAGE = ('usage: client_processes.py list | has MOUNT | '
         'retire MOUNT PID STARTTIME [--stall S] [--deadline S]')


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ''
    if command == 'list':
        # Own mounts, including stale ones whose daemon has died, and own
        # detached clients.
        uid = os.getuid()
        found = {row[0] for row in mounts() if uid == 0 or row[2] == uid}
        found |= {row[1] for row in clients()}
        for mount in sorted(found):
            if '\n' in mount or '\r' in mount:
                raise RuntimeError('unsupported mount pathname; discovery refused')
            print(mount)
    elif command == 'has' and len(sys.argv) == 3:
        found = controller(sys.argv[2])
        if not found:
            return 1
        print(f'{found[0]} {found[1]}')
    elif command == 'retire':
        args = sys.argv[2:]
        stall, deadline = seconds(args, '--stall'), seconds(args, '--deadline')
        if len(args) != 3 or not args[1].isdigit():
            raise RuntimeError(USAGE)
        record = retire(args[0], int(args[1]), args[2], stall_s=stall, deadline_s=deadline)
        print(f'Client stop finished for {canonical(args[0])}: {record["reason"]}'
              if record['reason'] != '-' else f'Client stop finished for {canonical(args[0])}',
              flush=True)
    else:
        raise RuntimeError(USAGE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(2)
