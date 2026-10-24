#!/usr/bin/env python3
"""Linux daemon ownership and graceful retirement; PID alone is not authority.

An efsd stop has one of these outcomes, read from the daemon's exit record
(<first storage root>/log/efsd.exit, written by efsd itself):

  orderly         rc 0: every phase stopped and every durability barrier
                  (PUT sync, reclaim journal, usage flush) completed.
  forced          rc 3: the shutdown deadline passed with a worker live;
                  efsd exited without teardown and left its stores as they
                  were. Reported as "stopped, forced, stores preserved".
  barrier-failed  rc 4: teardown completed, a durability barrier failed.
  unknown         no exit record from this PID (crash, signal, old binary).

Only orderly exits 0. stop-file and stop-port exit 3 for forced and 4 for
barrier-failed or unknown, after the process is gone and its PID record is
removed, so a caller never reports a non-orderly stop as an orderly restart.
Stores are never touched here.
"""
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


# efsd options that take a value (K33 CLI: efsd ADDR:PORT STORAGE, or
# efsd --join [SEED] STORAGE [--listen ADDR:PORT]).
VALUE_OPTIONS=('--listen','--discovery-port','--discover-on','--meta-storage','--quota','--writers')


def efsd_args(args):
    """(listen port or None, first storage root or None) of an efsd command line."""
    positional,listen,join,index=[],None,False,1
    while index<len(args):
        value=args[index]
        if value in VALUE_OPTIONS and index+1<len(args):
            if value=='--listen':listen=args[index+1]
            index+=2;continue
        if value=='--join':join=True
        elif not value.startswith('-'):positional.append(value)
        index+=1
    storage=positional[-1].split(',')[0] if positional else None
    endpoint=listen if join else (positional[0] if len(positional)==2 else None)
    port=endpoint.rsplit(':',1)[1] if endpoint and ':' in endpoint else None
    return port,storage


def owns(info,kind,value):
    if kind=='perf':
        if not info or Path(info['exe']).name != 'perf':return False
        args=info['args']
        return 'record' in args and any(args[index]=='-o' and os.path.realpath(args[index+1])==os.path.realpath(value) for index in range(len(args)-1))
    if not info or Path(info['exe']).name != 'efsd':
        return False
    port,storage=efsd_args(info['args'])
    if kind=='port':
        return port==value
    return bool(storage) and os.path.realpath(storage)==os.path.realpath(value)


def numeric(value):
    return value.isascii() and value.isdigit() and int(value)>1


def record(file,storage,kind='storage'):
    value=file.read_text().strip()
    if not numeric(value):
        raise RuntimeError('invalid daemon PID record')
    end=time.monotonic()+2
    while time.monotonic()<end:
        info=identity(int(value))
        if owns(info,kind,storage):
            tmp=Path(str(file)+f'.identity.{os.getpid()}')
            tmp.write_text(json.dumps(info))
            tmp.replace(str(file)+'.identity')
            return
        time.sleep(.05)
    raise RuntimeError('started PID is not the requested storage daemon')


EXIT_RECORD='log/efsd.exit'
OUTCOME_RC={'orderly':0,'forced':3,'barrier-failed':4,'unknown':4}
BARRIERS=('put_sync','reclaim_journal','usage_flush')


def exit_record_path(info):
    """The exit record of the daemon whose identity is info."""
    storage=efsd_args(info['args'])[1]
    return Path(storage)/EXIT_RECORD if storage else None


def read_exit_record(path):
    try:
        lines=path.read_text().splitlines()
    except (FileNotFoundError,UnicodeDecodeError,OSError):
        return None
    if not lines or lines[0]!='efsd-exit 1':
        return None
    return dict(line.split('=',1) for line in lines[1:] if '=' in line)


def classify(pid,record):
    """Outcome of PID's stop from its exit record: (outcome, message)."""
    if record is None or record.get('pid')!=str(pid):
        return 'unknown',(f'efsd PID {pid} stopped, outcome unknown (no exit record from this '
                          'process), stores preserved; not an orderly stop')
    barriers=', '.join(f"{name} {record.get(name,'missing')}" for name in BARRIERS)
    outcome,rc=record.get('outcome'),record.get('rc')
    if outcome=='orderly' and rc=='0' and all(record.get(name)=='done' for name in BARRIERS):
        return 'orderly',f'efsd PID {pid} stopped, orderly (rc 0; {barriers})'
    if outcome=='forced' and rc=='3':
        return 'forced',(f"efsd PID {pid} stopped, forced, stores preserved (rc 3 after "
                         f"{record.get('elapsed_ms','?')} ms of {record.get('deadline_ms','?')} ms; "
                         f"phase {record.get('phase','?')}, worker {record.get('worker','?')}; "
                         f"{barriers}); not an orderly stop")
    if outcome=='barrier-failed' and rc=='4':
        return 'barrier-failed',(f'efsd PID {pid} stopped, durability barrier failed, stores '
                                 f'preserved (rc 4; {barriers}); not an orderly stop')
    return 'unknown',(f'efsd PID {pid} stopped, outcome unknown (exit record outcome={outcome} '
                      f'rc={rc}), stores preserved; not an orderly stop')


def retire(pid,kind,value,expected=None,timeout=60):
    """Stop an owned daemon. Returns its outcome for efsd, else None."""
    before=identity(pid)
    if not owns(before,kind,value) or (expected is not None and expected != before):
        print(f'Stale daemon identity for PID {pid}; process retained',flush=True)
        return None
    if not hasattr(os,'pidfd_open') or not hasattr(signal,'pidfd_send_signal'):
        raise RuntimeError('safe daemon retirement requires Linux pidfd support')
    try:
        fd=os.pidfd_open(pid)
    except ProcessLookupError:
        return None
    try:
        if identity(pid) != before:
            print(f'PID {pid} changed identity; process retained',flush=True)
            return None
        record=None if kind=='perf' else exit_record_path(before)
        if record is not None:
            # A record left by an earlier incarnation must not speak for this one.
            record.unlink(missing_ok=True)
        print(f"Stopping owned {'perf' if kind=='perf' else 'efsd'} PID {pid} ({kind} {value})",flush=True)
        try:signal.pidfd_send_signal(fd,signal.SIGINT if kind=='perf' else signal.SIGTERM)
        except ProcessLookupError:return None
        poll=select.poll();poll.register(fd,select.POLLIN)
        if not poll.poll(int(timeout*1000)):
            raise RuntimeError(f'efsd PID {pid} did not stop; retained, no SIGKILL attempted')
    finally:
        os.close(fd)
    if kind=='perf':
        return None
    outcome,message=classify(pid,read_exit_record(record) if record else None)
    print(message,flush=True)
    return outcome


def stop_file(file,storage,kind='storage'):
    """Returns the stopped daemon's outcome, or None when none was stopped."""
    if not file.exists():
        return None
    value=file.read_text().strip()
    stamp=Path(str(file)+'.identity')
    expected=json.loads(stamp.read_text()) if stamp.exists() else None
    outcome=None
    if numeric(value):
        outcome=retire(int(value),kind,storage,expected)
    else:
        print(f'Invalid PID record {file}; no process signaled',flush=True)
    file.unlink(missing_ok=True)
    stamp.unlink(missing_ok=True)
    return outcome


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    for command in ('record','stop-file','record-perf','stop-perf'):
        item=sub.add_parser(command);item.add_argument('file',type=Path);item.add_argument('storage')
    item=sub.add_parser('stop-port');item.add_argument('pid');item.add_argument('port')
    args=parser.parse_args()
    outcome=None
    if args.command=='record-perf':record(args.file,args.storage,'perf')
    elif args.command=='stop-perf':stop_file(args.file,args.storage,'perf')
    elif args.command=='record':record(args.file,args.storage)
    elif args.command=='stop-file':outcome=stop_file(args.file,args.storage)
    elif numeric(args.pid) and args.port.isdigit() and 0<int(args.port)<65536:
        outcome=retire(int(args.pid),'port',args.port)
    else:raise RuntimeError('invalid PID/port; no process signaled')
    return OUTCOME_RC[outcome] if outcome else 0

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,ValueError) as error:
        print(f'ERROR: {error}',file=sys.stderr);sys.exit(1)
