#!/usr/bin/env python3
"""Task EZ / Stage 1 batch S1: the E4/D4 soak (scripts/soak.py) on distinct hosts.

scripts/soak.py is one host, loopback, five to seven fault-build members. This
controller keeps its workload, verification, recovery checks and churn code
(two mounts, a fixed 256-slot working set per mount, 1 B to 64 MiB private
overwrites and truncates with fsync acknowledgement, a shared O_APPEND file,
`.efs/stats` queries, verify every --verify-seconds on both mounts and at the
end after a remount) and changes only the shape:

- members are production `efsd` on the hosts given with --member, over TCP
  (EFS_TRANSPORT=tcp) on their IPoIB addresses, stores on each host's local
  /tmp, started and signalled through scripts/evidence/ez/ez_agent.py (the
  hosts refuse ssh to each other; the home is shared);
- the two mounts run on the controller's host, through members 1 and 2;
- the schedule is soak.py's: a SIGTERM clean stop every 600 s (seeded
  victims), a SIGKILL crash at every hour - 120 s, a client kill/revoke/
  remount at every hour - 60 s; the crash victim is the current group-0
  leader (the K43 incident's role), recorded with every member's raft roles
  at the moment of each kill; no member additions (the shape has a member
  per host);
- RSS is sampled through the agents and reported, not gated (shared hosts);
- progress.log gets one line a minute (acks, appends, events, errors).

  python3 ez_soak.py --ctl CTL --work /tmp/efs-taskEZ/s1 --out ART/s1 \
     --bin ~/work/efs-taskEZ-bin --source SHA \
     --member fcstor007=172.16.223.61:18441 --member fcstor008=172.16.223.62:18442 ...

Exit 0 only for PASS: every acknowledged file and append exact on both mounts
at every verify and after the final remount, no writer error, every event's
recovery settled within --recovery-bound.
"""
import argparse, json, os, re, signal, subprocess, sys, threading, time, traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(HERE))
import soak as S  # noqa: E402
import ez_remote as R  # noqa: E402
K = R.K


def ez_schedule(seconds, seed, members, scale=1):
    """soak.schedule's events over `members` members and no additions;
    scale < 1 compresses the clock for a rehearsal, as soak.py's does."""
    import random
    rng = random.Random(seed)
    victims = list(range(1, members + 1))
    rng.shuffle(victims)
    events = []
    for k, at in enumerate(range(600, int(seconds / scale) + 1, 600)):
        if at * scale >= seconds:
            break
        events.append(dict(at=at * scale, kind='clean', victim=victims[k % members]))
    for hour in range(1, int(seconds / (3600 * scale)) + 1):
        for offset, kind, victim in ((-120, 'crash', 'group0-leader'), (-60, 'client', (hour - 1) % 2)):
            if (hour * 3600 + offset) * scale < seconds:
                events.append(dict(at=(hour * 3600 + offset) * scale, kind=kind, victim=victim))
    return sorted(events, key=lambda e: e['at'])


def same_netns(pid):
    try:
        return os.readlink(f'/proc/{pid}/ns/net') == os.readlink('/proc/self/ns/net')
    except OSError:
        return False


class EZSoak(S.Soak):
    acks = 0

    def note(self, kind, **fields):
        if kind.startswith('ack'):
            self.acks += 1
        super().note(kind, **fields)

    def unrelated_jobs(self):
        # Shared hosts (Task EZ): jobs in other network namespaces (the W123
        # fleet's unshared lanes) cannot touch our ports or mounts; recorded
        # on the page, not a refusal. Ours in our namespace still refuse.
        with self.mu:
            jobs = S.foreign_jobs(p.pid for i, p in self.c.procs.items() if self.c.where[i] == self.a.here)
        return [j for j in jobs if same_netns(j['pid'])]

    def roles(self):
        out = {}
        for i in sorted(self.c.procs):
            if not self.c.alive(i):
                continue
            rc, text = K.mgmt(self.env, 'raft-status', self.c.ep(i))
            out[i] = {g: role for g, role in re.findall(r'^\s+group (\d+) .*?role=(\w+)', text, re.M)}
        return out

    def churn(self, event):
        if event['victim'] == 'group0-leader':
            roles = self.roles()
            leaders = [i for i, r in roles.items() if r.get('0') == 'LEADER']
            assert len(leaders) == 1, f'group 0 leader not unique: {roles}'
            event = dict(event, victim=leaders[0])
        return super().churn(event)

    def stop_node(self, i, sig):
        self.note('kill', victim=i, host=self.c.where[i], sig=int(sig), roles=self.roles())
        r = super().stop_node(i, sig)
        self.note('killed', victim=i, host=self.c.where[i], sig=int(sig))
        return r

    def sample(self):
        with self.mu:
            points = []
            for i, p in self.c.procs.items():
                rss = p.rss_kb() if p.poll() is None else None
                if rss:
                    self.rss_roles[f'{self.c.where[i]}-{p.pid}'] = f'node-{i}'
                    points.append(f'{self.c.where[i]}-{p.pid}:{rss}')
            for idx, m in enumerate(self.mounts):
                if m.p.poll() is None:
                    self.rss_roles[f'{self.a.here}-{m.p.pid}'] = f'client-{idx}'
                    rss = re.search(r'^VmRSS:\s+(\d+) kB', Path(f'/proc/{m.p.pid}/status').read_text(), re.M)
                    points.append(f'{self.a.here}-{m.p.pid}:{rss.group(1)}')
            if self.rss_fp:
                self.rss_fp.write(f'{int(time.time())} ' + ' '.join(points) + '\n')
            self.note('rss', values=points)

    def progress(self):
        last = (time.monotonic(), 0)
        with open(self.out / 'progress.log', 'a', buffering=1) as f:
            while not self.monitor_stop.wait(60):
                now, acks = time.monotonic(), self.acks
                f.write(f'{time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())} phase={self.phase} '
                        f'load_s={int(now - getattr(self, "load_start", now))} acks={acks} '
                        f'acks_per_min={acks - last[1]} files={len(self.manifest)} '
                        f'appends={sum(len(v) for v in self.appends.values())} events={len(self.events)} '
                        f'churning={int(self.churning)} errors={len(self.errors)}\n')
                last = (now, acks)

    def run(self):
        a = self.a
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_', 'LD_PRELOAD'))}
        self.env['EFS_TRANSPORT'] = 'tcp'
        placement = {i: (h, ep.split(':')[0], int(ep.split(':')[1])) for i, (h, ep) in enumerate(a.member, 1)}
        self.c = R.RemoteCluster(self.work, 'cluster', placement, self.env, a.ctl, self.out / 'logs',
                                 a.bin / 'efsd', f'{a.store_root}/s1')
        n = len(placement)
        self.threads = []
        result = dict(verdict='FAIL', source=a.source, schedule=ez_schedule(a.seconds, a.seed, n, a.schedule_scale),
                      placement={i: list(v) for i, v in placement.items()}, controller=a.here,
                      config={k: str(v) for k, v in vars(a).items()},
                      scope=f'{n} production members on {len(set(h for h, _, _ in placement.values()))} hosts, '
                            'TCP over IPoIB, two mounts on the controller host; SIGKILL not power loss')
        try:
            for binary in ('efsd', 'efs-mgmt', 'efs-fuse'):
                rc, version = self.command([str(a.bin / binary), '--version'], 10, 'build-identity')
                assert rc == 0, version
                result.setdefault('versions', {})[binary] = version.strip()
            for i in (1, 2, 3):
                self.start_node(i, join=None if i == 1 else 1, quota=a.quota)
            for i in (1, 2, 3):
                self.c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
            for i in range(4, n + 1):
                self.start_node(i, join=1, quota=a.quota)
                self.c.wait_members(list(range(1, i + 1)), i, timeout=a.recovery_bound)
            self.mounts = [K.Mount(self.c, 1, 'mnt-0'), K.Mount(self.c, 2, 'mnt-1')]
            self.settle()
            rc, text = self.command([str(S.MGMT), 'commands-policy', self.c.ep(1), 'get'], 60, 'commands-policy')
            assert rc == 0, text
            self.commands_enabled = bool(int(re.search(r'mask=(\d+)', text).group(1)))
            result['commands_enabled'] = self.commands_enabled
            if self.commands_enabled:
                end = time.monotonic() + 120
                for mount in self.mounts:
                    while True:
                        rc, text = self.command([sys.executable, str(mount.path / '.efs/stats'), '--json'], 30, 'query-ready')
                        if rc == 0:
                            break
                        assert rc == 75 and 'not indexed yet' in text and time.monotonic() < end, text
                        time.sleep(2)
            self.phase = 'warmup'
            for i in range(2):
                t = threading.Thread(target=self.worker, args=(i,), daemon=True)
                self.threads.append(t)
                t.start()
            warm_end = time.monotonic() + 180
            while len(self.manifest) < a.slots * 2:
                assert not self.stop.is_set(), self.errors
                assert time.monotonic() < warm_end, 'fixed working set warmup exceeded 180s'
                time.sleep(.2)
            self.note('warmup-complete', files=len(self.manifest))
            self.phase = 'load'
            self.load_start = time.monotonic()
            result['load_start_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            self.quiet(True)
            pending = list(result['schedule'])
            for target, args in ((self.monitor, ('rss',)), (self.monitor, ('status',)), (self.progress, ())):
                t = threading.Thread(target=target, args=args, daemon=True)
                self.threads.append(t)
                t.start()
            verify_at = a.verify_seconds
            while time.monotonic() - self.load_start < a.seconds:
                assert not self.stop.is_set(), self.errors
                elapsed = time.monotonic() - self.load_start
                if pending and elapsed >= pending[0]['at']:
                    self.churn(pending.pop(0))
                if elapsed >= verify_at:
                    self.verify()
                    verify_at = elapsed + a.verify_seconds
                self.stop.wait(.25)
            self.stop.set()
            self.monitor_stop.set()
            for t in self.threads:
                t.join(timeout=a.churn_deadline + 5)
                assert not t.is_alive(), 'worker failed to finish'
            assert not self.errors, self.errors
            assert not pending, f'unexecuted events: {pending}'
            result['load_end_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            self.phase = 'final'
            self.verify()
            self.quiet(False)
            self.stop.clear()
            for idx, m in enumerate(self.mounts):
                m.close()
                self.mounts[idx] = K.Mount(self.c, 1 + idx, f'mnt-final-{idx}')
            self.settle(orphans_revoked=True)
            self.verify()
            self.stop.set()
            self.sample()
            try:
                result['rss'] = self.plateau()
            except AssertionError as e:   # reported, not gated: shared hosts
                result['rss'] = json.loads((self.out / 'plateau.json').read_text()) if (self.out / 'plateau.json').exists() else repr(e)
            result.update(verdict='PASS', files=len(self.manifest), acks=self.acks,
                          appends=sum(len(v) for v in self.appends.values()), events=self.events,
                          reason='every acknowledged current version and append exact on both mounts at every '
                                 'verify and after the final remount; no writer error; every recovery settled')
        except BaseException:
            result['reason'] = traceback.format_exc()
            self.note('failure', trace=result['reason'], phase=self.phase)
        finally:
            self.stop.set()
            self.monitor_stop.set()
            self.quiet(False)
            for t in self.threads:
                t.join(timeout=a.churn_deadline + 5)
            with self.mu:
                for p in list(self.procs):
                    if p.poll() is None:
                        os.killpg(p.pid, signal.SIGKILL)
            for m in list(self.mounts):
                m.close()
            self.c.stop_all()
            for path in self.c.dir.glob('*.log'):
                (self.out / 'logs').mkdir(exist_ok=True)
                (self.out / 'logs' / path.name).write_bytes(path.read_bytes())
            result.update(seconds=time.monotonic() - self.started, completed_events=self.events,
                          acks=self.acks, end_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
            self.log.close()
            (self.out / 'result.json').write_text(json.dumps(result, indent=2, default=str))
        print(json.dumps(dict(verdict=result['verdict'], reason=result['reason'][-2000:])))
        return 0 if result['verdict'] == 'PASS' else 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ctl', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--bin', type=Path, required=True)
    p.add_argument('--store-root', default='/tmp/efs-taskEZ/stores')
    p.add_argument('--member', action='append', required=True, type=lambda s: s.split('=', 1),
                   help='HOST=ADDR:PORT, in node order (1 is the seed)')
    p.add_argument('--hours', type=float, default=4)
    p.add_argument('--seconds', type=float)
    p.add_argument('--seed', type=int, default=12345)
    p.add_argument('--schedule-scale', type=float, default=1, help='rehearsal only')
    p.add_argument('--source', required=True)
    p.add_argument('--verify-seconds', type=float, default=900)
    p.add_argument('--recovery-bound', type=float, default=300)
    p.add_argument('--down-seconds', type=float, default=5)
    p.add_argument('--io-deadline', type=float, default=60)
    p.add_argument('--meta-deadline', type=float, default=10)
    p.add_argument('--churn-deadline', type=float, default=180)
    p.add_argument('--slots', type=int, default=256)
    p.add_argument('--pace', type=float, default=.1)
    p.add_argument('--quota', default='8G')
    a = p.parse_args(argv)
    a.seconds = a.seconds if a.seconds is not None else a.hours * 3600
    a.profile = 'endurance'
    a.here = os.uname().nodename.split('.')[0]
    a.bin = a.bin.resolve()
    assert len(a.member) >= 4, 'S1 needs four members'
    K.FUSE = a.bin / 'efs-fuse'
    K.MGMT = S.MGMT = a.bin / 'efs-mgmt'
    return EZSoak(a).run()


if __name__ == '__main__':
    sys.exit(main())
