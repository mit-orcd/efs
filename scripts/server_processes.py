#!/usr/bin/env python3
"""Linux daemon ownership and graceful retirement; PID alone is not authority."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import sys
import time


def identity(pid):
    try:
        proc=Path('/proc')/str(pid)
        fields=(proc/'stat').read_text().rsplit(')',1)[1].split()
        if fields[0] in ('Z','X'):
            return None
        executable=os.readlink(proc/'exe').removesuffix(' (deleted)')
        args=[os.fsdecode(value) for value in (proc/'cmdline').read_bytes().split(b'\0') if value]
        return {'start':fields[19],'boot':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                'exe':executable,'args':args}
    except (FileNotFoundError,ProcessLookupError):
        return None


def owns(info,kind,value):
    if not info or Path(info['exe']).name != 'efsd':
        return False
    args=info['args']
    if kind=='port':
        return any(args[index]=='--port' and args[index+1]==value for index in range(len(args)-1))
    roots=[args[index+1].split(',')[0] for index in range(len(args)-1) if args[index]=='--storage']
    return bool(roots) and os.path.realpath(roots[0])==os.path.realpath(value)


def numeric(value):
    return value.isascii() and value.isdigit() and int(value)>1


def record(file,storage):
    value=file.read_text().strip()
    if not numeric(value):
        raise RuntimeError('invalid daemon PID record')
    end=time.monotonic()+2
    while time.monotonic()<end:
        info=identity(int(value))
        if owns(info,'storage',storage):
            tmp=Path(str(file)+f'.identity.{os.getpid()}')
            tmp.write_text(json.dumps(info))
            tmp.replace(str(file)+'.identity')
            return
        time.sleep(.05)
    raise RuntimeError('started PID is not the requested storage daemon')


def retire(pid,kind,value,expected=None,timeout=60):
    before=identity(pid)
    if not owns(before,kind,value) or (expected is not None and expected != before):
        print(f'Stale daemon identity for PID {pid}; process retained',flush=True)
        return
    if not hasattr(os,'pidfd_open') or not hasattr(signal,'pidfd_send_signal'):
        raise RuntimeError('safe daemon retirement requires Linux pidfd support')
    try:
        fd=os.pidfd_open(pid)
    except ProcessLookupError:
        return
    try:
        if identity(pid) != before:
            print(f'PID {pid} changed identity; process retained',flush=True)
            return
        print(f'Stopping owned efsd PID {pid} ({kind} {value})',flush=True)
        try:signal.pidfd_send_signal(fd,signal.SIGTERM)
        except ProcessLookupError:return
        poll=select.poll();poll.register(fd,select.POLLIN)
        if not poll.poll(int(timeout*1000)):
            raise RuntimeError(f'efsd PID {pid} did not stop; retained, no SIGKILL attempted')
    finally:
        os.close(fd)


def stop_file(file,storage):
    if not file.exists():
        return
    value=file.read_text().strip()
    stamp=Path(str(file)+'.identity')
    expected=json.loads(stamp.read_text()) if stamp.exists() else None
    if numeric(value):
        retire(int(value),'storage',storage,expected)
    else:
        print(f'Invalid PID record {file}; no process signaled',flush=True)
    file.unlink(missing_ok=True)
    stamp.unlink(missing_ok=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    for command in ('record','stop-file'):
        item=sub.add_parser(command);item.add_argument('file',type=Path);item.add_argument('storage')
    item=sub.add_parser('stop-port');item.add_argument('pid');item.add_argument('port')
    args=parser.parse_args()
    if args.command=='record':record(args.file,args.storage)
    elif args.command=='stop-file':stop_file(args.file,args.storage)
    elif numeric(args.pid) and args.port.isdigit() and 0<int(args.port)<65536:
        retire(int(args.pid),'port',args.port)
    else:raise RuntimeError('invalid PID/port; no process signaled')

if __name__=='__main__':
    try:main()
    except (OSError,RuntimeError,ValueError) as error:
        print(f'ERROR: {error}',file=sys.stderr);sys.exit(1)
