#!/usr/bin/env python3
"""A9: measure where the metadata group leaders sit over a run.

Elections alone place the leaders (D42; no balance pump). This script
records, against a running cluster, which node leads each metadata group
over time, through forced step-downs and member restarts, and reports the
distribution and the share of time every group's leader sat on one node.

  leader_placement.py run --seed ADDR:PORT [--duration S] [--interval S]
                          [--stepdowns N] [--restarts N --restart-cmd CMD]
                          [--mgmt PATH] [--json OUT] [--label TEXT]
  leader_placement.py report RUN.json     the tables of a recorded run

Each sample asks every member (from `efs-mgmt status SEED`) for
`efs-mgmt raft-status` in parallel and keeps the self-reported roles: a
group's leader is the member reporting `role=leader`, the highest term when
two claim it (the deposed one has not yet heard), none when no reachable
member claims it. A sample's state holds until the next sample, so shares
are time-weighted over the run.

Events are spread evenly over the run, step-downs and restarts interleaved.
A step-down asks the current leader of a group (groups taken in turn) to
stand down with no target (`efs-mgmt raft-step-down EP GROUP`), so the next
election decides. A restart stops and starts the current leader of a group
(groups in turn) through --restart-cmd, a shell command with {node} (node
id), {ep} (ADDR:PORT) and {action} (`stop` or `start`) substituted; it runs
`stop`, waits --down seconds, runs `start`. Without --restart-cmd, restarts
are refused. The script never starts or stops anything else.

The parsing and the summary are pure functions (parse_raft_status,
parse_members, leaders_of, summarize, format_report), checked by
tests/test_leader_placement.py on fixed `raft-status` output.
"""
import argparse, json, os, re, shlex, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

_GROUP = re.compile(r'^\s+group (\d+) hosted=(\d+) role=(\S+) leader=(-?\d+) term=(\d+) '
                    r'.*?voters=(0x[0-9a-f]+) joint=\d+ retired=(\d+)', re.M)
_NODE = re.compile(r'^node (\d+) root=', re.M)
_MEMBER = re.compile(r'^\s+node (\d+): (\S+)\s+(up|DOWN)\b', re.M)


def parse_raft_status(text):
    """`efs-mgmt raft-status` output -> {'node': id, 'groups': {group: {...}}}.
    leader is the node id this member believes leads (0 unknown): the
    status line prints raft ids, node id = raft id + 1."""
    m = _NODE.search(text)
    if not m:
        raise ValueError('not raft-status output: ' + text[:200])
    groups = {}
    for g, hosted, role, leader, term, voters, retired in _GROUP.findall(text):
        groups[int(g)] = {'hosted': int(hosted), 'role': role.lower(),
                          'leader': int(leader) + 1 if int(leader) >= 0 else 0,
                          'term': int(term), 'voters': int(voters, 16),
                          'retired': int(retired)}
    return {'node': int(m.group(1)), 'groups': groups}


def parse_members(text):
    """`efs-mgmt status` output -> {node id: (endpoint, up)}."""
    n = re.search(r'Cluster nodes \((\d+)\)', text)
    if not n:
        raise ValueError('not efs-mgmt status output: ' + text[:200])
    rows = {int(k): (ep, st == 'up') for k, ep, st in _MEMBER.findall(text)}
    if len(rows) != int(n.group(1)):
        raise ValueError(f'status lists {len(rows)} of {n.group(1)} members')
    return rows


def leaders_of(statuses, groups=None):
    """Self-reported leaders from one sample: statuses is a list of parsed
    raft-status results (unreachable members omitted). Returns
    {group: node id or 0}. With two claims the higher term wins."""
    best = {}
    seen = set()
    for st in statuses:
        for g, gs in st['groups'].items():
            seen.add(g)
            if gs['hosted'] and not gs['retired'] and gs['role'] == 'leader':
                if g not in best or gs['term'] > best[g][1]:
                    best[g] = (st['node'], gs['term'])
    want = sorted(groups if groups is not None else seen)
    return {g: best[g][0] if g in best else 0 for g in want}


def summarize(samples, end=None):
    """samples: [(t, {group: leader node or 0})] in time order; each state
    holds until the next sample (the last until `end`, default its own t).
    Returns time-weighted shares and leader changes."""
    if not samples:
        raise ValueError('no samples')
    end = samples[-1][0] if end is None else end
    groups = sorted({g for _, s in samples for g in s})
    total = 0.0
    per = {g: {} for g in groups}           # group -> {node (0 = none): seconds}
    together = 0.0                          # every group led, all by one node
    all_led = 0.0
    nodes_held = {}                         # distinct leader nodes -> seconds
    changes = {g: 0 for g in groups}
    last = {}                               # group -> last leader seen
    for i, (t, s) in enumerate(samples):
        nxt = samples[i + 1][0] if i + 1 < len(samples) else end
        dt = max(0.0, nxt - t)
        total += dt
        for g in groups:
            n = s.get(g, 0)
            per[g][n] = per[g].get(n, 0.0) + dt
            if n:
                if g in last and last[g] != n:
                    changes[g] += 1
                last[g] = n
        led = [s.get(g, 0) for g in groups]
        if all(led):
            all_led += dt
            k = len(set(led))
            nodes_held[k] = nodes_held.get(k, 0.0) + dt
            if k == 1:
                together += dt
    share = (lambda x: x / total if total else 0.0)
    return {
        'seconds': total, 'samples': len(samples), 'groups': groups,
        'by_group': {g: {n: share(v) for n, v in sorted(per[g].items())} for g in groups},
        'all_led': share(all_led),
        'together': share(together),
        'together_of_led': together / all_led if all_led else 0.0,
        'distinct_nodes': {k: share(v) for k, v in sorted(nodes_held.items())},
        'changes': changes,
    }


def format_report(summary, members=None, events=None, label=''):
    """Markdown tables of a summary (shares in percent of the run)."""
    groups = summary['groups']
    nodes = sorted({n for g in groups for n in summary['by_group'][g] if n} |
                   set(members or ()))
    out = []
    if label:
        out.append(f'**{label}**: {summary["seconds"]:.0f} s, {summary["samples"]} samples')
        out.append('')
    out.append('| Node | ' + ' | '.join(f'leads group {g}' for g in groups) + ' |')
    out.append('| --- |' + ' ---: |' * len(groups))
    for n in nodes:
        out.append(f'| {n} | ' + ' | '.join(f'{100 * summary["by_group"][g].get(n, 0.0):.1f} %'
                                            for g in groups) + ' |')
    out.append('| no leader | ' + ' | '.join(f'{100 * summary["by_group"][g].get(0, 0.0):.1f} %'
                                             for g in groups) + ' |')
    out.append('| leader changes | ' + ' | '.join(str(summary['changes'][g]) for g in groups) + ' |')
    out.append('')
    out.append(f'Every group led: {100 * summary["all_led"]:.1f} % of the run. '
               f'All leaders on one node: {100 * summary["together"]:.1f} % of the run '
               f'({100 * summary["together_of_led"]:.1f} % of the time every group was led).')
    if events:
        ok = {}
        for e in events:
            key = (e['kind'], e['rc'] == 0)
            ok[key] = ok.get(key, 0) + 1
        parts = [f'{k} {"ok" if good else "refused/failed"}: {v}' for (k, good), v in sorted(ok.items())]
        out.append('Events: ' + ', '.join(parts) + '.')
    return '\n'.join(out)


def schedule(stepdowns, restarts):
    """Event kinds in run order, each kind spread evenly and interleaved."""
    pos = [((j + 0.5) / stepdowns, 0, 'stepdown') for j in range(stepdowns)]
    pos += [((k + 0.5) / restarts, 1, 'restart') for k in range(restarts)]
    return [kind for _, _, kind in sorted(pos)]


# ---- live run ---------------------------------------------------------------

def _mgmt(mgmt, *args, timeout=20):
    try:
        p = subprocess.run([mgmt, *args], capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, 'timeout'


def sample(mgmt, members, groups=None):
    """One sample: raft-status of every up member in parallel."""
    res = {}

    def one(nid, ep):
        rc, out = _mgmt(mgmt, 'raft-status', ep, timeout=10)
        if rc == 0:
            try:
                res[nid] = parse_raft_status(out)
            except ValueError:
                pass
    th = [threading.Thread(target=one, args=(n, ep)) for n, (ep, up) in members.items() if up]
    for t in th:
        t.start()
    for t in th:
        t.join()
    return leaders_of(list(res.values()), groups), sorted(res)


def shell_restart(cmd):
    """A restart hook from --restart-cmd: (action, node, ep) -> exit status."""
    return lambda action, nid, ep: subprocess.run(
        cmd.format(node=nid, ep=shlex.quote(ep), action=action), shell=True).returncode


def run(a, restart=None):
    """restart: (action 'stop'|'start', node id, ep) -> status, or None."""
    mgmt = a.mgmt
    rc, out = _mgmt(mgmt, 'status', a.seed)
    members = parse_members(out)
    if restart is None and getattr(a, 'restart_cmd', None):
        restart = shell_restart(a.restart_cmd)
    if a.restarts and restart is None:
        sys.exit('--restarts needs --restart-cmd')
    events = []
    order = schedule(a.stepdowns, a.restarts)
    n_ev = len(order)
    t0 = time.monotonic()
    due = [t0 + a.duration * (i + 1) / (n_ev + 1) for i in range(n_ev)]
    samples, reached = [], []
    groups = None
    turn = 0
    restart_pending = None   # (node, ep, start_at)
    while True:
        now = time.monotonic()
        if now - t0 >= a.duration:
            break
        rc, out = _mgmt(mgmt, 'status', a.seed)
        try:
            members = parse_members(out)
        except ValueError:
            pass
        led, got = sample(mgmt, members, groups)
        if groups is None and led:
            groups = sorted(led)
        samples.append((now - t0, led))
        reached.append(len(got))
        if restart_pending and time.monotonic() >= restart_pending[2]:
            nid, ep, _ = restart_pending
            rc = restart('start', nid, ep)
            events.append({'t': time.monotonic() - t0, 'kind': 'restart-start', 'node': nid, 'rc': rc})
            restart_pending = None
        if order and time.monotonic() >= due[n_ev - len(order)] and not restart_pending:
            kind = order.pop(0)
            g = groups[turn % len(groups)] if groups else 0
            turn += 1
            nid = led.get(g, 0)
            t = time.monotonic() - t0
            if not nid or nid not in members:
                events.append({'t': t, 'kind': kind, 'group': g, 'node': 0, 'rc': -1,
                               'why': 'no leader'})
            elif kind == 'stepdown':
                rc, o = _mgmt(mgmt, 'raft-step-down', members[nid][0], str(g))
                events.append({'t': t, 'kind': kind, 'group': g, 'node': nid, 'rc': rc,
                               'out': o.strip()[-200:]})
            else:
                ep = members[nid][0]
                rc = restart('stop', nid, ep)
                events.append({'t': t, 'kind': 'restart-stop', 'group': g, 'node': nid, 'rc': rc})
                restart_pending = (nid, ep, time.monotonic() + a.down)
        time.sleep(max(0.0, a.interval - (time.monotonic() - now)))
    if restart_pending:
        nid, ep, _ = restart_pending
        rc = restart('start', nid, ep)
        events.append({'t': time.monotonic() - t0, 'kind': 'restart-start', 'node': nid, 'rc': rc})
    end = time.monotonic() - t0
    summary = summarize(samples, end)
    rec = {'label': a.label, 'seed': a.seed, 'members': {str(k): v[0] for k, v in members.items()},
           'duration': a.duration, 'interval': a.interval, 'stepdowns': a.stepdowns,
           'restarts': a.restarts, 'down': a.down, 'events': events,
           'samples': [[t, {str(g): n for g, n in s.items()}] for t, s in samples],
           'reached': reached, 'end': end}
    if a.json:
        Path(a.json).write_text(json.dumps(rec, indent=1) + '\n')
    print(format_report(summary, sorted(members), events, a.label))
    return rec


def load(path):
    rec = json.loads(Path(path).read_text())
    samples = [(t, {int(g): n for g, n in s.items()}) for t, s in rec['samples']]
    return rec, summarize(samples, rec['end'])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='measure a live cluster')
    r.add_argument('--seed', required=True, help='ADDR:PORT of any member')
    r.add_argument('--duration', type=float, default=600.0, help='seconds (default 600)')
    r.add_argument('--interval', type=float, default=0.5, help='sample period, s (default 0.5)')
    r.add_argument('--stepdowns', type=int, default=0)
    r.add_argument('--restarts', type=int, default=0)
    r.add_argument('--down', type=float, default=5.0, help='seconds a restarted member stays down')
    r.add_argument('--restart-cmd', help='shell command; {node} {ep} {action}=stop|start')
    r.add_argument('--mgmt', default=str(ROOT / 'efs-mgmt'))
    r.add_argument('--json', help='write the run (samples, events) here')
    r.add_argument('--label', default='')
    p = sub.add_parser('report', help='tables of a recorded run')
    p.add_argument('json')
    a = ap.parse_args(argv)
    if a.cmd == 'run':
        run(a)
    else:
        rec, s = load(a.json)
        print(format_report(s, sorted(int(k) for k in rec['members']), rec['events'], rec['label']))


if __name__ == '__main__':
    main()
