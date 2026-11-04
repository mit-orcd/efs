#!/usr/bin/env python3
"""K54 real-host stage on orcd-mini: clean unmount with one storage member down.

Usage: python3 scripts/evidence/orcd-mini/orcd_mini_k54_stage.py --transport rdma|tcp --out RESULT.tsv
           [--runs 6] [--ssh scripts/evidence/orcd-mini/orcd_mini_ssh.sh] [--files 4]

Driver on the Mac (no ssh exists between the nodes). The cluster is the live
orcd-mini one: four efsd on node9901 (n1 seed .. n4, 10.1.99.1:17432-17435),
the client under test on node9900 mounted at /data1/efs/orcd-mini-mnt from
the seed. Per run (member rotating 2, 3, 4; signal alternating SIGKILL and
SIGTERM, so over six runs each member gets both): write and fsync FILES files
through the node9900 mount, stop the member, `client.sh stop` on node9900,
require exit 0 and `session revoked at unmount ... rc=0`, restart the member,
wait for `Cluster state: OK (4 up, 0 down)` with `Protection: full` and no
degraded segments, remount and compare every file byte for byte. A failed
unmount is recorded (with its fence phase) and the run continues.
The cluster must already run with EFS_TRANSPORT=TRANSPORT and be formatted.
"""
import argparse, re, subprocess, sys, time

BASE = '/data1/efs'
SRC = '/data1/efs/orcd-mini-client/src'
MNT = '/data1/efs/orcd-mini-mnt'
CLOG = '/data1/efs/orcd-mini-client/logs/efs-fuse-orcd-mini-mnt.log'
SEED = '10.1.99.1:17432'
PORTS = {1: 17432, 2: 17433, 3: 17434, 4: 17435}

FILES_PY = r'''
import hashlib, os, sys
mode, d, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
def data(name, k):
    out, i = b'', 0
    while len(out) < 65536 + k * 4099:
        out += hashlib.sha256(('%s/%d' % (name, i)).encode()).digest(); i += 1
    return out[:65536 + k * 4099]
if mode == 'write':
    os.makedirs(d, exist_ok=True)
    for k in range(n):
        fd = os.open('%s/f%d' % (d, k), os.O_WRONLY | os.O_CREAT, 0o644)
        os.write(fd, data(d, k)); os.fsync(fd); os.close(fd)
    print('written', n)
else:
    bad = [k for k in range(n) if open('%s/f%d' % (d, k), 'rb').read() != data(d, k)]
    print('bad', len(bad))
    sys.exit(1 if bad else 0)
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--transport', required=True, choices=('rdma', 'tcp'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--runs', type=int, default=6)
    ap.add_argument('--files', type=int, default=4)
    ap.add_argument('--ssh', default='scripts/evidence/orcd-mini/orcd_mini_ssh.sh')
    a = ap.parse_args()
    tenv = 'EFS_TRANSPORT=%s' % a.transport + (' EFS_RDMA_DEV=mlx5_0' if a.transport == 'rdma' else '')

    def sh(host, cmd, stdin=None, timeout=900):
        # orcd_mini_ssh.sh takes the remote command as arguments
        r = subprocess.run(a.ssh.split() + [host, cmd], input=stdin, capture_output=True,
                           text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()

    def log(msg):
        print(msg, flush=True)

    def wait_in_sync(timeout=300):
        end, out = time.time() + timeout, ''
        while time.time() < end:
            rc, out = sh('node9901', '%s/src/efs-mgmt status %s' % (BASE, SEED))
            if (re.search(r'Cluster state: OK \(4 up, 0 down\)', out) and 'Protection: full' in out
                    and re.search(r'Degraded segments: 0 ', out)):
                return True
            time.sleep(2)
        log('NOT IN SYNC:\n' + out)
        return False

    def mount():
        return sh('node9900', '%s %s/scripts/client.sh %s %s' % (
            'EFS_FUSE_LOG=%s %s' % (CLOG, tenv), SRC, SEED, MNT))

    rows = []
    for run in range(1, a.runs + 1):
        victim = (run - 1) % 3 + 2
        sig = 'KILL' if run % 2 else 'TERM'
        d = '%s/k54-%s-%s-r%02d' % (MNT, a.transport, time.strftime('%H%M%S'), run)
        rc, out = sh('node9900', 'python3 - write %s %d' % (d, a.files), stdin=FILES_PY)
        assert rc == 0, 'write failed: ' + out
        _, off = sh('node9900', 'wc -l < %s' % CLOG)
        off = int(off.split()[-1])
        # stop the member and wait for the process to exit
        rc, out = sh('node9901', 'p=$(cat %s/logs/efsd-n%d.pid); kill -%s $p; '
                     'for i in $(seq 1 120); do kill -0 $p 2>/dev/null || break; sleep 0.5; done; '
                     'kill -0 $p 2>/dev/null && echo STILL-RUNNING || echo exited' % (BASE, victim, sig))
        log('run %d: n%d SIG%s -> %s' % (run, victim, sig, out))
        t0 = time.time()
        rc, out = sh('node9900', '%s/scripts/client.sh stop %s' % (SRC, MNT))
        secs = time.time() - t0
        _, line = sh('node9900', "tail -n +%d %s | grep 'session revoked at unmount' | tail -1" % (off + 1, CLOG))
        _, mounted = sh('node9900', 'mountpoint -q %s && echo mounted || echo unmounted' % MNT)
        clean = rc == 0 and re.search(r'\brc=0\b', line) is not None and mounted == 'unmounted'
        phase = (re.search(r'phase=(\S+)', line) or [None, '-'])[1]
        log('  unmount rc=%d %.1fs %s | %s' % (rc, secs, mounted, line or out[-200:]))
        # restart the member with the same command start.sh uses
        ep, st = '10.1.99.1:%d' % PORTS[victim], '%s/n%d' % (BASE, victim)
        rc, out = sh('node9901',
                     'cd %s; n=$(wc -l < logs/efsd-n%d.log); %s nohup %s/src/efsd --join %s --listen %s %s --quota 720G '
                     '>> logs/efsd-n%d.log 2>&1 < /dev/null & echo $! > %s/logs/efsd-n%d.pid; '
                     'python3 %s/src/scripts/server_processes.py record %s/logs/efsd-n%d.pid %s; '
                     'for i in $(seq 1 240); do tail -n +$((n+1)) logs/efsd-n%d.log | grep -q listening && echo up && break; sleep 0.5; done'
                     % (BASE, victim, tenv, BASE, SEED, ep, st, victim, BASE, victim, BASE, BASE, victim, st, victim))
        up = out.endswith('up')
        synced = up and wait_in_sync()
        bad = '-'
        if synced:
            rc, out = mount()
            if rc == 0:
                rc, out = sh('node9900', 'python3 - read %s %d' % (d, a.files), stdin=FILES_PY)
                bad = out.split()[-1] if rc == 0 or 'bad' in out else 'ERR:' + out[-100:]
            else:
                bad = 'MOUNT-FAIL:' + out[-100:]
        row = (run, a.transport, 'n%d' % victim, 'SIG' + sig, 'clean' if clean else 'FAIL', '%.1f' % secs,
               phase, 'synced' if synced else 'NOT-SYNCED', bad)
        rows.append(row)
        log('  -> ' + ' '.join(row[1:]))
        open(a.out, 'a').write('\t'.join(str(x) for x in row) + '\t' + line + '\n')
        if not synced:
            log('stopping: cluster not back in sync; leaving it for inspection')
            break
    ok = sum(r[4] == 'clean' and r[8] == '0' for r in rows)
    log('K54 %s: %d/%d clean unmounts with exact read-back' % (a.transport, ok, len(rows)))
    return 0 if ok == len(rows) else 1


if __name__ == '__main__':
    sys.exit(main())
