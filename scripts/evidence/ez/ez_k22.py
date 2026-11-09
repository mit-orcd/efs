#!/usr/bin/env python3
"""Task EZ / Stage 1 batch S2: the K22 follower plateau at production caps on distinct hosts.

tests/measure/k22_follower_bound.sh's method (warm, stall the follower's
compactor until a leader reports it busy, hold a plateau window, release,
drain) on three hosts, with the candidate's CLI (positional endpoint, self-
forming cluster, no raft-mkfs) and production bounds: node 3 runs the
EFS_FAULTS build only for the compactor park (EFS_FAULT_COMPACT_STALL=1 +
EFS_FAULT_COMPACT_FILE); no L0 or memtable override, so its bound is the
derived production one (1 GiB L0 cap, 128 MiB memtable budget, 524,288
unapplied entries). Members 1 and 2 are production efsd. Members start and
stop through scripts/evidence/ez/ez_agent.py (no host-to-host ssh); the
efs-bench --meta load runs on the controller's host against member 1.

  python3 ez_k22.py --ctl CTL --out ART/s2 --bin BIN --source SHA \
     --member fcstor007=172.16.223.61:18451 --member fcstor008=... --member fcstor009=...

Every 5 s per node: VmRSS (agent), raft-status bound line and per-group
commit/applied/unapplied/busy, the leader's busy count for node 3, files and
client errors so far -> samples.tsv; one line a minute -> progress.log;
summary.json at the end. PASS: node 3's peaks within its bound during the
plateau (memtable <= budget + one entry batch, L0 <= cap + one flush,
unapplied <= max_unapplied), node 3 RSS flat over the plateau (last-third
growth <= 5% of peak), node 3 never leader, the leaders admitted everything
(admit_busy 0), the quorum kept committing, and node 3 applied every commit in
drain. Client bench-round errors are reported, not gated (K22 does not own
them; see the xorinox page).
"""
import argparse, json, os, re, signal, subprocess, sys, threading, time, traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
import ez_remote as R  # noqa: E402
import rss_plateau as RSS  # noqa: E402
K = R.K
COLS = ('t_s phase node rss_kb group role commit applied unapplied max_unapplied busy_sent '
        'leader_busy_from_n3 kv_mt_bytes kv_mem_budget kv_l0_bytes kv_l0_cap kv_apply_deferred '
        'admit_busy campaigns_held files errors').split()


def kv(text, key):
    m = re.search(rf'\b{key}=(\S+)', text)
    return m.group(1) if m else ''


def parse_status(text):
    bound = next((l for l in text.splitlines() if l.startswith('  bound ')), '')
    groups, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^  group (\d+) (.*)', line)
        if m:
            cur = m.group(1)
            groups[cur] = dict(line=m.group(2), n3_busy='0')
        elif cur and re.match(r'^    peer 2 ', line):
            groups[cur]['n3_busy'] = kv(line, 'busy')
    return bound, groups


class Run:
    def __init__(self, a):
        self.a = a
        self.out = a.out
        self.out.mkdir(parents=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_', 'LD_PRELOAD'))}
        self.env['EFS_TRANSPORT'] = 'tcp'
        placement = {i: (h, ep.split(':')[0], int(ep.split(':')[1])) for i, (h, ep) in enumerate(a.member, 1)}
        work = Path(a.work)
        work.mkdir(parents=True)
        self.c = R.RemoteCluster(work, 'cluster', placement, self.env, a.ctl, self.out / 'logs',
                                 a.bin / 'efsd', f'{a.store_root}/s2')
        res_scope = 'rehearsal (scaled caps)' if a.rehearsal_caps else 'production caps'
        self.scope = res_scope
        self.fault = f'{a.store_root}/s2-compact-park'
        self.stop = threading.Event()
        self.phase = 'start'
        self.t0 = time.monotonic()
        self.counts = {}
        self.loads = []
        self.drv = open(self.out / 'driver.log', 'a', buffering=1)
        self.samples = open(self.out / 'samples.tsv', 'a', buffering=1)
        self.samples.write('\t'.join(COLS) + '\n')
        self.last = {}

    def say(self, msg):
        self.drv.write(f'[k22 {time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}] {msg}\n')

    def status(self, i):
        rc, text = K.mgmt(self.env, 'raft-status', self.c.ep(i))
        return text if rc == 0 else ''

    def sample(self):
        t = int(time.monotonic() - self.t0)
        files = sum(n for n, _ in self.counts.values())
        errs = sum(e for _, e in self.counts.values())
        rows = []
        for i in sorted(self.c.procs):
            p = self.c.procs[i]
            rss = p.rss_kb() if p.poll() is None else 0
            bound, groups = parse_status(self.status(i))
            for g, d in sorted(groups.items()):
                x = d['line']
                row = [t, self.phase, i, rss or 0, g, kv(x, 'role'), kv(x, 'commit'), kv(x, 'applied'),
                       kv(x, 'unapplied'), kv(x, 'max_unapplied'), kv(x, 'busy_sent'), d['n3_busy'],
                       kv(bound, 'kv_mt_bytes'), kv(bound, 'kv_mem_budget'), kv(bound, 'kv_l0_bytes'),
                       kv(bound, 'kv_l0_cap'), kv(bound, 'kv_apply_deferred'), kv(bound, 'admit_busy'),
                       kv(x, 'campaigns_held'), files, errs]
                rows.append(row)
                self.samples.write('\t'.join(map(str, row)) + '\n')
        self.last = rows
        return rows

    def bench(self, w):
        n = e = r = 0
        log = open(self.out / f'load{w}.log', 'a')
        while not self.stop.is_set():
            r += 1
            p = subprocess.Popen([str(self.a.bin / 'efs-bench'), self.c.ep(1), '--meta', '--files',
                                  str(self.a.bench_files), '--dirs', '16', '--workers', '16', '--phases',
                                  'mkdir,create', '--keep', '--id', str(w * 100000 + r)],
                                 stdout=log, stderr=subprocess.STDOUT, env=self.env, start_new_session=True)
            self.loads.append(p)
            if p.wait() == 0:
                n += self.a.bench_files
            elif not self.stop.is_set():
                e += 1
            self.counts[w] = (n, e)
        log.close()

    def progress(self):
        prev = None
        while not self.stop.wait(60):
            n3 = [r for r in self.last if r[2] == 3]
            lead = {r[4]: int(r[6] or 0) for r in self.last if r[5] == 'LEADER'}
            rate = '' if prev is None else ' '.join(f'g{g}={(c - prev.get(g, c)) / 60:.0f}/s' for g, c in lead.items())
            prev = lead
            with open(self.out / 'progress.log', 'a') as f:
                f.write(f'{time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())} phase={self.phase} '
                        f't={int(time.monotonic() - self.t0)} files={sum(n for n, _ in self.counts.values())} '
                        f'errors={sum(e for _, e in self.counts.values())} commit_rate {rate} '
                        + ' '.join(f'n3.g{r[4]}:rss={r[3]} unapplied={r[8]}/{r[9]} mt={r[12]}/{r[13]} '
                                   f'l0={r[14]}/{r[15]} deferred={r[16]} lead_busy={r[11]}' for r in n3) + '\n')

    def phase_run(self, name, seconds, plateau=0):
        self.phase = name
        end, busy_at = time.monotonic() + seconds, None
        self.say(f'phase {name} for up to {seconds} s')
        while time.monotonic() < end:
            rows = self.sample()
            if plateau and busy_at is None and any(r[5] == 'LEADER' and int(r[11] or 0) > 0 for r in rows):
                busy_at = time.monotonic()
                end = busy_at + plateau
                self.say(f'leader reports node 3 busy; plateau window {plateau} s')
                self.plateau_t = int(busy_at - self.t0)
            time.sleep(5)
        return busy_at

    def run(self):
        a = self.a
        res = dict(verdict='FAIL', source=a.source, scope=self.scope, config={k: str(v) for k, v in vars(a).items()})
        threads = []
        try:
            for b in ('efsd', 'efs-mgmt', 'efs-bench', 'faults/efsd'):
                res.setdefault('versions', {})[b] = subprocess.run([str(a.bin / b), '--version'], capture_output=True,
                                                                   text=True).stdout.strip()
            self.c.start(1, quota=a.quota)
            self.c.start(2, join=1, quota=a.quota)
            fault_env = dict(EFS_FAULT_COMPACT_STALL='1', EFS_FAULT_COMPACT_FILE=self.fault)
            if a.rehearsal_caps:   # rehearsal only: the xorinox scaled caps, never S2 evidence
                fault_env.update(EFS_FAULT_L0_BYTES=str(32 << 20), EFS_FAULT_MEMTABLE_BYTES=str(2 << 20))
            self.c.efsd, prod = str(a.bin / 'faults/efsd'), self.c.efsd
            self.c.start(3, join=1, quota=a.quota, extra_env=fault_env)
            self.c.efsd = prod
            for i in (1, 2, 3):
                self.c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
            for g in (0, 2):  # node 3 must follow both groups
                for _ in range(60):
                    lead = re.search(rf'group{g}=node(\d+)', self.status(1))
                    if lead and lead.group(1) != '3':
                        break
                    if lead:
                        K.mgmt(self.env, 'raft-step-down', self.c.ep(3), str(g), '1')
                    time.sleep(1)
                else:
                    raise AssertionError(f'group {g}: node 3 still leads or no leader')
            res['start_status'] = {i: self.status(i) for i in (1, 2, 3)}
            self.say('formed; node 3 follows both groups\n' + res['start_status'][3])
            res['start_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            for w in range(1, a.workers + 1):
                t = threading.Thread(target=self.bench, args=(w,), daemon=True)
                threads.append(t)
                t.start()
            pt = threading.Thread(target=self.progress, daemon=True)
            pt.start()
            self.phase_run('warm', a.warm)
            rc, out = self.c.agent(3).run(['touch', self.fault])
            assert rc == 0, out
            self.say('fault file created: node 3 compactor parks')
            busy = self.phase_run('stall', a.stall, a.plateau)
            res['plateau_start_t'] = getattr(self, 'plateau_t', None)
            rc, out = self.c.agent(3).run(['rm', '-f', self.fault])
            assert rc == 0, out
            self.say('fault file removed')
            self.phase_run('release', a.release)
            self.stop.set()
            for p in self.loads:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGTERM)
            for t in threads:
                t.join(timeout=120)
            self.phase = 'drain'
            d0, caught = time.monotonic(), False
            while time.monotonic() - d0 < a.drain:
                rows = self.sample()
                n3 = [r for r in rows if r[2] == 3]
                if n3 and all(r[6] == r[7] and str(r[8]) == '0' for r in n3):
                    caught = True
                    break
                time.sleep(5)
            res['drain_s'] = round(time.monotonic() - d0)
            res['caught_up'] = caught
            res['stall_reached_bound'] = busy is not None
            res['end_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            self.say(f'drain caught_up={caught} after {res["drain_s"]} s')
            res.update(self.summarize())
            res['verdict'] = 'PASS' if (busy is not None and caught and res['bound_ok'] and res['rss_flat']
                                        and res['n3_never_leader'] and res['leaders_admit_busy'] == 0) else 'FAIL'
        except BaseException:
            res['reason'] = traceback.format_exc()
            self.say('failure\n' + res['reason'])
        finally:
            self.stop.set()
            for p in self.loads:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
            self.c.agent(3).run(['rm', '-f', self.fault])
            res['final_status'] = {i: self.status(i) for i in sorted(self.c.procs) if self.c.alive(i)}
            self.c.stop_all()
            (self.out / 'summary.json').write_text(json.dumps(res, indent=2, default=str))
        print(json.dumps({k: res.get(k) for k in ('verdict', 'reason', 'caught_up', 'bound_ok', 'rss_flat')}))
        return 0 if res['verdict'] == 'PASS' else 1

    def summarize(self):
        rows = [l.split('\t') for l in (self.out / 'samples.tsv').read_text().splitlines()[1:]]
        f = lambda r, c: int(r[COLS.index(c)] or 0)  # noqa: E731
        pl = [r for r in rows if r[1] == 'stall' and self.__dict__.get('plateau_t') is not None
              and int(r[0]) >= self.plateau_t]
        n3 = [r for r in pl if r[2] == '3']
        out = {}
        if n3:
            out['plateau'] = dict(
                seconds=int(n3[-1][0]) - int(n3[0][0]),
                n3_rss_kb=[min(f(r, 'rss_kb') for r in n3), max(f(r, 'rss_kb') for r in n3)],
                n3_unapplied=[min(f(r, 'unapplied') for r in n3), max(f(r, 'unapplied') for r in n3)],
                max_unapplied=f(n3[0], 'max_unapplied'),
                n3_mt=[min(f(r, 'kv_mt_bytes') for r in n3), max(f(r, 'kv_mt_bytes') for r in n3)],
                mem_budget=f(n3[0], 'kv_mem_budget'),
                n3_l0=[min(f(r, 'kv_l0_bytes') for r in n3), max(f(r, 'kv_l0_bytes') for r in n3)],
                l0_cap=f(n3[0], 'kv_l0_cap'))
            p = out['plateau']
            # One entry batch / one flush of slack, as the xorinox page read it.
            out['bound_ok'] = (p['n3_unapplied'][1] <= p['max_unapplied'] and p['n3_mt'][1] <= p['mem_budget'] * 1.01
                               and p['n3_l0'][1] <= p['l0_cap'] + p['mem_budget'])
            pts = [(int(r[0]), f(r, 'rss_kb')) for r in n3 if r[4] == '0']
            out['rss'] = RSS.assess_points(pts) if len(pts) > 3 else dict(verdict='NOTRUN')
            out['rss_flat'] = out['rss'].get('verdict') == 'PASS'
        else:
            out.update(bound_ok=False, rss_flat=False)
        per_phase = {}
        for ph in ('warm', 'stall', 'release', 'drain'):
            lr = [r for r in rows if r[1] == ph and r[5] == 'LEADER']
            rates = {}
            for g in ('0', '2'):
                gr = [r for r in lr if r[4] == g]
                if len(gr) > 1 and int(gr[-1][0]) > int(gr[0][0]):
                    rates[f'g{g}'] = round((f(gr[-1], 'commit') - f(gr[0], 'commit')) / (int(gr[-1][0]) - int(gr[0][0])), 1)
            ph_rows = [r for r in rows if r[1] == ph]
            per_phase[ph] = dict(commit_per_s=rates, files=max((f(r, 'files') for r in ph_rows), default=0),
                                 errors=max((f(r, 'errors') for r in ph_rows), default=0),
                                 n3_peak_rss_kb=max((f(r, 'rss_kb') for r in ph_rows if r[2] == '3'), default=0))
        out['phases'] = per_phase
        out['n3_never_leader'] = not any(r[2] == '3' and r[5] == 'LEADER' for r in rows)
        out['leaders_admit_busy'] = max((f(r, 'admit_busy') for r in rows if r[2] != '3'), default=0)
        out['n3_peaks'] = {c: max((f(r, c) for r in rows if r[2] == '3'), default=0)
                           for c in ('rss_kb', 'unapplied', 'busy_sent', 'kv_mt_bytes', 'kv_l0_bytes',
                                     'kv_apply_deferred', 'campaigns_held')}
        return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ctl', type=Path, required=True)
    p.add_argument('--work', required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--bin', type=Path, required=True)
    p.add_argument('--store-root', default='/tmp/efs-taskEZ/stores')
    p.add_argument('--member', action='append', required=True, type=lambda s: s.split('=', 1))
    p.add_argument('--source', required=True)
    p.add_argument('--quota', default='64G')
    p.add_argument('--workers', type=int, default=8)
    p.add_argument('--bench-files', type=int, default=5000)
    p.add_argument('--warm', type=int, default=300)
    p.add_argument('--stall', type=int, default=8 * 3600, help='deadline to reach the bound plus the plateau')
    p.add_argument('--plateau', type=int, default=1800)
    p.add_argument('--release', type=int, default=300)
    p.add_argument('--drain', type=int, default=1800)
    p.add_argument('--rehearsal-caps', action='store_true', help='scaled node-3 caps; not evidence')
    a = p.parse_args()
    assert len(a.member) == 3, 'S2 is three members; node 3 is the stalled follower'
    a.bin = a.bin.resolve()
    K.MGMT = a.bin / 'efs-mgmt'
    return Run(a).run()


if __name__ == '__main__':
    sys.exit(main())
