#!/usr/bin/env python3
"""Task FN / K108: peak RSS of a member that rejoins through snapshot import.

Three private fault-build efsd members on 127.0.0.1 (ports 18800..18899 only).
EFS_FAULT_SNAP_BYTES shrinks the retained log window. Member 3 is SIGSTOPped
once warm, the load (efs-bench --meta mkdir,create against member 1) runs until
both groups' leaders have snapshotted past member 3's match four times and at
least --files files exist, then member 3 is released (SIGCONT). Member 3 must
rejoin through InstallSnapshot; its VmRSS/VmHWM are sampled every 50 ms from
the release until both groups logged the end of the import and the log is
quiet, plus VmHWM at release and at the end.

  python3 fn_import_rss.py --src ~/efs-taskFN/src --work ~/efs-taskFN/run-a \
      --out ~/efs-artifacts/taskFN-20261110/before-a --files 50000 [--port 18800]

Raw output (logs, samples.tsv) stays out of Git; summary.json is the evidence
input. Cleanup: the script kills its three daemons by pid; remove --work after.
"""
import argparse, json, os, re, signal, subprocess, sys, time
from pathlib import Path


def vm(pid, key):
    try:
        for line in open(f'/proc/{pid}/status'):
            if line.startswith(key + ':'):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def mgmt(mg, env, *args):
    p = subprocess.run([str(mg), *args], capture_output=True, text=True, env=env, timeout=60)
    return p.returncode, p.stdout + p.stderr


def groups(text):
    out, g = {}, None
    for line in text.splitlines():
        m = re.match(r'\s+group (\d+) .*', line)
        if m:
            g = int(m.group(1))
            out[g] = {k: (re.search(k + r'=(\S+)', line) or [None, ''])[1] for k in ('role', 'commit', 'applied', 'snap', 'last')}
            out[g]['peers'] = {}
            continue
        pm = re.match(r'\s+peer (\d+) .*match=(\d+)', line)
        if pm and g is not None:
            out[g]['peers'][int(pm.group(1))] = int(pm.group(2))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--files', type=int, required=True, help='minimum files created while member 3 is stopped')
    ap.add_argument('--port', type=int, default=18800)
    ap.add_argument('--snap-bytes', type=int, default=1 << 20)
    ap.add_argument('--round-files', type=int, default=20000)
    ap.add_argument('--quota', default='8G')
    ap.add_argument('--efsd', default='tests/faults/efsd')
    ap.add_argument('--stop-deadline', type=int, default=1500)
    ap.add_argument('--memtable-bytes', type=int, default=0,
                    help='EFS_FAULT_MEMTABLE_BYTES for every member: shrinks the memtable so its fill (bounded by the 128 MiB budget in production) does not hide what the importer holds')
    ap.add_argument('--kill-delay-s', type=float, default=3.0,
                    help='with --kill-mid-import: seconds after the first import open before the SIGKILL (the apply of ~180 k keys takes ~10 s)')
    ap.add_argument('--kill-mid-import', action='store_true',
                    help='SIGKILL member 3 as soon as its first import is open, restart it, and require the install to repeat and member 3 to catch up')
    a = ap.parse_args()
    assert 18800 <= a.port and a.port + 2 <= 18899, 'ports must stay in 18800..18899'
    efsd, mg, bench = a.src / a.efsd, a.src / 'efs-mgmt', a.src / 'efs-bench'
    a.work.mkdir(parents=True)
    a.out.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}
    env.update(EFS_TRANSPORT='tcp', EFS_RAFT_DBG='1', EFS_RAFT_AE_DBG='1', EFS_FAULT_SNAP_BYTES=str(a.snap_bytes))
    if a.memtable_bytes:
        env['EFS_FAULT_MEMTABLE_BYTES'] = str(a.memtable_bytes)
    eps = {i: f'127.0.0.1:{a.port + i - 1}' for i in (1, 2, 3)}
    procs, logs = {}, {}

    def start(i):
        store = a.work / f'n{i}'
        store.mkdir(exist_ok=True)
        argv = [str(efsd), eps[i], str(store)] if i == 1 else [str(efsd), '--join', eps[1], '--listen', eps[i], str(store)]
        argv += ['--quota', a.quota, '--writers', '2', '--discovery-port', '0']
        logs[i] = a.out / f'n{i}.log'
        f = open(logs[i], 'a')
        f.write(f'==== start {" ".join(argv)}\n')
        f.flush()
        procs[i] = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT, env=env, start_new_session=True)

    def wait_log(i, pat, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pat, logs[i].read_text(errors='replace')):
                return
            time.sleep(0.2)
        raise AssertionError(f'node {i}: {pat!r} not seen')

    def leaders():
        out = {}
        for i in (1, 2):
            rc, t = mgmt(mg, env, 'raft-status', eps[i])
            if rc == 0:
                for g, d in groups(t).items():
                    if d.get('role') == 'LEADER':
                        out[g] = (i, d)
        return out

    res = dict(files_target=a.files, snap_bytes=a.snap_bytes, memtable_bytes=a.memtable_bytes)
    files = 0
    try:
        for i in (1, 2, 3):
            start(i)
            if i == 1:
                time.sleep(1)
        for i in (1, 2, 3):
            wait_log(i, r'raft-host: formed: 3 members')
        pid3 = procs[3].pid
        # warm: a small load so node 3 holds a real KV before the stall
        subprocess.run([str(bench), eps[1], '--meta', '--files', '2000', '--dirs', '16', '--workers', '16',
                        '--phases', 'mkdir,create', '--keep', '--id', '1'], env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=600)
        time.sleep(3)
        res['warm_rss_kb'] = vm(pid3, 'VmRSS')
        res['warm_hwm_kb'] = vm(pid3, 'VmHWM')
        os.kill(pid3, signal.SIGSTOP)
        t0 = time.time()
        seen = {0: set(), 2: set()}
        rnd = 1
        while time.time() - t0 < a.stop_deadline:
            rnd += 1
            rc = subprocess.run([str(bench), eps[1], '--meta', '--files', str(a.round_files), '--dirs', '16',
                                 '--workers', '16', '--phases', 'mkdir,create', '--keep', '--id', str(rnd)],
                                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1200).returncode
            files += a.round_files   # a round that reports errors still wrote most of its files
            if rc:
                res.setdefault('bench_nonzero_rounds', []).append(rnd)
            for g, (node, d) in leaders().items():
                snap = int(d.get('snap') or 0)
                n3 = min(d['peers'].values()) if d.get('peers') else 0
                if snap and snap > n3:
                    seen[g].add(snap)
            if files >= a.files and all(len(v) >= 4 for v in seen.values()):
                break
        else:
            raise AssertionError(f'deadline: snapshots seen {seen}, files {files}')
        res['files_written'] = files
        res['stall_s'] = round(time.time() - t0)
        res['stopped_hwm_kb'] = vm(pid3, 'VmHWM')
        res['stopped_rss_kb'] = vm(pid3, 'VmRSS')
        os.kill(pid3, signal.SIGCONT)
        t_rel = time.time()
        if a.kill_mid_import:
            end = time.time() + 120
            while time.time() < end and not re.search(r'raft-snap: import open|raft-snap: import diff', logs[3].read_text(errors='replace')):
                time.sleep(0.01)
            time.sleep(a.kill_delay_s)   # let batches land: the open itself writes nothing
            os.kill(pid3, signal.SIGKILL)
            procs[3].wait()
            res['killed_after_s'] = round(time.time() - t_rel, 2)
            before = logs[3].read_text(errors='replace')
            res['kill_snap_lines'] = [l for l in before.splitlines() if 'raft-snap: import' in l][-6:]
            res['applied_before_kill'] = re.findall(r'import applied group=(\d+) ', before)
            pre_open = {g: (int(ni), int(nl)) for g, ni, nl in re.findall(r'import open group=(\d+) rc=0 n_in=(\d+) n_local=(\d+)', before)}
            start(3)
            pid3 = procs[3].pid
            end = time.time() + 600
            while time.time() < end:
                last = logs[3].read_text(errors='replace').rsplit('==== start', 1)[-1]
                if len(re.findall(r'on_snap_req .*INSTALLED', last)) >= 2:
                    break
                time.sleep(0.2)
            res['restart_installed'] = len(re.findall(r'on_snap_req .*INSTALLED', last))
            post_open = {g: (int(ni), int(nl)) for g, ni, nl in re.findall(r'import open group=(\d+) rc=0 n_in=(\d+) n_local=(\d+)', last)}
            res['open_before_kill'] = pre_open
            res['open_after_restart'] = post_open
            # a batch landed before the kill iff the restart's local export is no longer the one the first import saw
            res['partial_state_seen'] = [g for g in pre_open if g in post_open and post_open[g][1] != pre_open[g][1]]
            pre_install = last.split('INSTALLED', 1)[0]
            res['restart_applies_before_first_install'] = len(re.findall(r'raft-host: applied |INCOMPATIBLE|KV incomplete', pre_install))
            for _ in range(120):   # caught up: node 3 applied == the leader's commit, per group
                caught = {}
                for i in (1, 2):
                    rc_, t = mgmt(mg, env, 'raft-status', eps[i])
                    for g, d in groups(t).items():
                        if d.get('role') == 'LEADER':
                            caught[g] = int(d['commit'])
                rc_, t3 = mgmt(mg, env, 'raft-status', eps[3])
                n3 = {g: int(d['applied'] or 0) for g, d in groups(t3).items()}
                res['caught_up'] = {g: [n3.get(g), c] for g, c in caught.items()}
                if caught and all(n3.get(g, 0) >= c - 50 for g, c in caught.items()):
                    break
                time.sleep(1)
        samples = open(a.out / 'samples.tsv', 'w')
        samples.write('t_s\trss_kb\thwm_kb\n')
        last_end, quiet, reset_done = None, 0, False
        while time.time() - t_rel < 900:
            samples.write(f'{time.time() - t_rel:.2f}\t{vm(pid3, "VmRSS")}\t{vm(pid3, "VmHWM")}\n')
            txt = logs[3].read_text(errors='replace')
            if not reset_done and 'raft-snap: import start' in txt:
                # The release itself (queued socket data, AppendEntries backlog) raises VmHWM
                # before any import. Record it, then reset the peak so the final VmHWM is
                # the peak from the first import start to the end. /proc/PID/clear_refs 5.
                res['pre_import_hwm_kb'] = vm(pid3, 'VmHWM')
                res['pre_import_rss_kb'] = vm(pid3, 'VmRSS')
                with open(f'/proc/{pid3}/clear_refs', 'w') as f:
                    f.write('5')
                res['hwm_after_reset_kb'] = vm(pid3, 'VmHWM')
                reset_done = True
            ends = len(re.findall(r'on_snap_req .*INSTALLED', txt))
            if ends and ends == last_end:
                quiet += 1
            else:
                quiet, last_end = 0, ends
            if ends >= 2 and quiet > 100:   # >= 5 s with no further install
                break
            time.sleep(0.05)
        samples.close()
        time.sleep(2)
        res['final_hwm_kb'] = vm(pid3, 'VmHWM')
        res['final_rss_kb'] = vm(pid3, 'VmRSS')
        res['peak_growth_kb'] = res['final_hwm_kb'] - res['stopped_hwm_kb']
        res['import_window_growth_kb'] = res['final_hwm_kb'] - res.get('hwm_after_reset_kb', 0) if reset_done else None
        txt = logs[3].read_text(errors='replace')
        res['import_diff'] = re.findall(r'raft-snap: import diff group=(\d+) rc=(-?\d+) n=(\d+)', txt)
        # fixed tree: keys applied and the importer's own buffer high-water per group
        res['import_applied'] = re.findall(r'raft-snap: import applied group=(\d+) incl=\d+ diff_keys=(\d+) buf_peak=(\d+)', txt)
        res['installed'] = len(re.findall(r'on_snap_req .*INSTALLED', txt))
        res['snap_lines'] = [l for l in txt.splitlines() if 'raft-snap: import' in l or 'INSTALLED' in l][-12:]
        res['rc'] = 'ok'
    except BaseException as e:   # keep the failure in the summary
        res['rc'] = f'fail: {e!r}'
    finally:
        for p in procs.values():
            try:
                os.kill(p.pid, signal.SIGCONT)
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
        time.sleep(3)
        for p in procs.values():
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            p.wait()
        (a.out / 'summary.json').write_text(json.dumps(res, indent=2))
        print(json.dumps(res, indent=2))
    return 0 if res.get('rc') == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
