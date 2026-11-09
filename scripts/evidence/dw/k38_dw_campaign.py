#!/usr/bin/env python3
"""Task DW: K38 shared-chunk pressure on a distinct-host cluster.

tests/test_k38_shared_chunk_pressure.py --clients 2 runs on loopback only.
This controller runs its shape on five ORCD hosts: three production `efsd`
(fcstor012/013/014, IPoIB 172.16.223.66-68, port 18432) and two `efs-fuse`
mounts (fcstor010 through node 1, fcstor011 through node 2), 24 writers split
over both mounts writing one shared file (scripts/evidence/dw/k38_dw_client.py),
client dcache budget 32 MiB / reclaim 1 MiB, then verify on both mounts and on
a remount through node 3. A fresh cluster (new stores) per run.

Every remote step is a bash script on stdin of `$DW_SSH HOST` (a wrapper that
runs `bash -s` on HOST; Task DW used ssh through orcd-login with BatchMode).
Binaries and k38_dw_client.py are read from DW_BIN on the shared home.

  DW_SSH=./r.sh python3 k38_dw_campaign.py --arm rdma --runs 10 --out DIR

Per run it writes DIR/<arm>-rNN.json and copies every daemon and client log
to DW_ART/runs/<arm>-rNN/<host>/ on the shared home. A run with a writer error
(EIO) or a record that differs stops the campaign (keep every log).
"""
import argparse, json, os, shlex, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

SSH = os.environ.get('DW_SSH', 'ssh')
BIN = os.environ.get('DW_BIN', '/home/erbmi1/efs-taskDW/bin')
ART = os.environ.get('DW_ART', '/home/erbmi1/efs-artifacts/taskDW-20261107')
# DW_RUNROOT, DW_STOREROOT and DW_TMP move the per-host trees (Task EY keeps stores off /data1).
RUNROOT = os.environ.get('DW_RUNROOT', '/tmp/efs-taskDW')
STOREROOT = os.environ.get('DW_STOREROOT', '/data1/01/erbmi1/efs-taskDW')
TMP = os.environ.get('DW_TMP', '/tmp/efs-taskDW-tmp')
PORT = 18432
SERVERS = {1: ('fcstor012', '172.16.223.66'), 2: ('fcstor013', '172.16.223.67'),
           3: ('fcstor014', '172.16.223.68')}
CLIENTS = {'a': ('fcstor010', 1), 'b': ('fcstor011', 2)}
REMOUNT = ('fcstor010', 3)
CLIENT_PAT = {'pressure': 'dcache-pressure ', 'stale_rounds': 'report-stale: chunks=',
              'superseded': ' superseded by seq=', 'report_stuck': 'report-stuck ',
              'span_lost': 'report-span-lost ', 'publish_stalled': 'publish-stalled',
              'image_base': 'why=image-base', 'k94': 'segmented RDMA setup failed or incompatible peer',
              'rdma_up': 'RDMA transport up'}
DAEMON_PAT = {'k94': 'segmented RDMA setup failed or incompatible peer',
              'image_base': 'why=image-base', 'pub_stale': 'pub-stale why=',
              'publish_stalled': 'publish-stalled', 'rdma_up': 'RDMA transport up'}


def rsh(host, body, timeout=120):
    p = subprocess.run([SSH, host], input='set -u\n' + body, capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def must(host, body, timeout=120):
    rc, out = rsh(host, body, timeout)
    if rc:
        raise RuntimeError(f'{host} rc={rc}: {out[-2000:]}')
    return out


def ep(i):
    return f'{SERVERS[i][1]}:{PORT}'


def hosts():
    return [h for h, _ in SERVERS.values()] + sorted({h for h, _ in CLIENTS.values()})


def preflight():
    """Nothing of ours or anyone's on the port, no efsd/efs-fuse of ours."""
    for h in hosts():
        out = must(h, f'''ss -ltn | awk '{{print $4}}' | grep -E ':{PORT}$' && exit 3
pgrep -u "$USER" -x efsd && exit 4; pgrep -u "$USER" -x efs-fuse && exit 5
test ! -e {RUNROOT}/run || exit 6; echo ok''')
        assert out.strip().endswith('ok'), (h, out)


def start_daemon(run, i, arm):
    h, addr = SERVERS[i]
    store = f'{STOREROOT}/{run}/n{i}'
    args = f'{ep(i)} {store}' if i == 1 else f'--join {ep(1)} --listen {ep(i)} {store}'
    must(h, f'''set -e
mkdir -p {RUNROOT}/run {store} {TMP}
test "$(stat -c %U {store})" = "$USER"
cd {RUNROOT}/run
TMPDIR={TMP} EFS_TRANSPORT={arm} EFS_RDMA_DEV=mlx5_2 setsid nohup {BIN}/efsd {args} \
  --quota 1G --writers 2 --discovery-port 0 </dev/null >{RUNROOT}/run/efsd.log 2>&1 &
echo $! > {RUNROOT}/run/efsd.pid''')


def wait_grep(h, f, pat, secs):
    return must(h, f'timeout {secs} bash -c \'until grep -qF -- "$0" "$1"; do sleep 0.2; done\' '
                   f'{shlex.quote(pat)} {f} || {{ echo "{pat} not seen in {f}"; tail -20 {f}; exit 1; }}',
                timeout=secs + 30)


def mount(h, node, name, arm):
    m = f'{RUNROOT}/run/mnt-{name}'
    must(h, f'''set -e
mkdir -p {m} {TMP}
TMPDIR={TMP} EFS_TRANSPORT={arm} EFS_RDMA_DEV=mlx5_2 EFS_DCACHE_HARD_BYTES={32 << 20} \
  EFS_DCACHE_BYTES={1 << 20} setsid nohup {BIN}/efs-fuse {ep(node)} {m} -f \
  </dev/null >{RUNROOT}/run/fuse-{name}.log 2>&1 &
echo $! > {RUNROOT}/run/fuse-{name}.pid
timeout 30 bash -c 'until mountpoint -q {m}; do kill -0 $(cat {RUNROOT}/run/fuse-{name}.pid) || exit 1; sleep 0.1; done' \
  || {{ tail -30 {RUNROOT}/run/fuse-{name}.log; exit 1; }}''')
    return m


def umount(h, name):
    m = f'{RUNROOT}/run/mnt-{name}'
    return rsh(h, f'''f={RUNROOT}/run/fuse-{name}.pid
test -f $f || exit 0
p=$(cat $f)
mountpoint -q {m} && fusermount3 -u {m}
timeout 60 tail --pid=$p -f /dev/null || {{ echo "efs-fuse $p still running"; exit 1; }}
mountpoint -q {m} && {{ echo "{m} still mounted"; exit 1; }}
rm -f $f; echo unmounted''', timeout=120)


def stop_daemon(i):
    h = SERVERS[i][0]
    return rsh(h, f'''f={RUNROOT}/run/efsd.pid
test -f $f || exit 0
p=$(cat $f)
kill -TERM $p 2>/dev/null
timeout 60 tail --pid=$p -f /dev/null || {{ echo "efsd $p still running"; exit 1; }}
rm -f $f; echo stopped''', timeout=120)


def counts(h, files, pats):
    script = ''.join(f'echo "{f} {k} $(grep -cF -- {shlex.quote(v)} {RUNROOT}/run/{f} 2>/dev/null || true)"\n'
                     for f in files for k, v in pats.items())
    script += ''.join(f'echo "{f} transport $(grep -m1 -o \'transport [a-z]*[:(].*\' {RUNROOT}/run/{f} | tr " " _)"\n'
                      for f in files)
    out = {}
    for line in must(h, script).splitlines():
        f, k, *v = line.split(' ', 2) + ['']
        out.setdefault(f, {})[k] = int(v[0]) if v[0].isdigit() else (v[0] or None)
    return out


def client(h, *args, timeout=120):
    rc, out = rsh(h, f'cd {RUNROOT}/run && TMPDIR={TMP} python3 {BIN}/k38_dw_client.py '
                     + ' '.join(map(shlex.quote, args)), timeout)
    try:
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {'ok': False, 'errors': [f'rc={rc}: {out[-2000:]}']}


def collect(run_name):
    for h in hosts():
        dest = f'{ART}/runs/{run_name}/{h}'
        must(h, f'mkdir -p {dest} && cp {RUNROOT}/run/*.log {dest}/ 2>/dev/null; '
                f'cp {RUNROOT}/run/*.json {dest}/ 2>/dev/null; true')


def wipe(run):
    for h in hosts():
        must(h, f'''for m in {RUNROOT}/run/mnt-*; do test -e "$m" || continue
  mountpoint -q "$m" && {{ echo "$m still mounted"; exit 1; }}; done
for d in {RUNROOT}/run {STOREROOT}/{run}; do test -d $d && ~/bin/edelete --force --delete $d >/dev/null; done; echo wiped''')


def one(arm, n):
    run = f'{arm}-r{n:02}'
    res = {'run': run, 'arm': arm, 'started': time.strftime('%Y-%m-%dT%H:%M:%S%z')}
    preflight()
    try:
        for i in (1, 2, 3):
            start_daemon(run, i, arm)
        for i in (1, 2, 3):
            wait_grep(SERVERS[i][0], f'{RUNROOT}/run/efsd.log', 'raft-host: formed: 3 members', 60)
        ma = mount(CLIENTS['a'][0], CLIENTS['a'][1], 'a', arm)
        mb = mount(CLIENTS['b'][0], CLIENTS['b'][1], 'b', arm)
        assert client(CLIENTS['a'][0], 'size', '--mount', ma)['ok'], 'size'
        start_at = float(must(CLIENTS['a'][0], 'date +%s.%N').strip()) + 6
        with ThreadPoolExecutor(2) as pool:
            fa = pool.submit(client, CLIENTS['a'][0], 'write', '--mount', ma, '--index', '0',
                             '--start-at', str(start_at), timeout=700)
            fb = pool.submit(client, CLIENTS['b'][0], 'write', '--mount', mb, '--index', '1',
                             '--start-at', str(start_at), timeout=700)
            res['write'] = {'a': fa.result(), 'b': fb.result()}
        for k in ('a', 'b'):
            res['write'][k].pop('outcomes', None)
        res['write_s'] = max(res['write'][k].get('seconds', 0) for k in ('a', 'b'))
        res['verify'] = {k: client(CLIENTS[k][0], 'verify', '--mount', m, '--what', f'mnt-{k}', timeout=300)
                         for k, m in (('a', ma), ('b', mb))}
        res['unmount'] = {k: umount(CLIENTS[k][0], k) for k in ('a', 'b')}
        mr = mount(REMOUNT[0], REMOUNT[1], 'r', arm)
        res['verify']['remount'] = client(REMOUNT[0], 'verify', '--mount', mr, '--what', 'remount', timeout=300)
        res['unmount']['r'] = umount(REMOUNT[0], 'r')
    except Exception as e:  # noqa: BLE001 - recorded; teardown below still runs
        res['harness_error'] = repr(e)[-3000:]
    finally:
        for k, h in (('a', CLIENTS['a'][0]), ('b', CLIENTS['b'][0]), ('r', REMOUNT[0])):
            umount(h, k)
        res['stop'] = {i: stop_daemon(i) for i in (1, 2, 3)}
        res['clients'] = {}
        where = {'a': CLIENTS['a'][0], 'b': CLIENTS['b'][0], 'r': REMOUNT[0]}
        for h in sorted(set(where.values())):
            res['clients'][h] = counts(h, [f'fuse-{k}.log' for k, kh in where.items() if kh == h],
                                       CLIENT_PAT)
        res['daemons'] = {SERVERS[i][0]: counts(SERVERS[i][0], ['efsd.log'], DAEMON_PAT)['efsd.log']
                          for i in (1, 2, 3)}
        collect(run)
        wipe(run)
    w, v = res.get('write', {}), res.get('verify', {})
    cl = [c for h in res['clients'].values() for c in h.values()]
    res['eio_or_torn'] = (not all(w.get(k, {}).get('ok') for k in ('a', 'b'))
                          or not all(v.get(k, {}).get('ok') for k in ('a', 'b', 'remount')))
    res['pressure'] = sum(c.get('pressure') or 0 for c in cl)
    res['k94_daemon'] = sum(d.get('k94') or 0 for d in res['daemons'].values())
    bad_logs = sum((c.get(k) or 0) for c in cl for k in ('report_stuck', 'span_lost', 'publish_stalled', 'image_base'))
    bad_logs += sum((d.get(k) or 0) for d in res['daemons'].values() for k in ('image_base', 'publish_stalled'))
    res['pass'] = (not res['eio_or_torn'] and not bad_logs and res['pressure'] > 0
                   and 'harness_error' not in res)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--arm', choices=('rdma', 'tcp'), required=True)
    ap.add_argument('--runs', type=int, required=True)
    ap.add_argument('--first', type=int, default=1)
    ap.add_argument('--out', required=True)
    opt = ap.parse_args()
    os.makedirs(opt.out, exist_ok=True)
    for n in range(opt.first, opt.first + opt.runs):
        res = one(opt.arm, n)
        with open(os.path.join(opt.out, f"{res['run']}.json"), 'w') as f:
            json.dump(res, f, indent=1)
        print(f"{res['run']} pass={res['pass']} write_s={res.get('write_s')} pressure={res['pressure']} "
              f"k94={res['k94_daemon']} eio_or_torn={res['eio_or_torn']} "
              f"err={res.get('harness_error', '')[:200]}", flush=True)
        if res['eio_or_torn'] or 'harness_error' in res:
            print('stopping: EIO, torn record or harness error; every log kept', flush=True)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
