#!/usr/bin/env python3
"""Private four-daemon NUC A/B; run as efs on nuc_efs, after native build.

Never calls installed start/stop scripts. Fresh roots and ports are mandatory.
"""
import argparse
import json
import os
import re
from pathlib import Path
import signal
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=30, choices=range(121))
    parser.add_argument('--repeat', type=int, default=2, choices=range(1,5))
    parser.add_argument('--resume', action='store_true', help='continue after private processes have stopped')
    args = parser.parse_args()
    base = args.base.resolve()
    if base.parent != Path('/data1/efs/bench') or os.getuid() == 0:
        raise RuntimeError('run unprivileged with a direct child of /data1/efs/bench')
    src = base/'src'
    output = base/'results'
    output.mkdir(exist_ok=args.resume)
    nvme = Path('/data2/efs/bench')/base.name
    nvme.mkdir(parents=True, exist_ok=True)
    mount = base/'mnt'
    mount.mkdir(exist_ok=args.resume)
    ports = list(range(27432,27436))
    seed = '127.0.0.1:27432'
    env = dict(os.environ, EFS_MD_RAFT_N='4', EFS_TRANSPORT='tcp')
    # Pin relevant knobs; do not inherit experimental launch environment.
    for key in list(env):
        if key.startswith('EFS_') and key not in ('EFS_MD_RAFT_N','EFS_TRANSPORT'):
            del env[key]
    children = []
    def run(argv, timeout=120):
        result = subprocess.run([str(x) for x in argv], env=env, text=True,
                                capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'{argv}: {result.returncode}\n{result.stdout}\n{result.stderr}')
        return result.stdout
    def snapshot():
        rows = {}
        for path in Path('/proc').glob('[0-9]*/cmdline'):
            try:
                cmd = path.read_bytes().split(b'\0')
                if Path(os.fsdecode(cmd[0])).name in ('efsd','efs-fuse'):
                    rows[path.parent.name] = [os.fsdecode(x) for x in cmd if x]
            except (FileNotFoundError, PermissionError, IndexError):
                pass
        return rows
    baseline = snapshot()
    if args.resume:
        assert baseline == json.loads((output/'installed-before.json').read_text()), 'installed processes changed before resume'
    else:
        (output/'installed-before.json').write_text(json.dumps(baseline,indent=2)+'\n')
    for port in ports:
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('127.0.0.1',port))
    def save(path, data):
        path.write_text(json.dumps(data,indent=2)+'\n')
    config = {'seconds':args.seconds,'repeat':args.repeat,
         'ports':ports,'env':{k:v for k,v in env.items() if k.startswith('EFS_')},
         'source':str(src),'nvme_base':str(nvme.resolve()),'writers':2,'quota':'2G'}
    if args.resume:
        assert config == json.loads((output/'config.json').read_text()), 'resume configuration differs'
    else:
        save(output/'config.json',config)
    def stop():
        if os.path.ismount(mount):
            run([src/'efs-fuse','--stop',mount],90)
            run(['fusermount3','-u',mount],90)
        for proc, argv, handle in children:
            if proc.poll() is None:
                actual = Path(f'/proc/{proc.pid}/cmdline').read_bytes().split(b'\0')
                if [os.fsdecode(x) for x in actual if x] != argv:
                    raise RuntimeError('private PID command changed; refusing signal')
                proc.send_signal(signal.SIGTERM)
        for proc, argv, handle in children:
            proc.wait(timeout=60)
            handle.close()
        children.clear()
    def start(case, roots, engine, mode, restart=False):
        tag = 'restart' if restart else 'start'
        commands = []
        for i,root in enumerate(roots):
            argv = [str(src/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1',
                    '--port',str(ports[i]),'--storage',str(root),'--quota','2G',
                    '--writers','2','--storage-engine',engine,
                    '--direct-io' if mode=='direct' else '--no-direct-io']
            if i:
                argv += ['--join',seed]
            handle = (case/f'node{i+1}-{tag}.log').open('w')
            proc = subprocess.Popen(argv,env=env,stdout=handle,stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL)
            children.append((proc,argv,handle))
            time.sleep(.2)
            actual = Path(f'/proc/{proc.pid}/cmdline').read_bytes().split(b'\0')
            actual_env = Path(f'/proc/{proc.pid}/environ').read_bytes().split(b'\0')
            assert [os.fsdecode(x) for x in actual if x] == argv
            assert b'EFS_MD_RAFT_N=4' in actual_env and b'EFS_TRANSPORT=tcp' in actual_env
            commands.append({'pid':proc.pid,'argv':argv,'storage_canonical':str(root.resolve()),
                             'environment':['EFS_MD_RAFT_N=4','EFS_TRANSPORT=tcp']})
        save(case/f'daemon-arguments-{tag}.json',commands)
        deadline = time.monotonic()+90
        while time.monotonic()<deadline:
            if any(p.poll() is not None for p,_,_ in children):
                raise RuntimeError('private daemon exited during startup')
            try:
                status = run([src/'efs-mgmt','status',seed],10)
                if all(f'127.0.0.1:{port}' in status for port in ports):
                    raft = [run([src/'efs-mgmt','raft-status',f'127.0.0.1:{port}'],10) for port in ports]
                    hosted = [line for value in raft for line in value.splitlines() if 'hosted=1' in line]
                    # Membership precedes elections; require both groups to know a leader.
                    if hosted and all(re.search(r'leader=\d+',line) for line in hosted) and all(
                        sum(f'group {group} hosted=1 role=LEADER' in line for line in hosted)==1
                        for group in (0,2)):
                        (case/f'raft-status-{tag}.txt').write_text('\n'.join(raft))
                        break
            except (RuntimeError,subprocess.TimeoutExpired):
                pass
            time.sleep(.5)
        else:
            raise RuntimeError('four-node membership not ready')
        (case/f'status-{tag}.txt').write_text(status)
        if not restart:
            attempts = []
            for attempt in range(30):
                result = subprocess.run([str(src/'efs-mgmt'),'raft-mkfs',seed,'fs'],
                         env=env,text=True,capture_output=True,timeout=180)
                attempts.append({'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
                save(case/'mkfs-attempts.json',attempts)
                if result.returncode==0:
                    (case/'mkfs.txt').write_text(result.stdout+result.stderr)
                    break
                # MKFS retries preserve the committed salt (W37). Only retry
                # explicit election/BUSY replies on this fresh private group.
                if not re.search(r'rc=-(13|15)\b',result.stdout):
                    raise RuntimeError('non-transient private mkfs failure: '+result.stdout+result.stderr)
                time.sleep(1)
            else:
                raise RuntimeError('private mkfs did not become ready')
        with (case/f'client-{tag}.log').open('w') as log:
            client = subprocess.run([str(src/'efs-fuse'),seed,'fs',str(mount)],env=env,
                        stdout=log,stderr=subprocess.STDOUT,timeout=90)
            if client.returncode:
                raise RuntimeError('private FUSE client failed')
        assert os.path.ismount(mount)
    rows = json.loads((output/'results.json').read_text()) if args.resume else []
    assert all(row['status']=='PASS' for row in rows)
    completed = {row['name'] for row in rows}
    order = [('files','buffered'),('containers','direct'),('containers','buffered'),('files','direct')]
    try:
        for repeat in range(1,args.repeat+1):
            for engine, mode in order if repeat%2 else order[::-1]:
                name = f'{engine}-{mode}-r{repeat}'
                if name in completed:
                    continue
                print('START '+name,flush=True)
                case = output/name
                case.mkdir()
                roots = [(base if i<2 else nvme)/name/f'n{i+1}' for i in range(4)]
                for root in roots:
                    root.mkdir(parents=True,exist_ok=False)
                start(case,roots,engine,mode)
                phases = []
                def phase(kind,restarted=False):
                    label = ('restart-' if restarted else '')+kind
                    duration = args.seconds if kind in ('read','churn') else 0
                    result = run(['python3',src/'tests/live/storage_engine_ab.py',kind,
                           '--mount',mount,'--state',case/'expected.json',
                           '--output',case/(label+'.json'),'--seconds',str(duration)],600)
                    value = json.loads(result.strip().splitlines()[-1])
                    value['phase'] = label
                    phases.append(value)
                def inventory():
                    return [json.loads(run(['python3',src/'tests/live/storage_engine_inventory.py',root])) for root in roots]
                for kind in ('create','read','churn','verify'):
                    phase(kind)
                save(case/'inventory-live.json',inventory())
                stop()
                start(case,roots,engine,mode,True)
                phase('verify',True)
                phase('delete',True)
                deadline = time.monotonic()+120
                while True:
                    inv = inventory()
                    save(case/'inventory-delete.json',inv)
                    if all(v['data_records']==0 and v['container_allocated']==0 for v in inv):
                        break
                    if time.monotonic()>deadline:
                        raise RuntimeError('GC did not retire all data within 120 seconds')
                    time.sleep(1)
                for i,port in enumerate(ports,1):
                    (case/f'gc-node{i}.txt').write_text(run([src/'efs-mgmt','gc-status',f'127.0.0.1:{port}']))
                stop()
                rows.append({'name':name,'engine':engine,'io':mode,'repeat':repeat,
                             'status':'PASS','phases':phases})
                save(output/'results.json',rows)
                print('PASS '+name,flush=True)
        after = snapshot()
        save(output/'installed-after.json',after)
        assert baseline == after, 'installed daemon/client identities changed'
        (output/'finished').write_text('PASS\n')
    finally:
        stop()

if __name__ == '__main__':
    main()
