#!/usr/bin/env python3
"""Stage 2 qualification driver (alpha 1.0: A1-A5, C1-C5, D1-D5, E1-E3, F3).

One command runs the Stage 2 gates against a described cluster and leaves,
outside Git, everything an evidence summary needs:

    qualify.py run --cluster CLUSTER.json --out DIR [--gates a,b] [--skip a,b]
                   [--allow restart,turnover,remount|all] [--deadline GATE=SEC]
                   [--plugin NAME=COMMAND] [--work DIR] [--plan]
    qualify.py run --loopback 3 --out DIR --work DIR [--port-base 18740] ...
    qualify.py run --matrix scripts/qualify_matrix.json --topology NAME --out DIR
                   [--user-approved] [--ignore-blocked]   one A11 topology, its defaults
    qualify.py plan [--matrix FILE] [--topology NAME] [--check]
                                          validate the A11 matrix; print each topology's command
    qualify.py list                       gates, checklist items, destructive needs
    qualify.py verify-sums DIR            recheck DIR/SHA256SUMS (exit 1 on mismatch)

For every gate DIR/gates/NAME/result.json records each exact command, its
host, start and end times, deadline and exit status, the gate verdict (PASS,
FAIL or NOTRUN with the reason), the raw output files and the fixture counts
and sizes. DIR/identity.json holds the build identity of every daemon
(`efs-mgmt version`), every mounted client (the live efs-fuse process: its
executable, sha256 and `--version`) and the harness tree, read at the start
and again at the end of the run. DIR/SHA256SUMS covers every raw file; then
the driver writes DIR/summary-draft.md (the AGENTS.md evidence shape) and
DIR/f3-row.md (this topology's column of the F3 ledger). Exit status: 0 all
selected gates PASS, 1 any FAIL, 3 no FAIL but some NOTRUN, 2 usage error.

Safety. The driver never formats, wipes or stops a cluster it did not
start. Destructive steps run only with --allow and only through commands the
cluster description names: `restart` (orderly whole-cluster stop/start for
E1, a member stop/start for E3 catch-up), `turnover` (raft-step-down for E3)
and `remount` (client stop without force and remount: durability, E2).
Every performed step is listed in run.json and the summary. --loopback starts
a private cluster (127.0.0.1, ports PORT_BASE..+9, stores under --work) that
the driver owns and removes at the end. The live harnesses always start
their own private clusters on cluster.private ports. Reserved ports
(17432-17435, 27432, 27433) and stores under /data1/efs are refused.

Cluster description (JSON; command templates run under bash on the named
host, through ssh unless it is this host; {placeholders} are substituted
shell-quoted, so never quote them in the template):

    {"name": "intel-3", "candidate": "<commit>",
     "source": "/path/to/built/tree",        # same path on every host, or
     "sources": {"host": "/path"},           # per-host trees
     "seed": "10.0.0.1:17432",
     "nodes": [{"addr": "10.0.0.1:17432", "host": "h1"}, ...],
     "mounts": [{"path": "/mnt/a", "host": "h1"}, {"path": "/mnt/b", "host": "h2"}],
     "transport": "rdma",                    # fragment I/O is O_DIRECT only (D42)
     "features": {"packing": true, "efs_commands": true, "expiry": false},
     "quota": "...", "hardware": "...", "notes": "...",
     "mgmt_host": "local",                   # where efs-mgmt reaches the nodes
     "private": {"host": "local", "root": "/home/u/qual-live", "port_base": 18700},
     "fixture": {"e1_files": 240, "e1_sizes": [0, 1, 4095, 4096, 65537, 1048576],
                 "posix_jobs": 4, "posix_timeout_s": 30, "e3_seconds": 60, ...},
     "control": {"stop_cluster": "...", "start_cluster": "...",
                 "stop_node": "... {addr} {host} {node}", "start_node": "...",
                 "client_stop": "cd {source} && scripts/client.sh stop {mount}",
                 "client_start": "cd {source} && scripts/client.sh {seed} {mount}",
                 "stop_timeout_s": 300, "ready_timeout_s": 300},
     "plugins": {"b-checks": {"command": "...", "deadline_s": 3600,
                              "host": "local", "needs": [], "items": "B1-B6"}},
     "ssh": "scripts/orcd_mini_ssh.sh",       # optional: PROG HOST COMMAND replaces ssh
     "private_cluster": {"root": "/home/u/efs-qual", "quota": "4G", "env": {}}}

With private_cluster the driver owns the cluster: every node names its
"store" (and every mount its path) under that root on its host; the driver
starts one efsd per node there (`_node start`: the first node is the seed,
the others --join it), mounts the clients, and at the end unmounts, stops
those daemons and deletes the root on every host (unless --keep-work).
Every gate the A11 plan names is a real gate (no stubs); the A13, C5, D1,
D3 and E4 gates run their host-side work as scripts/stage2_helpers.py.

Plugins (the B checks and anything later) are commands run with the same
placeholders plus {cluster} (the description copy in DIR) and {gate_dir}.
Exit 0 is PASS, 77 NOTRUN, anything else FAIL. A plugin may also write
{gate_dir}/plugin-result.json ({"verdict", "reason", "fixture"}); it can
only lower a verdict, never turn a failing exit into a PASS.
"""
import argparse, datetime, hashlib, json, os, random, re, shlex, signal, socket
import subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESERVED_PORTS = {17432, 17433, 17434, 17435, 27432, 27433}
DESTRUCTIVE = ('restart', 'turnover', 'remount')
NOTRUN_RC = 77
PASS, FAIL, NOTRUN = 'PASS', 'FAIL', 'NOTRUN'
RANK = {PASS: 0, NOTRUN: 1, FAIL: 2}
LEDGER_ROWS = ('Candidate and profile', 'Placement/capacity and decode',
               'POSIX and concurrent I/O', 'Packing/shared-fragment GC',
               'Enabled `.efs` under load', 'Full payloads + orderly restart',
               'Scale/progress evidence (E4, D4)')
DEFAULT_FIXTURE = dict(
    e1_files=240, e1_sizes=[0, 1, 4095, 4096, 4097, 65536, 65537, 1048576, 4194305],
    posix_jobs=4, posix_timeout_s=30, e3_seconds=60, e3_file_kib=64,
    turnover_timeout_s=60, catchup_timeout_s=180,
    append_failover_args='--records 8000 --writers 4 --failover step-down --interval 4 --first-event 3',
    k14_phases='1 2 3 7', gc_args='--posix', remote_tmp='/tmp',
    b6_pack_wait_s=600, b6_rebuild_s=900, b6_read_s=30, b6_write_s=90, b6_fsync_s=75,
    a13_members=5, a13_quota='2G', a13_port_offset=100, a13_files=60, a13_threads=4, a13_op_deadline_s=30,
    a13_join_timeout_s=180, a13_interrupt_s=0.1,
    c5_fence_attempts=12,
    d1_files=400, d1_max_size=8192, d1_pack_wait_s=600, d1_compact_wait_s=300,
    d3_static_files=200, d3_writers=8, d3_queriers=4, d3_load_s=300, d3_io_deadline_s=10,
    d3_query_deadline_s=30, d3_index_wait_s=600,
    e4_small_per_member=5000, e4_large_per_member=2, e4_large_mib=256, e4_threads=16,
    e4_query_deadline_s=30, e4_refresh_s=60, e4_reclaim_s=900, soak_hours=4, soak_profile='endurance')


def delete_command(path):
    """Use the host's explicit tree-deletion tool; never substitute rm."""
    quoted = shlex.quote(str(path))
    return (f'export PATH="$HOME/bin:$PATH"; test -e {quoted} || exit 0; '
            f'edelete --force --delete {quoted}')


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds')


def worst(verdicts):
    verdicts = list(verdicts)
    return max(verdicts, key=RANK.get) if verdicts else NOTRUN


def identity_key(version):
    """The comparable part of a version line, as efs-mgmt status compares it
    (status_version_identity): drop the program name and the ", built ..."
    tail, keep commit, -dirty and branch."""
    text = version.strip().splitlines()[0] if version.strip() else ''
    text = re.sub(r'^\S+\s+', '', text, count=1)
    return re.sub(r', built [^)]*\)', ')', text)


def identity_change_reason(start, end):
    """The final read is evidence only if it still matches the frozen run."""
    if not start:
        return 'starting build identity was not recorded'
    for role in ('daemons', 'clients', 'harness'):
        before = start.get(role, [])
        if not before:
            continue
        after = end.get(role, [])
        if len(before) != len(after):
            return f'{role}: identity count changed ({len(before)} -> {len(after)})'
        if any(not e.get('key') for e in after):
            return f'{role}: final build identity unreadable'
        if any(e.get('exe_deleted') for e in after):
            return f'{role}: final executable was replaced'
        if sorted(e.get('key') or '' for e in before) != sorted(e['key'] for e in after):
            return f'{role}: build identity changed at the end of the run'
    return ''


def is_local(host):
    if host in (None, '', 'local', 'localhost', '127.0.0.1'):
        return True
    name = socket.gethostname()
    return host in (name, name.split('.')[0])


def ssh_program(ssh):
    return ssh if os.path.isabs(ssh) else str(ROOT / ssh)


def fill(template, values):
    return template.format(**{k: shlex.quote(str(v)) for k, v in values.items()})


def inside_git(path):
    for parent in [path, *path.parents]:
        if (parent / '.git').exists():
            return parent
    return None


class Runner:
    """Runs commands on hosts with a deadline; local ones in their own
    process group (killed on expiry), remote ones under timeout(1) via ssh."""

    def __init__(self, env=None, ssh=None):
        self.env = env or {}
        self.ssh = ssh  # PROG called as `PROG HOST COMMAND` in place of ssh (jump-host wrappers)

    def argv(self, host, cmd, deadline):
        exports = ''.join(f'export {k}={shlex.quote(str(v))}; ' for k, v in self.env.items())
        cmd = exports + cmd
        if is_local(host):
            return ['bash', '-c', cmd]
        wrapped = f'timeout -k 30 {int(deadline) + 1} bash -c {shlex.quote(cmd)}' if deadline else \
            f'bash -c {shlex.quote(cmd)}'
        if self.ssh:
            return [ssh_program(self.ssh), host, wrapped]
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20',
                '-o', 'ServerAliveInterval=30', host, wrapped]

    def run(self, host, cmd, deadline, log_path, stdin_path=None):
        argv = self.argv(host, cmd, deadline)
        backstop = None if deadline is None else deadline + (0 if is_local(host) else 60)
        with open(log_path, 'wb') as log, \
                open(stdin_path or os.devnull, 'rb') as stdin:
            p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, stdin=stdin,
                                 start_new_session=True)
            timed_out = False
            try:
                rc = p.wait(timeout=backstop)
            except subprocess.TimeoutExpired:
                timed_out = True
                for sig, grace in ((signal.SIGTERM, 10), (signal.SIGKILL, 10)):
                    try:
                        os.killpg(p.pid, sig)
                    except ProcessLookupError:
                        pass
                    try:
                        p.wait(timeout=grace)
                        break
                    except subprocess.TimeoutExpired:
                        continue
                rc = 124
        if rc == 124 and not is_local(host):
            timed_out = True
        return rc, timed_out


class Gate:
    def __init__(self, name, fn, items, ledger, needs=(), deadline_s=3600, partial_needs=(), doc=''):
        self.name, self.fn, self.items, self.ledger = name, fn, items, ledger
        self.needs, self.partial_needs = tuple(needs), tuple(partial_needs)
        self.deadline_s, self.doc = deadline_s, doc


class GateRun:
    """Bookkeeping for one gate: commands, raw files, fixture, steps."""

    def __init__(self, q, gate, deadline_s):
        self.q, self.gate, self.deadline_s = q, gate, deadline_s
        self.dir = q.out / 'gates' / gate.name
        self.dir.mkdir(parents=True)
        self.commands, self.fixture, self.steps, self.raw = [], {}, [], []
        self.start = time.monotonic()
        self.record = dict(gate=gate.name, items=gate.items, ledger=gate.ledger,
                           needs=list(gate.needs), deadline_s=deadline_s, start=now())

    def remaining(self):
        return max(1.0, self.deadline_s - (time.monotonic() - self.start))

    def raw_file(self, name):
        path = self.dir / name
        self.raw.append(name)
        return path

    def run(self, label, host, cmd, deadline=None, stdin_path=None):
        index = len(self.commands) + 1
        log = self.raw_file(f'{index:02d}-{label}.log')
        deadline = min(deadline or self.remaining(), self.remaining())
        entry = dict(label=label, host=host or 'local', command=cmd, deadline_s=round(deadline, 1),
                     start=now(), output=log.name)
        if self.q.plan:
            entry.update(end=entry['start'], rc=None, timed_out=False)
            self.commands.append(entry)
            return 0, ''
        t0 = time.monotonic()
        rc, timed_out = self.q.runner.run(host, cmd, deadline, log, stdin_path)
        entry.update(end=now(), seconds=round(time.monotonic() - t0, 3), rc=rc, timed_out=timed_out)
        self.commands.append(entry)
        return rc, log.read_text(errors='replace')

    def step(self, kind, what):
        """A destructive step this gate performed (listed in the summary)."""
        self.steps.append(dict(kind=kind, what=what, at=now()))

    def fetch(self, host, path, name):
        """Copy a remote (or local) file into the gate's raw outputs."""
        dest = self.raw_file(name)
        if self.q.plan:
            return dest
        cmd = f'cat {shlex.quote(path)}'
        with open(dest, 'wb') as out:
            subprocess.run(self.q.runner.argv(host, cmd, 120), stdout=out,
                           stderr=subprocess.DEVNULL, timeout=180)
        return dest


def outcome_from_rc(rc, text, timed_out=False):
    if timed_out:
        return FAIL, 'deadline exceeded'
    marks = [line.strip() for line in text.splitlines() if line.strip()]
    if rc == 0:
        return PASS, ''
    if rc == NOTRUN_RC:
        notrun = [m for m in marks if 'NOTRUN' in m or 'SKIP' in m]
        return NOTRUN, (notrun or marks or ['exit 77'])[-1][:300]
    failing = [m for m in marks if re.search(r'\bFAIL|Error|error:|Traceback|assert', m)]
    return FAIL, f'exit {rc}: ' + (failing or marks or ['no output'])[-1][:300]


def count_marks(text):
    counts = {}
    for line in text.splitlines():
        fields = line.split('\t')[:3] if '\t' in line else line.split()[:1] + line.split()[-1:]
        for field in fields:
            field = field.strip().rstrip(':')
            if field in ('PASS', 'FAIL', 'SKIP', 'NOTRUN', 'XFAIL'):
                counts[field] = counts.get(field, 0) + 1
                break
    return counts


# --------------------------------------------------------------------------
# Controllers: who may stop and start what.

class Controller:
    owned = False

    def __init__(self, q):
        self.q = q
        self.c = q.cluster.get('control', {})

    def has(self, *names):
        return all(self.c.get(n) for n in names)

    def values(self, **extra):
        q = self.q
        return dict(seed=q.cluster.get('seed', ''), source=q.source(None), **extra)

    def cmd(self, g, name, host=None, **extra):
        cmd = fill(self.c[name], self.values(**extra))
        if name == 'client_stop' and 'force' in cmd:
            raise SystemExit('control.client_stop must not force (E2 needs a clean stop)')
        deadline = self.c.get('stop_timeout_s', 300) if 'stop' in name else \
            self.c.get('ready_timeout_s', 300)
        return g.run(name, host, cmd, deadline)

    def client_stop(self, g, m):
        values = dict(mount=m['path'], source=self.q.source(m['host']), host=m['host'])
        cmd = fill(self.c.get('client_stop') or 'cd {source} && scripts/client.sh stop {mount}',
                   self.values(**values))
        if 'force' in cmd:
            raise SystemExit('control.client_stop must not force (E2 needs a clean stop)')
        return g.run('client-stop', m['host'], cmd, self.c.get('stop_timeout_s', 300))

    def client_start(self, g, m):
        values = dict(mount=m['path'], source=self.q.source(m['host']), host=m['host'])
        cmd = fill(self.c.get('client_start') or 'cd {source} && scripts/client.sh {seed} {mount}',
                   self.values(**values))
        return g.run('client-start', m['host'], cmd, self.c.get('ready_timeout_s', 300))

    def stop_cluster(self, g):
        return self.cmd(g, 'stop_cluster', self.q.cluster.get('control_host'))

    def start_cluster(self, g):
        return self.cmd(g, 'start_cluster', self.q.cluster.get('control_host'))

    def can_restart_nodes(self):
        return self.has('stop_node', 'start_node')

    def stop_node(self, g, node):
        return self.cmd(g, 'stop_node', node.get('host'), addr=node['addr'],
                        host=node.get('host', ''), node=node.get('id', ''))

    def start_node(self, g, node):
        return self.cmd(g, 'start_node', node.get('host'), addr=node['addr'],
                        host=node.get('host', ''), node=node.get('id', ''))


class Loopback(Controller):
    """A private cluster the driver starts, owns and removes: N efsd on
    127.0.0.1 (K33 positional CLI, seed + --join) and two client.sh mounts."""
    owned = True

    def __init__(self, args, work):
        self.n, self.work, self.quota = args.loopback, work, args.loopback_quota
        self.candidate = getattr(args, "candidate", None)
        self.base = args.port_base
        ports = list(range(self.base, self.base + self.n))
        if any(p in RESERVED_PORTS for p in ports + [self.base + 9]) or not 1024 < self.base < 65000:
            raise SystemExit(f'--port-base {self.base}: reserved or invalid ports')
        if str(work.resolve()).startswith('/data1/efs'):
            raise SystemExit('loopback stores may not live under /data1/efs')
        for port in ([] if args.plan else ports):
            with socket.socket() as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    s.bind(('127.0.0.1', port))
                except OSError:
                    raise SystemExit(f'loopback port {port} is in use')
        self.addrs = [f'127.0.0.1:{p}' for p in ports]
        self.stores = [work / 'loopback' / f'n{i + 1}' for i in range(self.n)]
        self.mounts = [work / 'loopback' / 'mnt-a', work / 'loopback' / 'mnt-b']
        self.procs, self.c = {}, {}
        # .efs follows the cluster commands-policy, enabled by default (D42).
        self.env = dict(os.environ, EFS_TRANSPORT='tcp', TMPDIR=str(work / 'tmp'))

    def description(self, private_base):
        return dict(name=f'loopback-{self.n}', candidate=self.candidate, source=str(ROOT),
                    seed=self.addrs[0], owned=True,
                    nodes=[dict(addr=a, host='local', id=i + 1) for i, a in enumerate(self.addrs)],
                    mounts=[dict(path=str(m), host='local') for m in self.mounts],
                    transport='tcp',
                    features=dict(efs_commands='cluster commands-policy (default: enabled)',
                                  packing='build default (not set by the driver)'),
                    quota=f'{self.quota} per daemon', hardware=f'{socket.gethostname()} (one host, loopback)',
                    private=dict(host='local', root=str(self.work / 'live'), port_base=private_base),
                    notes='driver-owned loopback dry-run cluster; not distinct hosts (A4)')

    def start_daemon(self, i, join):
        self.stores[i].mkdir(parents=True, exist_ok=True)
        if join and i:
            argv = [str(ROOT / 'efsd'), '--join', self.addrs[0], '--listen', self.addrs[i],
                    str(self.stores[i])]
        else:
            argv = [str(ROOT / 'efsd'), self.addrs[i], str(self.stores[i])]
        argv += ['--quota', self.quota, '--discovery-port', '0']
        log = open(self.work / 'loopback' / f'efsd-n{i + 1}.log', 'ab')
        log.write(f'\n==== {now()} {" ".join(argv)}\n'.encode())
        log.flush()
        self.procs[i] = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=self.env)

    def mgmt(self, *args, timeout=30):
        try:
            return subprocess.run([str(ROOT / 'efs-mgmt'), *map(str, args)], capture_output=True,
                                  text=True, env=self.env, timeout=timeout)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(args, 124, '', 'timeout')

    def wait_ready(self, deadline=180):
        end = time.time() + deadline
        while time.time() < end:
            if all(self.mgmt('raft-getattr', a, 1).returncode == 0 for a in self.addrs):
                return True
            for i, p in self.procs.items():
                if p.poll() is not None:
                    raise RuntimeError(f'loopback efsd n{i + 1} exited rc={p.returncode}')
            time.sleep(1)
        return False

    def mount(self, m):
        m.mkdir(parents=True, exist_ok=True)
        return subprocess.run(['scripts/client.sh', self.addrs[0], str(m)], cwd=ROOT, env=self.env,
                              capture_output=True, text=True, timeout=120)

    def start(self):
        (self.work / 'loopback').mkdir(parents=True)
        (self.work / 'tmp').mkdir(exist_ok=True)
        for i in range(self.n):
            self.start_daemon(i, join=True)
            time.sleep(1)
        if not self.wait_ready():
            raise RuntimeError('loopback cluster never became ready')
        for m in self.mounts:
            r = self.mount(m)
            if r.returncode:
                raise RuntimeError(f'loopback mount {m} failed: {r.stdout}{r.stderr}')

    def save_logs(self, out):
        """The owned daemons' and clients' logs are raw outputs of the run."""
        dest = out / 'loopback'
        dest.mkdir(exist_ok=True)
        for path in sorted((self.work / 'loopback').glob('*.log')):
            dest.joinpath(path.name).write_bytes(path.read_bytes())

    def stop_daemon(self, i, timeout=120):
        p = self.procs.get(i)
        if p is None or p.poll() is not None:
            return 0
        p.terminate()
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return 124

    def teardown(self):
        for m in self.mounts:
            if os.path.ismount(m):
                r = subprocess.run(['scripts/client.sh', 'stop', str(m)], cwd=ROOT, env=self.env,
                                   capture_output=True, timeout=180)
                if r.returncode and os.path.ismount(m):
                    subprocess.run(['fusermount3', '-uz', str(m)], capture_output=True)
        for i in list(self.procs):
            self.stop_daemon(i)

    # Controller interface, implemented in-process and logged like commands.
    def has(self, *names):
        return True

    def can_restart_nodes(self):
        return True

    def _logged(self, g, label, fn, deadline=300):
        entry = dict(label=label, host='local', command=f'(driver-owned loopback) {label}',
                     deadline_s=deadline, start=now(), output=None)
        t0 = time.monotonic()
        try:
            ok = fn()
            rc = 0 if ok is None or ok is True or (type(ok) is int and ok == 0) else 1
        except Exception as exc:  # recorded as the step's failure
            rc, ok = 1, exc
        entry.update(end=now(), seconds=round(time.monotonic() - t0, 3), rc=rc, timed_out=False)
        if rc:
            entry['error'] = str(ok)[:500]
        g.commands.append(entry)
        return rc, entry.get('error', '')

    def client_stop(self, g, m):
        return g.run('client-stop', 'local', f'cd {shlex.quote(str(ROOT))} && '
                     f'scripts/client.sh stop {shlex.quote(m["path"])}', 300)

    def client_start(self, g, m):
        return g.run('client-start', 'local', f'cd {shlex.quote(str(ROOT))} && export '
                     f'EFS_TRANSPORT=tcp && scripts/client.sh {self.addrs[0]} {shlex.quote(m["path"])}',
                     300)

    def stop_cluster(self, g):
        def stop():
            # A failed stop must not short-circuit retirement of other owned nodes.
            results = [self.stop_daemon(i) for i in sorted(self.procs)]
            return all(rc == 0 for rc in results)
        return self._logged(g, 'stop-cluster', stop)

    def start_cluster(self, g):
        def start():
            for i in range(self.n):
                self.start_daemon(i, join=False)
            return self.wait_ready()
        return self._logged(g, 'start-cluster', start)

    def index(self, node):
        return self.addrs.index(node['addr'])

    def stop_node(self, g, node):
        return self._logged(g, f'stop-node-{node["addr"]}',
                            lambda: self.stop_daemon(self.index(node)) == 0)

    def start_node(self, g, node):
        return self._logged(g, f'start-node-{node["addr"]}',
                            lambda: self.start_daemon(self.index(node), join=False))


class PrivateCluster(Controller):
    """A cluster the driver starts on the described hosts and removes at the
    end (`private_cluster` in the description): one efsd per node from that
    host's tree (`_node start`), the first node the seed and the others
    `--join` it, then a client.sh mount per mount; restarts use the stores'
    persisted identity. Every store and mount must lie under
    private_cluster.root on its host (never /data1/efs); teardown unmounts,
    stops the daemons it started and deletes that root on every host."""
    owned = True

    def __init__(self, q):
        super().__init__(q)
        self.pc = q.cluster['private_cluster']
        self.nodes = q.cluster.get('nodes', [])

    def has(self, *names):
        return True

    def can_restart_nodes(self):
        return True

    def env_args(self):
        env = dict(EFS_TRANSPORT=self.q.cluster.get('transport', 'tcp'), **self.pc.get('env', {}))
        return [f'--env={k}={v}' for k, v in sorted(env.items())]

    def node_cmd(self, node, op, join=False):
        args = ['_node', op, '--addr', node['addr'], '--store', node['store']]
        if op == 'start':
            args += ['--quota', self.pc.get('quota', '4G'), *self.env_args()]
            if join:
                args += ['--join', self.q.cluster['seed']]
        return self.q.helper(node['host'], *args)

    def start_node(self, g, node, join=False):
        return g.run(f'start-node-{node["addr"]}', node['host'], self.node_cmd(node, 'start', join), 300)

    def stop_node(self, g, node):
        return g.run(f'stop-node-{node["addr"]}', node['host'], self.node_cmd(node, 'stop'), 300)

    def start_cluster(self, g, join=False):
        for i, node in enumerate(self.nodes):
            rc, text = self.start_node(g, node, join=join and i > 0)
            if rc:
                return rc, text
        if not wait_cluster_ready(self.q, g, self.pc.get('ready_timeout_s', 300)):
            return 1, 'cluster not ready (raft-getattr on every node)'
        return 0, ''

    def stop_cluster(self, g):
        worst_rc, texts = 0, []
        for node in reversed(self.nodes):
            rc, text = self.stop_node(g, node)
            worst_rc, texts = worst_rc or rc, texts + [text]
        return worst_rc, '\n'.join(texts)

    def client_env(self):
        return ' '.join(f'export {a[6:].split("=", 1)[0]}={shlex.quote(a[6:].split("=", 1)[1])};'
                        for a in self.env_args())

    def client_start(self, g, m):
        src = shlex.quote(self.q.source(m['host']))
        return g.run('client-start', m['host'], f'mkdir -p {shlex.quote(m["path"])} && cd {src} && '
                     f'{self.client_env()} scripts/client.sh {shlex.quote(self.q.cluster["seed"])} '
                     f'{shlex.quote(m["path"])}', 300)

    def client_stop(self, g, m):
        return g.run('client-stop', m['host'], f'cd {shlex.quote(self.q.source(m["host"]))} && '
                     f'scripts/client.sh stop {shlex.quote(m["path"])}', 300)

    def owned_setup(self, g):
        g.step('format', 'driver-owned private cluster: fresh stores, seed + --join formation')
        rc, text = self.start_cluster(g, join=True)
        if rc:
            return rc, text
        for m in self.q.cluster.get('mounts', []):
            rc, text = self.client_start(g, m)
            if rc:
                return rc, f'mount {m["host"]}:{m["path"]} failed: ' + text.strip()[-300:]
        return 0, ''

    def owned_teardown(self, g):
        for m in self.q.cluster.get('mounts', []):
            path = shlex.quote(m['path'])
            g.run('teardown-unmount', m['host'], f'mountpoint -q {path} || exit 0; '
                  f'cd {shlex.quote(self.q.source(m["host"]))} && scripts/client.sh stop {path} || '
                  f'fusermount3 -uz {path}', 300)
        self.stop_cluster(g)
        # The daemons' and clients' logs are raw outputs; the stores are not.
        for n in self.nodes:
            store = Path(n['store'])
            g.fetch(n['host'], f'{store.parent}/{store.name}.log', f'efsd-{n["addr"].replace(":", "-")}.log')
        for m in self.q.cluster.get('mounts', []):
            mount = Path(m['path'])
            g.fetch(m['host'], f'{mount.parent}/efs-fuse-{mount.name}.log', f'efs-fuse-{m["host"]}-{mount.name}.log')
        root = self.pc['root']
        for host in sorted({n['host'] for n in self.nodes} | {m['host'] for m in self.q.cluster.get('mounts', [])}):
            g.run('teardown-delete', host, delete_command(root), 900)


def check_private_cluster(cluster):
    """Refusals for a driver-owned multi-host cluster; returns error strings."""
    pc, errors = cluster.get('private_cluster') or {}, []
    root = pc.get('root', '')
    if not root.startswith('/') or root.rstrip('/').count('/') < 2 or root.startswith('/data1/efs'):
        errors.append(f'private_cluster.root {root!r} must be an absolute work directory outside /data1/efs')
    for kind, entries, key in (('node', cluster.get('nodes', []), 'store'), ('mount', cluster.get('mounts', []), 'path')):
        for e in entries:
            path = e.get(key, '')
            if not path.startswith(root.rstrip('/') + '/'):
                errors.append(f'{kind} {e.get("addr") or path!r}: {key} {path!r} is not under private_cluster.root')
    for n in cluster.get('nodes', []):
        port = int(n.get('addr', ':0').rsplit(':', 1)[-1] or 0)
        if port in RESERVED_PORTS:
            errors.append(f'node {n["addr"]}: reserved port')
    if cluster.get('nodes') and cluster.get('seed') != cluster['nodes'][0].get('addr'):
        errors.append('private_cluster: seed must be the first node')
    return errors


# --------------------------------------------------------------------------
# The qualification run.

class Qualify:
    def __init__(self, args, cluster, out, work, controller, plugins):
        self.args, self.cluster, self.out, self.work = args, cluster, out, work
        self.allow = set(args.allow)
        self.plan = args.plan
        self.fixture = dict(DEFAULT_FIXTURE, **cluster.get('fixture', {}))
        self.private = dict(dict(host='local', root=str(work / 'live'), port_base=18700),
                            **cluster.get('private', {}))
        self.runid = out.name
        self.runner = Runner(ssh=cluster.get('ssh'))
        self.control = controller
        self.plugins = plugins
        self.scratch = {}
        self.identity_start = None
        self.before_sums = None

    def source(self, host):
        return self.cluster.get('sources', {}).get(host or 'local', self.cluster.get('source', str(ROOT)))

    def mount(self, i):
        mounts = self.cluster.get('mounts', [])
        return mounts[i] if i < len(mounts) else None

    def remote_dir(self, g, host, name):
        """Per-run scratch on HOST (raw files are fetched into DIR)."""
        if is_local(host):
            base = self.work / 'scratch'
        else:
            base = Path(self.fixture['remote_tmp']) / f'qualify-{self.runid}'
        self.scratch[host or 'local'] = str(base)
        path = f'{base}/{name}'
        g.run('mkdir-scratch', host, f'mkdir -p {shlex.quote(path)}', 60)
        return path

    def helper(self, host, *args):
        return ' '.join(['python3', shlex.quote(f'{self.source(host)}/scripts/qualify.py'),
                         *map(shlex.quote, map(str, args))])

    def mgmt_cmd(self, *args):
        return ' '.join([shlex.quote(f'{self.source(self.cluster.get("mgmt_host"))}/efs-mgmt'),
                         *map(shlex.quote, map(str, args))])

    def mgmt(self, g, label, *args, deadline=60):
        return g.run(label, self.cluster.get('mgmt_host'), self.mgmt_cmd(*args), deadline)

    # -- live identity ----------------------------------------------------
    def read_identity(self, g, phase, harness=True):
        ident = dict(phase=phase, at=now(), daemons=[], clients=[], harness=[])
        for node in self.cluster.get('nodes', []):
            rc, text = self.mgmt(g, f'version-{phase}', 'version', node['addr'])
            # efs-mgmt prints `version ADDR:PORT efsd <describe> (branch B, built T on H)`.
            line = next((l for l in text.splitlines() if l.startswith('version ')), '')
            line = re.sub(r'^version \S+\s+', '', line)
            ident['daemons'].append(dict(addr=node['addr'], host=node.get('host'), rc=rc,
                                         version=line.strip(), key=identity_key(line) if rc == 0 else None))
        for m in self.cluster.get('mounts', []):
            rc, text = g.run(f'client-identity-{phase}', m['host'],
                             self.helper(m['host'], '_client-identity', m['path']), 60)
            try:
                info = json.loads(text.strip().splitlines()[-1])
            except (ValueError, IndexError):
                info = dict(error=text.strip()[-300:] or f'exit {rc}')
            info.update(mount=m['path'], host=m['host'])
            info['key'] = identity_key(info['version']) if info.get('version') else None
            ident['clients'].append(info)
        hosts = {self.private['host']} | {m['host'] for m in self.cluster.get('mounts', [])}
        for host in sorted(h or 'local' for h in hosts) if harness else ():
            src = self.source(host)
            for prog in ('efsd', 'efs-fuse', 'efs-mgmt'):
                rc, text = g.run(f'tree-{prog}-{phase}', host,
                                 f'{shlex.quote(src + "/" + prog)} --version', 30)
                ident['harness'].append(dict(host=host, tree=src, program=prog, rc=rc,
                                             version=text.strip()[:200],
                                             key=identity_key(text) if rc == 0 else None))
        return ident

    # -- the run ------------------------------------------------------------
    def selected(self, gates):
        names = [g.name for g in gates]
        chosen = self.args.gates or names
        for name in chosen + self.args.skip:
            if name not in names:
                raise SystemExit(f'unknown gate {name!r}; known: {", ".join(names)}')
        return [g for g in gates if g.name in chosen and g.name not in self.args.skip]

    def run(self, gates):
        tick = time.monotonic()
        started = now()
        (self.out / 'gates').mkdir(parents=True)
        (self.out / 'cluster.json').write_text(json.dumps(self.cluster, indent=2) + '\n')
        results = []
        try:
            setup_failed = self.setup(results)
            self.run_gates(gates, results, setup_failed)
        finally:
            self.teardown()
        self.cleanup_scratch()
        if self.before_sums:
            self.before_sums(self.out)
        verdict = worst(r['verdict'] for r in results)
        run = dict(driver='scripts/qualify.py', argv=sys.argv, cwd=os.getcwd(),
                   driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                   driver_host=socket.gethostname(), start=started, end=now(),
                   seconds=round(time.monotonic() - tick, 3),
                   allow=sorted(self.allow), owned_cluster=self.control.owned,
                   verdict=verdict, counts={v: sum(r['verdict'] == v for r in results)
                                            for v in (PASS, FAIL, NOTRUN)},
                   gates=[dict(gate=r['gate'], verdict=r['verdict'], reason=r['reason'])
                          for r in results],
                   destructive_steps=[dict(gate=r['gate'], **s) for r in results
                                      for s in r['destructive_steps']])
        (self.out / 'run.json').write_text(json.dumps(run, indent=2) + '\n')
        sums = write_sums(self.out)
        write_summary(self, run, results, sums)
        return verdict

    def setup(self, results):
        """Start a driver-owned multi-host cluster; a failed formation is a
        FAIL result of its own and the gates are NOTRUN."""
        if not hasattr(self.control, 'owned_setup'):
            return None
        g = GateRun(self, Gate('_setup', None, 'A13 (formation), A5', 'Candidate and profile'), 1800)
        rc, why = self.control.owned_setup(g)
        failed = None if rc == 0 or self.plan else (why.strip()[-300:] or f'exit {rc}')
        g.record.update(verdict=FAIL if failed else (NOTRUN if self.plan else PASS), reason=failed or '',
                        end=now(), seconds=round(time.monotonic() - g.start, 3), commands=g.commands,
                        fixture=g.fixture, destructive_steps=g.steps, raw=g.raw)
        (g.dir / 'result.json').write_text(json.dumps(g.record, indent=2) + '\n')
        print(f'[{now()}] setup {g.record["verdict"]} {failed or ""}', flush=True)
        if failed:
            results.append(g.record)
        return failed

    def teardown(self):
        if not hasattr(self.control, 'owned_teardown') or getattr(self, 'torn_down', False):
            return
        self.torn_down = True
        g = GateRun(self, Gate('_teardown', None, '', ''), 1800)
        if not self.args.keep_work:
            self.control.owned_teardown(g)
        (g.dir / 'result.json').write_text(json.dumps(dict(g.record, commands=g.commands, end=now(), raw=g.raw,
                                                           kept=bool(self.args.keep_work))) + '\n')

    def run_gates(self, gates, results, setup_failed):
        for gate in self.selected(gates):
            deadline = self.args.deadline.get(gate.name, gate.deadline_s)
            g = GateRun(self, gate, deadline)
            missing = [n for n in gate.needs if n not in self.allow]
            print(f'[{now()}] gate {gate.name} start (deadline {deadline}s)', flush=True)
            if gate.name != 'identity' and not self.plan and not setup_failed:
                g.record['identity'] = self.gate_identity(g)
            if setup_failed:
                verdict, reason = NOTRUN, 'cluster setup failed: ' + setup_failed
            elif missing:
                verdict, reason = NOTRUN, 'destructive step not allowed: needs ' + \
                    ' '.join(f'--allow {n}' for n in missing)
            else:
                try:
                    verdict, reason = gate.fn(self, g)
                except Exception as exc:  # the gate's own harness failed
                    verdict, reason = FAIL, f'driver error: {type(exc).__name__}: {exc}'[:400]
            if g.record.get('identity', {}).get('changed') and verdict != NOTRUN:
                verdict, reason = FAIL, 'build identity differs from the run start: ' + \
                    ' | '.join(g.record['identity']['keys'])
            self.collect_harness_logs(g)
            if self.plan:
                verdict, reason = NOTRUN, 'plan only (--plan): nothing ran'
            g.record.update(verdict=verdict, reason=reason, end=now(),
                            seconds=round(time.monotonic() - g.start, 3), commands=g.commands,
                            fixture=g.fixture, destructive_steps=g.steps, raw=g.raw)
            (g.dir / 'result.json').write_text(json.dumps(g.record, indent=2) + '\n')
            print(f'[{now()}] gate {gate.name} {verdict} {reason}', flush=True)
            results.append(g.record)
        if not self.plan and self.cluster.get('nodes'):
            end = GateRun(self, Gate('_identity-end', None, 'A1, A2', 'Candidate and profile'), 600)
            self.identity_end = self.read_identity(end, 'end')
            end.raw_file('identity-end.json').write_text(json.dumps(self.identity_end, indent=2) + '\n')
            why = identity_change_reason(self.identity_start, self.identity_end)
            verdict = (FAIL if self.identity_start else NOTRUN) if why else PASS
            end.record.update(verdict=verdict, reason=why, end=now(),
                              seconds=round(time.monotonic() - end.start, 3),
                              commands=end.commands, fixture=end.fixture,
                              destructive_steps=end.steps, raw=end.raw)
            (end.dir / 'result.json').write_text(json.dumps(end.record, indent=2) + '\n')
            results.append(end.record)

    def gate_identity(self, g):
        """Daemon and client identity read live before this gate."""
        if not self.cluster.get('nodes'):
            return dict(keys=[], unreadable=[], changed=False)
        ident = self.read_identity(g, 'gate', harness=False)
        g.raw_file('identity.json').write_text(json.dumps(ident, indent=2) + '\n')
        entries = ident['daemons'] + ident['clients']
        keys = sorted({e['key'] for e in entries if e.get('key')})
        start = getattr(self, 'identity_start', None)
        start_keys = sorted({e['key'] for e in start['daemons'] + start['clients'] if e.get('key')}) \
            if start else keys
        return dict(keys=keys, changed=bool(keys) and keys != start_keys,
                    unreadable=[e.get('addr') or e.get('mount') for e in entries if not e.get('key')])

    def collect_harness_logs(self, g):
        """Copy a live harness's text logs and reports (not its stores) into
        the gate's raw outputs before the private tree is deleted."""
        work = Path(self.private['root']) / self.runid / g.gate.name
        if self.plan or not is_local(self.private['host']) or not work.is_dir():
            return
        skip = re.compile(r'^(n\d+|mnt[\w-]*|peer|storage\d*|tmp|data|raft)$')
        for path in sorted(work.rglob('*')):
            rel = path.relative_to(work)
            if any(skip.match(part) for part in rel.parts[:-1]) and 'logs' not in rel.parts:
                continue
            if len(rel.parts) > 4 or not path.is_file() or path.is_symlink() or \
                    path.suffix not in ('.log', '.json', '.txt', '.tsv', '.out') or path.stat().st_size > 50 << 20:
                continue
            dest = g.dir / 'harness' / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(path.read_bytes())
            g.raw.append(f'harness/{rel.as_posix()}')

    def cleanup_scratch(self):
        if self.plan or self.args.keep_work:
            return
        trees = [(h, p) for h, p in self.scratch.items() if re.search(r'/qualify-[\w.-]+$|/scratch$', p)]
        trees.append((self.private['host'], f'{self.private["root"]}/{self.runid}'))
        if not self.control.owned:  # an owned cluster's mounts go with it
            trees += [(m['host'], f'{m["path"]}/qualify-{self.runid}') for m in self.cluster.get('mounts', [])]
        for host, path in trees:
            cmd = delete_command(path)
            try:
                subprocess.run(self.runner.argv(host, cmd, 600), capture_output=True, timeout=660)
            except subprocess.TimeoutExpired:
                print(f'cleanup of {host}:{path} timed out; remove it by hand', flush=True)


# --------------------------------------------------------------------------
# Gates.

def g_identity(q, g):
    ident = q.read_identity(g, 'start')
    q.identity_start = ident
    (g.raw_file('identity-start.json')).write_text(json.dumps(ident, indent=2) + '\n')
    entries = ident['daemons'] + ident['clients'] + ident['harness']
    g.fixture = dict(daemons=len(ident['daemons']), clients=len(ident['clients']),
                     harness_programs=len(ident['harness']))
    if not ident['daemons'] or not ident['clients']:
        return NOTRUN, 'the description names no daemons or no mounts'
    unread = [e.get('addr') or e.get('mount') or e.get('program') for e in entries if not e.get('key')]
    keys = sorted({e['key'] for e in entries if e.get('key')})
    replaced = [c['mount'] for c in ident['clients'] if c.get('exe_deleted')]
    if len(keys) > 1:
        return FAIL, 'build identities differ: ' + ' | '.join(keys)
    if replaced:
        return FAIL, 'client executable replaced since start: ' + ', '.join(replaced)
    if unread:
        return NOTRUN, 'identity unreadable for: ' + ', '.join(map(str, unread))
    if 'no git metadata' in keys[0]:
        return NOTRUN, 'binaries carry no git identity (build with EFS_GIT_ID/EFS_GIT_DESCRIBE set): ' + keys[0]
    g.fixture['identity'] = keys[0]
    return PASS, ''


def parse_members(text):
    """`efs-mgmt status` member lines -> {endpoint: (up, storage roots)}."""
    out = {}
    for m in re.finditer(r'^\s+node \d+: (\S+)\s+(up|DOWN)\b[^\n]*?(?:storage\((\d+)\))?\s*:?$', text, re.M):
        out[m.group(1)] = (m.group(2) == 'up', int(m.group(3)) if m.group(3) else None)
    return out


def g_topology(q, g):
    """A3/A4/A5: the members the cluster reports are the described nodes,
    all up, on how many distinct hosts; the profile goes into the fixture."""
    c = q.cluster
    nodes, hosts = c.get('nodes', []), distinct_hosts(c)
    mounts = c.get('mounts', [])
    g.fixture.update(nodes=len(nodes), node_hosts=hosts, transport=c.get('transport'),
                     quota=c.get('quota'), features=c.get('features', {}),
                     mounts=[f'{m["host"]}:{m["path"]}' for m in mounts],
                     client_hosts=sorted({m['host'] for m in mounts}), hardware=c.get('hardware'),
                     fixture=q.fixture)
    if len(nodes) < 3:
        return FAIL, f'{len(nodes)} nodes described; alpha 1.0 needs at least 3'
    rc, text = q.mgmt(g, 'status', 'status', c.get('seed') or nodes[0]['addr'])
    if q.plan:
        return NOTRUN, 'plan only'
    if rc:
        return FAIL, outcome_from_rc(rc, text)[1]
    members = parse_members(text)
    g.fixture['members'] = {ep: dict(up=up, storage_roots=n) for ep, (up, n) in members.items()}
    want = {n['addr'] for n in nodes}
    if set(members) != want:
        return FAIL, (f'status reports members {sorted(members)}, the description {sorted(want)}')
    down = sorted(ep for ep, (up, _) in members.items() if not up)
    if down:
        return FAIL, 'members down: ' + ', '.join(down)
    if len(hosts) < len(nodes) or c.get('owned') and not c.get('private_cluster'):
        return NOTRUN, (f'{len(nodes)} members on {len(hosts)} host(s): not distinct failure domains '
                        '(A4); regression evidence only')
    return PASS, f'{len(nodes)} members up on {len(hosts)} distinct hosts'


def g_posix(script, extra=''):
    def gate(q, g):
        m = q.mount(0)
        if not m:
            return NOTRUN, 'no mount described'
        res = q.remote_dir(g, m['host'], g.gate.name) + '/results.tsv'
        cmd = (f'cd {shlex.quote(q.source(m["host"]))} && python3 {script} {shlex.quote(m["path"])} '
               f'--results {shlex.quote(res)} --jobs {q.fixture["posix_jobs"]} '
               f'--timeout-s {q.fixture["posix_timeout_s"]}{extra}')
        rc, text = g.run('suite', m['host'], cmd)
        g.fixture.update(cases=count_marks(g.fetch(m['host'], res, 'results.tsv').read_text(errors='replace')
                                           if not q.plan else ''), mount=m['path'],
                         jobs=q.fixture['posix_jobs'], per_case_timeout_s=q.fixture['posix_timeout_s'])
        return scoped(g, outcome_from_rc(rc, text, g.commands[-1].get('timed_out')))
    return gate


def scoped(g, outcome):
    """A PASS whose suite skipped cases says so: skipped cases are scope, not PASS."""
    cases = g.fixture.get('cases') or {}
    skipped = cases.get('SKIP', 0) + cases.get('NOTRUN', 0)
    if outcome[0] == PASS and skipped:
        return PASS, f'{cases.get("PASS", 0)} cases pass; {skipped} SKIP/NOTRUN cases are out of scope (see results.tsv)'
    return outcome


def g_two_client(q, g):
    a, b = q.mount(0), q.mount(1)
    if not b:
        return NOTRUN, 'two-client suite needs two mounts'
    src = q.source(a['host'])
    if (a['host'] or 'local') == (b['host'] or 'local'):
        res = q.remote_dir(g, a['host'], g.gate.name) + '/results.tsv'
        cmd = (f'cd {shlex.quote(src)} && python3 tests/posix/posix_2client.py --local '
               f'{shlex.quote(a["path"])} {shlex.quote(b["path"])} --results {shlex.quote(res)}')
        host = a['host']
    elif a['path'] == b['path']:
        res = str(q.work / 'scratch' / g.gate.name / 'results.tsv')
        (q.work / 'scratch' / g.gate.name).mkdir(parents=True, exist_ok=True)
        cmd = (f'cd {shlex.quote(str(ROOT))} && python3 tests/posix/posix_2client.py --remote '
               f'{shlex.quote(a["host"])} {shlex.quote(b["host"])} --mnt {shlex.quote(a["path"])} '
               f'--ssh {shlex.quote(ssh_program(q.cluster["ssh"]) if q.cluster.get("ssh") else "ssh")} '
               f'--results {shlex.quote(res)}')
        host = 'local'
    else:
        return NOTRUN, 'two hosts need the same mount path for posix_2client.py --remote'
    rc, text = g.run('suite', host, cmd)
    g.fixture.update(cases=count_marks(g.fetch(host, res, 'results.tsv').read_text(errors='replace')
                                       if not q.plan else ''), mounts=[a['path'], b['path']])
    return scoped(g, outcome_from_rc(rc, text, g.commands[-1].get('timed_out')))


def stop_failure(q, text, context, rc=1):
    """A desktop observer makes a private clean-stop gate untestable, not PASS.

    Never ignore an owned writer, arbitrary holder or another stop error, and
    never change the client's refusal or send a signal to the observer.
    """
    match = re.search(r'is in use by: ([^\n;]+); stop refused', text)
    if rc == 1 and getattr(q.control, 'owned', False) and match:
        holders = re.findall(r'([^ ,()]+)\(\d+\)', match.group(1))
        if holders and all(re.fullmatch(r'localsearch(?:-3)?|gvfs-[A-Za-z0-9_-]+', h) for h in holders):
            return NOTRUN, 'environment: desktop mount observer holds the private fixture; clean stop was refused: ' + ', '.join(holders)
    return FAIL, context + outcome_from_rc(rc, text, rc == 124)[1]


def g_durability(q, g):
    m = q.mount(0)
    if not m:
        return NOTRUN, 'no mount described'
    src = shlex.quote(q.source(m['host']))
    res = q.remote_dir(g, m['host'], g.gate.name)
    tag = f'qual-{q.runid}'
    base = f'cd {src} && python3 tests/posix/posix_persist.py {shlex.quote(m["path"])} --tag {tag}'
    rc, text = g.run('prepare', m['host'], f'{base} --phase prepare --results {shlex.quote(res)}/prepare.tsv')
    if rc:
        return outcome_from_rc(rc, text, g.commands[-1].get('timed_out'))
    g.step('remount', f'client stop (no force) and remount of {m["host"]}:{m["path"]}')
    rc, text = q.control.client_stop(g, m)
    if rc:
        return stop_failure(q, text, 'clean client stop refused or failed: ', rc)
    rc, text = q.control.client_start(g, m)
    if rc:
        return FAIL, 'remount failed: ' + outcome_from_rc(rc, text)[1]
    rc, text = g.run('verify', m['host'], f'{base} --phase verify --results {shlex.quote(res)}/verify.tsv')
    if not q.plan:
        g.fixture['cases'] = count_marks(g.fetch(m['host'], f'{res}/verify.tsv', 'verify.tsv')
                                         .read_text(errors='replace'))
    return outcome_from_rc(rc, text, g.commands[-1].get('timed_out'))


def live_dir(q, g):
    path = f'{q.private["root"]}/{q.runid}/{g.gate.name}'
    g.run('mkdir-private', q.private['host'], f'mkdir -p {shlex.quote(path)}', 60)
    return path


def live_gate(build):
    """A harness that starts its own private cluster on the private host."""
    def gate(q, g):
        src = q.source(q.private['host'])
        work = live_dir(q, g)
        cmds, fixture = build(q, src, work, int(q.private['port_base']))
        if isinstance(cmds, tuple):
            return cmds
        g.fixture.update(fixture, private_host=q.private['host'], private_root=work)
        verdict, reason = PASS, ''
        for label, cmd, always in cmds:
            if verdict != PASS and not always:
                continue
            rc, text = g.run(label, q.private['host'], f'cd {shlex.quote(src)} && {cmd}')
            if not always:
                verdict, reason = outcome_from_rc(rc, text, g.commands[-1].get('timed_out'))
                g.fixture.setdefault('checks', {}).update(count_marks(text))
        return verdict, reason
    return gate


def b_append(q, src, work, port):
    args = q.fixture['append_failover_args']
    return [('append-failover', f'python3 tests/live/append_failover.py --rel {shlex.quote(src)} '
             f'--root {shlex.quote(work)}/run --port {port} {args}', False)], \
        dict(args=args, ports=f'{port + 1}-{port + 3}', rule='K15/K29 account()')


def b_k14(q, src, work, port):
    faults = q.cluster.get('faults_source')
    phases = q.fixture['k14_phases'] if not faults else '1 2 3 4 5 6 7'
    tail = f' {shlex.quote(faults)}' if faults else ''
    base = f'scripts/k14_atomic_cluster.sh {{}} {shlex.quote(src)} {shlex.quote(work)}/run {port + 20}{tail}'
    return [('k14-run', f'K14_PHASES={shlex.quote(phases)} ' + base.format('run'), False),
            ('k14-stop', base.format('stop'), True)], \
        dict(phases=phases, fault_build=bool(faults), ports=f'{port + 21}-{port + 23}')


def b_k13(q, src, work, port):
    return [('k13', f'python3 tests/live/k13_ticket_cluster.py --source {shlex.quote(src)} '
             f'--work {shlex.quote(work)}/run --port {port + 10}', False)], \
        dict(ports=f'{port + 10}-{port + 12}', stream_mib=256)


def b_fragment(q, src, work, port):
    return [('fragment-integrity', f'mkdir -p {shlex.quote(work)}/tmp && TMPDIR={shlex.quote(work)}/tmp '
             f'python3 tests/live/fragment_integrity.py', False)], dict(configs='O_DIRECT x 1/2 roots')


def b_gc(q, src, work, port):
    w = shlex.quote(work)
    return [('gc-reclamation', f'mkdir -p {w}/p1 {w}/p2 && python3 tests/live/gc_reclamation.py '
             f'--root {w}/p1 --root {w}/p2 --port {port + 30} {q.fixture["gc_args"]}',
             False)], dict(args=q.fixture['gc_args'], ports=f'{port + 30}-{port + 33}')


def b_d5(q, src, work, port):
    """D5 (D43): expiry under the cluster expiry policy. The owned four-node
    fixture (tests/posix/posix_expiry.py -> gc_reclamation.py --expiry
    --expiry-only): policy off by default, on/off/on at runtime, folder
    coverage, lifecycle, restarts, member and leader loss, physical
    reclamation, then the POSIX core suite with expiry on."""
    w = shlex.quote(work)
    return [('d5-expiry', f'mkdir -p {w}/r1 {w}/r2 {w}/mnt {w}/peer && python3 tests/posix/posix_expiry.py '
             f'{w}/mnt --suite expiry --root {w}/r1 --root {w}/r2 --peer-mount {w}/peer '
             f'--port {port + 40} --jobs {q.fixture["posix_jobs"]} --timeout-s {q.fixture["posix_timeout_s"]}',
             False)], \
        dict(ports=f'{port + 40}-{port + 43}', members=4, transport='tcp',
             posix_timeout_s=q.fixture['posix_timeout_s'],
             policy='cluster expiry policy off by default, then on/off/on at runtime (D43)')


def manifest_verify(q, g, m, manifest_path, label):
    rc, text = g.run(label, m['host'], q.helper(m['host'], '_manifest', 'verify'),
                     stdin_path=manifest_path)
    try:
        result = json.loads(text.strip().splitlines()[-1])
    except (ValueError, IndexError):
        result = dict(ok=False, error=text.strip()[-300:])
    return rc == 0 and result.get('ok'), result


def wait_mounts_serving(q, g, label, mounts=None):
    for m in mounts or q.cluster.get('mounts', []):
        rc, text = g.run(label, m['host'], f'mountpoint -q {shlex.quote(m["path"])} && '
                         f'timeout 30 stat {shlex.quote(m["path"])} >/dev/null', 60)
        if rc:
            return False, m
    return True, None


def wait_cluster_ready(q, g, deadline):
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        rcs = [q.mgmt(g, 'ready-probe', 'raft-getattr', n['addr'], 1, deadline=30)[0]
               for n in q.cluster.get('nodes', [])]
        if rcs and not any(rcs):
            return True
        time.sleep(3)
    return False


def g_e1(q, g):
    m = q.mount(0)
    if not m:
        return NOTRUN, 'no mount described'
    target = f'{m["path"]}/qualify-{q.runid}/e1'
    sizes = ','.join(map(str, q.fixture['e1_sizes']))
    manifest = g.raw_file('manifest.json')
    rc, text = g.run('write', m['host'], q.helper(m['host'], '_manifest', 'write', '--dir', target,
                                                  '--files', q.fixture['e1_files'], '--sizes', sizes,
                                                  '--seed', q.runid))
    if q.plan:
        return NOTRUN, 'plan'
    try:
        doc = json.loads(text.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return FAIL, 'manifest write produced no manifest: ' + text.strip()[-200:]
    manifest.write_text(json.dumps(doc) + '\n')
    g.fixture.update(target_files=q.fixture['e1_files'], acknowledged=len(doc['files']),
                     acknowledged_bytes=sum(f['size'] for f in doc['files']),
                     write_errors=len(doc['errors']), sizes=q.fixture['e1_sizes'], dir=target)
    ok, result = manifest_verify(q, g, m, manifest, 'verify-before')
    g.fixture['before'] = result
    if not ok:
        return FAIL, f'manifest verify before restart: {result}'[:400]
    if doc['errors']:
        return FAIL, f'{len(doc["errors"])} writes failed: {doc["errors"][0]}'[:400]
    res = orderly_restart(q, g, 'client stops (no force), orderly cluster stop, start, readiness, remount')
    if res:
        if res[0] == NOTRUN:
            return NOTRUN, f'manifest of {len(doc["files"])} acknowledged files verified; {res[1]}'
        return res
    ok, result = manifest_verify(q, g, m, manifest, 'verify-after')
    g.fixture['after'] = result
    ident = q.read_identity(g, 'after-restart')
    g.raw_file('identity-after-restart.json').write_text(json.dumps(ident, indent=2) + '\n')
    keys = {e['key'] for e in ident['daemons'] + ident['clients'] if e.get('key')}
    if not ok:
        return FAIL, f'manifest verify after restart: {result}'[:400]
    start_keys = {e['key'] for e in getattr(q, 'identity_start', {}).get('daemons', []) if e.get('key')}
    if start_keys and keys != start_keys:
        return FAIL, f'build identity changed across the restart: {sorted(start_keys)} -> {sorted(keys)}'
    return PASS, ''


def g_e2(q, g):
    m = q.mount(0)
    if not m:
        return NOTRUN, 'no mount described'
    path, host = m['path'], m['host']
    rc, text = g.run('fuser-idle', host, f'fuser -m {shlex.quote(path)} 2>/dev/null', 60)
    if q.plan:
        return NOTRUN, 'plan'
    if rc == 0 and text.strip():
        return NOTRUN, f'mount already in use by other processes ({text.strip()[:120]}); not touching it'
    d = f'{path}/qualify-{q.runid}/e2'
    rc, text = g.run('holder', host, f'mkdir -p {shlex.quote(d)} && cd {shlex.quote(d)} && '
                     f'(nohup sleep 900 >/dev/null 2>&1 & echo $!)', 60)
    holder = (text.strip().split() or [''])[-1]
    if rc or not holder.isdigit():
        return FAIL, 'could not start the holder process in the mount'
    try:
        rc, text = g.run('fuser-held', host, f'fuser -m {shlex.quote(path)} 2>/dev/null', 60)
        if holder not in re.findall(r'\d+', text):
            return FAIL, f'fuser -m does not list the holder pid {holder}: {text.strip()[:120]}'
        g.step('remount', f'client stop attempt while pid {holder} holds {host}:{path}')
        rc, text = q.control.client_stop(g, m)
        still, _ = wait_mounts_serving(q, g, 'still-mounted', [m])
        g.fixture['busy_stop_rc'] = rc
        if rc == 0 or not still:
            if not still:
                q.control.client_start(g, m)
            return FAIL, 'client stop was not refused while a process used the mount'
    finally:
        g.run('holder-kill', host, f'kill {holder} 2>/dev/null; true', 30)
    time.sleep(1)
    rc, text = g.run('fuser-released', host, f'fuser -m {shlex.quote(path)} 2>/dev/null', 60)
    if rc == 0 and text.strip():
        return FAIL, f'mount still in use after the holder exited: {text.strip()[:120]}'
    pending = f'{d}/pending.bin'
    rc, text = g.run('pending-write', host, q.helper(host, '_pending', 'write', pending, q.runid), 120)
    if rc:
        return FAIL, 'unsynced pending write failed: ' + text.strip()[-200:]
    g.step('remount', f'clean client stop (no force) and remount of {host}:{path}')
    t0 = time.monotonic()
    rc, text = q.control.client_stop(g, m)
    g.fixture.update(stop_seconds=round(time.monotonic() - t0, 3),
                     stop_deadline_s=q.cluster.get('control', {}).get('stop_timeout_s', 300))
    if rc:
        return stop_failure(q, text, 'clean client stop failed with the mount idle: ', rc)
    rc, text = g.run('unmounted', host, f'! mountpoint -q {shlex.quote(path)}', 30)
    if rc:
        q.control.client_start(g, m)
        return FAIL, 'client stop returned 0 but the path is still a mount point'
    rc, text = q.control.client_start(g, m)
    if rc:
        return FAIL, 'remount failed: ' + outcome_from_rc(rc, text)[1]
    rc, text = g.run('pending-verify', host, q.helper(host, '_pending', 'verify', pending, q.runid), 120)
    if rc:
        return FAIL, 'pending write lost or changed across the clean stop: ' + text.strip()[-200:]
    e1 = q.out / 'gates' / 'e1-manifest' / 'manifest.json'
    if e1.exists():
        ok, result = manifest_verify(q, g, m, e1, 'e1-verify')
        g.fixture['e1_after_remount'] = result
        if not ok:
            return FAIL, f'E1 manifest after remount: {result}'[:400]
    return PASS, ''


def leaders(q, g, label):
    """{group: (leader addr, term, leader raft id, voter ids)} by the nodes'
    own reports, plus each node's view (addr, role, leader id, term)."""
    views, out = {}, {}
    for node in q.cluster.get('nodes', []):
        rc, text = q.mgmt(g, label, 'raft-status', node['addr'], deadline=30)
        for m in re.finditer(r'group (\d+) hosted=1 role=(\w+) leader=(-?\d+) term=(\d+)([^\n]*)', text):
            if re.search(r'\bretired=1\b', m.group(5)):
                continue  # retained old Raft state is not an active hosting view
            group, role, lid, term = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))
            views.setdefault(group, []).append((node['addr'], role, lid, term))
            match = re.search(r'\bvoters=0x([0-9a-f]+)', m.group(5))
            voters = [b for b in range(64) if int(match.group(1) if match else '0', 16) >> b & 1]
            if role == 'LEADER' and term >= out.get(group, ('', -1))[1]:
                out[group] = (node['addr'], term, lid, voters)
    return out, views


def agreed(views, lid, term):
    """Every hosting node follows leader lid at term >= term (no stale leader)."""
    return bool(views) and all(v[2] == lid and v[3] >= term and v[1] in ('LEADER', 'FOLLOWER')
                               for v in views)


def applied(q, g, addr, label):
    rc, text = q.mgmt(g, label, 'raft-status', addr, deadline=30)
    return {int(m.group(1)): (int(m.group(2)), int(m.group(3))) for m in
            re.finditer(r'group (\d+) hosted=1 role=\w+ leader=-?\d+ term=\d+ commit=(\d+) applied=(\d+)([^\n]*)',
                        text) if not re.search(r'\bretired=1\b', m.group(4))}


def g_e3(q, g):
    m = q.mount(0)
    if not m or not q.cluster.get('nodes'):
        return NOTRUN, 'needs a mount and nodes'
    before, views = leaders(q, g, 'leaders-before')
    if q.plan:
        return NOTRUN, 'plan'
    g.fixture['leaders_before'] = {k: list(v[:2]) for k, v in before.items()}
    if not before:
        return FAIL, 'no metadata group leader observed'
    seconds = int(q.fixture['e3_seconds'])
    target = f'{m["path"]}/qualify-{q.runid}/e3'
    wl_log = g.raw_file('workload.json')
    wl_cmd = q.runner.argv(m['host'], q.helper(m['host'], '_workload', target, seconds,
                                               q.fixture['e3_file_kib'], q.runid), seconds + 120)
    workload = subprocess.Popen(wl_cmd, stdout=open(wl_log, 'wb'), stderr=subprocess.STDOUT,
                                start_new_session=True)
    t_start = time.monotonic()
    time.sleep(min(5, seconds / 4))
    events = []
    for group, (addr, term, lid, voters) in sorted(before.items()):
        # Name the successor: an untargeted step-down may be re-won by the
        # same node at a higher term, which is no turnover.
        others = [v for v in voters if v != lid]
        successor = sorted(others, key=lambda v: (v - lid) % 64)[0] + 1 if others else None
        g.step('turnover', f'raft-step-down group {group} on leader {addr} (term {term}) to node {successor}')
        t0 = time.monotonic()
        rc, text = q.mgmt(g, f'step-down-g{group}', 'raft-step-down', addr, group,
                          *([successor] if successor else []))
        new, attempts = None, 1
        while time.monotonic() - t0 < q.fixture['turnover_timeout_s']:
            now_leaders, views = leaders(q, g, f'leaders-g{group}')
            cur = now_leaders.get(group)
            if cur and cur[1] > term and cur[0] != addr and agreed(views.get(group), cur[2], cur[1]):
                new = cur
                break
            time.sleep(1)
            # A refusal that asks for a retry (target not caught up yet) is
            # retried while the old leader still leads.
            if rc and 'retry' in text and (not cur or cur[0] == addr):
                attempts += 1
                rc, text = q.mgmt(g, f'step-down-g{group}', 'raft-step-down', addr, group,
                                  *([successor] if successor else []))
        events.append(dict(group=group, old=addr, old_term=term, target_node=successor, rc=rc, attempts=attempts,
                           new=new and new[0], new_term=new and new[1],
                           seconds=round(time.monotonic() - t0, 3),
                           views=views.get(group)))
        if new is None:
            workload.wait()
            g.fixture['events'] = events
            return FAIL, (f'group {group}: leadership did not move off {addr} to an agreed new leader '
                          f'within {q.fixture["turnover_timeout_s"]}s')
    g.fixture['events'] = events
    catchup = None
    if 'restart' in q.allow and q.control.can_restart_nodes():
        group, (addr, *_) = sorted(before.items())[0]
        node = next(n for n in q.cluster['nodes'] if n['addr'] == addr)
        g.step('restart', f'orderly stop and start of member {addr} (returning member catch-up)')
        down_at = now()
        rc, text = q.control.stop_node(g, node)
        if rc:
            workload.wait()
            return FAIL, f'orderly stop of {addr} failed'
        time.sleep(5)
        rc, text = q.control.start_node(g, node)
        t0 = time.monotonic()
        caught = False
        while rc == 0 and time.monotonic() - t0 < q.fixture['catchup_timeout_s']:
            # The leaders' commit first, then the member's applied index: the
            # member has caught up once it applied what was committed before.
            lead, _ = leaders(q, g, 'catchup-leaders')
            ref = {gr: applied(q, g, la, 'catchup-leader').get(gr, (0, 0))[0] for gr, (la, *_) in lead.items()}
            mine = applied(q, g, addr, 'catchup-member')
            ref = {gr: v for gr, v in ref.items() if gr in mine}
            if mine and ref and all(mine[gr][1] >= ref[gr] for gr in ref):
                caught = True
                break
            time.sleep(2)
        catchup = dict(member=addr, down_at=down_at, caught_at=now(), caught_up=caught,
                       seconds=round(time.monotonic() - t0, 3))
        g.fixture['catchup'] = catchup
        if not caught:
            workload.wait()
            return FAIL, f'returning member {addr} did not catch up within {q.fixture["catchup_timeout_s"]}s'
    try:
        workload.wait(timeout=max(10, seconds - (time.monotonic() - t_start)) + 120)
    except subprocess.TimeoutExpired:
        os.killpg(workload.pid, signal.SIGKILL)
        return FAIL, 'client workload did not finish (stuck mount?)'
    try:
        wl = json.loads(wl_log.read_text().strip().splitlines()[-1])
    except (ValueError, IndexError):
        return FAIL, 'workload produced no report: ' + wl_log.read_text()[-200:]
    g.fixture['workload'] = {k: v for k, v in wl.items() if k not in ('files', 'failed')}
    manifest = g.raw_file('workload-manifest.json')
    # A failed write may leave its name behind; only acknowledged files are checked.
    manifest.write_text(json.dumps(dict(dir=target, files=wl['files'],
                                        errors=[dict(name=n) for n in wl.get('failed', [])])) + '\n')
    ok, result = manifest_verify(q, g, m, manifest, 'verify-workload')
    g.fixture['verify'] = result
    if wl['errors']:
        # Split by window so triage sees whether turnover or the member
        # outage (catch-up phase) cost the I/O; either is a FAIL.
        outage = [e for e in wl['errors'] if catchup and catchup['down_at'] <= e['at'] <= catchup['caught_at']]
        g.fixture['errors_in_member_outage'] = len(outage)
        g.fixture['errors_in_turnover'] = len(wl['errors']) - len(outage)
        failed = wl['ops'] - wl['acknowledged']
        return FAIL, (f'{failed} of {wl["ops"]} client ops failed ({len(wl["errors"]) - len(outage)} of the first '
                      f'{len(wl["errors"])} in the turnover window, {len(outage)} while member '
                      f'{catchup and catchup["member"]} was down): {wl["errors"][0]}')[:400]
    if not ok:
        return FAIL, f'acknowledged workload files wrong after turnover: {result}'[:400]
    if wl['max_gap_s'] > q.fixture['turnover_timeout_s']:
        return FAIL, f'client made no progress for {wl["max_gap_s"]}s'
    if catchup is None:
        return NOTRUN, ('turnover and client reconnect PASS; returning-member catch-up not run '
                        '(needs --allow restart and control.stop_node/start_node)')
    return PASS, ''


# B6 on a real cluster: one member stopped in order (control.stop_node), the
# fault the description can make. Refusing, full and slow members need the
# EFS_FAULTS daemon or a quota change: tests/test_b6_unavailable.py (loopback).
B6_SETS = {'before': [('R0', 300000, 1), ('R1', 300000, 1), ('R2', 300000, 1),
                      ('P0', 1, 1), ('P1', 512, 1), ('P2', 4096, 1), ('P3', 16384, 1),
                      ('F0', 300000, 0), ('F1', 300000, 0), ('F2', 300000, 0),
                      ('FS0', 1, 0), ('FS1', 4096, 0)],
           'during': [('W0', 300000, 1), ('W1', 300000, 1), ('W2', 300000, 1),
                      ('Q0', 1, 1), ('Q1', 512, 1), ('Q2', 4096, 1), ('Q3', 16384, 1)]}


def b6_status(q, g, label):
    rc, text = q.mgmt(g, label, 'status', q.cluster['seed'])
    m = re.search(r'^Degraded chunks: (\d+) ', text, re.M)
    down = re.findall(r'^\s+node (\d+): (\S+)\s+DOWN\b', text, re.M)
    return (int(m.group(1)) if m else None), {ep for _, ep in down}, text


def b6_pack_idle(q, g, label):
    rc, text = q.mgmt(g, label, 'pack-compact', q.cluster['seed'], 0, deadline=120)
    backlog = [int(n) for n in re.findall(r'backlog group=\d+ candidates=(\d+)', text)]
    return rc == 0 and len(backlog) >= 2 and not any(backlog)


def b6_helper(q, g, label, m, op, d, *extra, stdin_path=None, deadline=600):
    rc, text = g.run(label, m['host'], q.helper(m['host'], '_b6', op, '--dir', d, '--seed', q.runid,
                                                 *extra), deadline, stdin_path=stdin_path)
    try:
        return json.loads(text.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return dict(error=text.strip()[-300:] or f'exit {rc}')


def g_b6(q, g):
    w, r = q.mount(0), q.mount(1) or q.mount(0)
    if not w:
        return NOTRUN, 'no mount described'
    nodes = q.cluster.get('nodes', [])
    if len(nodes) < 3:
        return NOTRUN, 'needs three members'
    if not q.control.can_restart_nodes():
        return NOTRUN, 'control.stop_node/start_node not described'
    fx = q.fixture
    victim = next((n for n in reversed(nodes) if n['addr'] != q.cluster.get('seed')), nodes[-1])
    d = f'{w["path"]}/qualify-{q.runid}/b6'
    rd = f'{r["path"]}/qualify-{q.runid}/b6'
    if q.plan:
        return NOTRUN, 'plan'
    count, down, text = b6_status(q, g, 'status-start')
    if count != 0 or down:
        return NOTRUN, f'cluster not fully protected at the start (degraded {count}, down {sorted(down)})'
    before = b6_helper(q, g, 'write-before', w, 'write', d, '--set', 'before')
    if before.get('errors') or 'files' not in before:
        return FAIL, f'writes with every member up failed: {before}'[:400]
    acked = {f['name']: f['size'] for f in before['files']}
    end = time.monotonic() + fx['b6_pack_wait_s']
    while not b6_pack_idle(q, g, 'pack-wait'):
        if time.monotonic() > end:
            return NOTRUN, f'small files not packed within {fx["b6_pack_wait_s"]} s (pack backlog)'
        time.sleep(5)
    g.step('restart', f'orderly stop of member {victim["addr"]} (B6: unavailable destination)')
    rc, text = q.control.stop_node(g, victim)
    if rc:
        return FAIL, f'orderly stop of {victim["addr"]} failed'
    out = g.fixture
    try:
        end = time.monotonic() + 120
        while victim['addr'] not in b6_status(q, g, 'status-down')[1]:
            if time.monotonic() > end:
                return FAIL, f'{victim["addr"]} never reported DOWN'
            time.sleep(2)
        names = [n for n in acked if n[0] in 'RP']
        reads = b6_helper(q, g, 'read-down', r, 'verify', rd, '--names', ','.join(names),
                          '--bound', fx['b6_read_s'])
        out['reads_member_down'] = reads
        if not reads.get('ok'):
            return FAIL, f'reads with {victim["addr"]} down: {reads}'[:400]
        fs = b6_helper(q, g, 'fsync-down', w, 'fsync', d,
                       '--names', ','.join(n for n in acked if n.startswith('F')), '--bound', fx['b6_fsync_s'])
        out['fsync_member_down'] = fs
        if not fs.get('ok'):
            return FAIL, f'fsync with {victim["addr"]} down: {fs}'[:400]
        during = b6_helper(q, g, 'write-down', w, 'write', d, '--set', 'during', '--bound', fx['b6_write_s'])
        out['writes_member_down'] = during
        if during.get('errors') or 'files' not in during:
            return FAIL, f'writes with {victim["addr"]} down: {during}'[:400]
        acked.update({f['name']: f['size'] for f in during['files']})
        # The group leaders refresh the count in their GC pass: wait for it.
        end = time.monotonic() + 120
        while True:
            count, _, text = b6_status(q, g, 'status-degraded')
            if count and 'Protection: DEGRADED' in text:
                break
            if time.monotonic() > end:
                return FAIL, f'degraded state not shown with {victim["addr"]} down (count {count})'
            time.sleep(3)
        out['degraded_member_down'] = count
    finally:
        g.step('restart', f'start of member {victim["addr"]} (restore)')
        rc, text = q.control.start_node(g, victim)
    if rc:
        return FAIL, f'start of {victim["addr"]} failed'
    t0 = time.monotonic()
    while True:
        count, down, text = b6_status(q, g, 'status-rebuild')
        if count == 0 and not down and 'Protection: full' in text:
            break
        if time.monotonic() - t0 > fx['b6_rebuild_s']:
            return FAIL, f'W154 did not clear the degraded state within {fx["b6_rebuild_s"]} s (count {count})'
        time.sleep(5)
    out['rebuild_s'] = round(time.monotonic() - t0, 1)
    for label, m, path in (('verify-reader', r, rd), ('verify-writer', w, d)):
        res = b6_helper(q, g, label, m, 'verify', path, '--names', ','.join(sorted(acked)),
                        '--bound', fx['b6_read_s'])
        out[label] = res
        if not res.get('ok'):
            return FAIL, f'{label} after the restore: {res}'[:400]
    out['acknowledged'] = len(acked)
    return PASS, (f'member {victim["addr"]} down: reads exact, fsync with proof, writes degraded '
                  f'({out["degraded_member_down"]} marked); W154 cleared in {out["rebuild_s"]} s; '
                  f'{len(acked)} files exact (refusing, full and slow members: loopback test only)')


# --------------------------------------------------------------------------
# A13, C5, D1/D2, D3 and E4. Host-side work runs from the candidate tree as
# scripts/stage2_helpers.py (one JSON line out); see that file.

def helper2(q, host, *args):
    return ' '.join(['python3', shlex.quote(f'{q.source(host)}/scripts/stage2_helpers.py'),
                     *map(shlex.quote, map(str, args))])


def json_tail(text):
    try:
        return json.loads(text.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return dict(error=text.strip()[-300:] or 'no output')


class Background:
    """A command left running on a host while the gate goes on (a client
    load under joins, queries during reads); recorded like g.run."""

    def __init__(self, q, g, label, host, cmd, deadline):
        self.g = g
        index = len(g.commands) + 1
        self.log = g.raw_file(f'{index:02d}-{label}.log')
        self.entry = dict(label=label, host=host or 'local', command=cmd, deadline_s=round(deadline, 1),
                          start=now(), output=self.log.name, background=True)
        g.commands.append(self.entry)
        self.t0, self.p = time.monotonic(), None
        if q.plan:
            self.entry.update(end=self.entry['start'], rc=None, timed_out=False)
            return
        self.p = subprocess.Popen(q.runner.argv(host, cmd, deadline), stdout=open(self.log, 'wb'),
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)

    def wait(self, timeout):
        if self.p is None:
            return 0, ''
        timed_out = False
        try:
            rc = self.p.wait(timeout=max(1, timeout))
        except subprocess.TimeoutExpired:
            timed_out, rc = True, 124
            os.killpg(self.p.pid, signal.SIGKILL)
            self.p.wait()
        self.entry.update(end=now(), seconds=round(time.monotonic() - self.t0, 3), rc=rc, timed_out=timed_out)
        return rc, self.log.read_text(errors='replace')


def parse_status(text):
    """`efs-mgmt status` -> ({id: {addr, up, used, quota, capacity}}, usable {used, free, capacity})."""
    nodes, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^\s+node (\d+): (\S+)\s+(up|DOWN)\b', line)
        if m:
            cur = int(m.group(1))
            nodes[cur] = dict(addr=m.group(2), up=m.group(3) == 'up')
            continue
        b = re.match(r'^\s+bytes: used=(\d+) quota=(\d+) capacity=(\d+) available=(\d+)', line)
        if b and cur in nodes:
            nodes[cur].update(used=int(b.group(1)), quota=int(b.group(2)), capacity=int(b.group(3)))
    u = re.search(r'^Usable bytes: used=(\d+) free=(\d+) capacity=(\d+)', text, re.M)
    usable = dict(used=int(u.group(1)), free=int(u.group(2)), capacity=int(u.group(3))) if u else None
    return nodes, usable


def parse_raft(text):
    """`efs-mgmt raft-status` of one node -> (node id, {group: {role, voters, joint}}) for the groups
    it hosts as a voter; a replica retired by a voter change (K33: `retired=1`, its last view kept)
    is not hosted."""
    m = re.search(r'^node (\d+) root=', text, re.M)
    groups = {int(gr): dict(role=role, voters=int(v, 16), joint=int(j)) for gr, role, v, j, retired in
              re.findall(r'group (\d+) hosted=1 role=(\w+) leader=-?\d+ term=\d+ .*?voters=0x([0-9a-f]+) '
                         r'joint=(\d+) retired=(\d+)', text) if retired == '0'}
    return (int(m.group(1)) if m else None), groups


def mask(ids):
    return sum(1 << (i - 1) for i in ids)


def client_exports(q):
    env = dict(EFS_TRANSPORT=q.cluster.get('transport') or 'tcp')
    env.update((q.cluster.get('private_cluster') or {}).get('env', {}))
    return ' '.join(f'export {k}={shlex.quote(str(v))};' for k, v in sorted(env.items()))


def a13_layout(q):
    """The five members the A13 gate forms and grows: the description's
    `a13` block, else derived (driver-owned clusters only): member i on the
    host of topology node i mod N, at that node's port + a13_port_offset
    (+1 per wrap), its store and the gate mount under the cluster root."""
    c, fx = q.cluster, q.fixture
    if c.get('a13'):
        return c['a13']['nodes'], c['a13']['mount'], c['a13']['root']
    count = int(fx['a13_members'])
    if isinstance(q.control, Loopback):
        root = str(q.work / 'a13')
        base = int(q.private['port_base']) + int(fx['a13_port_offset'])
        return ([dict(addr=f'127.0.0.1:{base + i}', host='local', store=f'{root}/n{i + 1}') for i in range(count)],
                dict(host='local', path=f'{root}/mnt'), root)
    if not c.get('private_cluster') or not c.get('nodes') or not c.get('mounts'):
        return None
    topo, root = c['nodes'], c['private_cluster']['root'].rstrip('/') + '/a13'
    nodes = []
    for i in range(count):
        t = topo[i % len(topo)]
        h, p = t['addr'].rsplit(':', 1)
        nodes.append(dict(addr=f'{h}:{int(p) + int(fx["a13_port_offset"]) + i // len(topo)}', host=t['host'],
                          store=f'{root}/n{i + 1}'))
    return nodes, dict(host=c['mounts'][0]['host'], path=f'{root}/mnt'), root


def g_a13(q, g):
    """A13 on real daemons: form 3 (seed + --join), grow to 4 under a client
    load with an interrupted join retried (SIGKILL mid-join, same store and
    command: the same member), then to 5 through another member while the
    seed is stopped (seed loss). Checks the SHARD2 voter transition
    {1,2,3} -> {2,3,4} (no joint left), member 5 storage-only, usable
    capacity of the added quotas, new fragments on the added members, and
    every acknowledged payload exact."""
    layout = a13_layout(q)
    if not layout:
        return NOTRUN, ('needs a driver-owned cluster (loopback or private_cluster) or an `a13` block naming '
                        'five member endpoints, hosts and stores')
    nodes, mnt, root = layout
    fx = q.fixture
    if len(nodes) < 5:
        return NOTRUN, f'a13 names {len(nodes)} members; the gate grows 3 -> 4 -> 5'
    bad = [n['addr'] for n in nodes if int(n['addr'].rsplit(':', 1)[1]) in RESERVED_PORTS or
           n['store'].startswith('/data1/efs')]
    if bad or mnt['path'].startswith('/data1/efs'):
        return FAIL, f'a13 layout uses reserved ports or /data1/efs: {bad or mnt["path"]}'
    seed = nodes[0]['addr']
    g.fixture.update(members=[f'{n["addr"]} ({n["host"]})' for n in nodes], mount=f'{mnt["host"]}:{mnt["path"]}',
                     distinct_hosts=len({n['host'] for n in nodes}), quota=fx['a13_quota'],
                     load_threads=fx['a13_threads'], op_deadline_s=fx['a13_op_deadline_s'],
                     join_timeout_s=fx['a13_join_timeout_s'], interrupt_after_s=fx['a13_interrupt_s'])
    envs = [f'--env=EFS_TRANSPORT={q.cluster.get("transport") or "tcp"}'] + \
        [f'--env={k}={v}' for k, v in sorted((q.cluster.get('private_cluster') or {}).get('env', {}).items())]

    def node_cmd(n, op, join=None, timeout=120):
        args = ['_node', op, '--addr', n['addr'], '--store', n['store'], '--timeout', timeout]
        if op == 'start':
            args += ['--quota', fx['a13_quota'], *envs] + (['--join', join] if join else [])
        return q.helper(n['host'], *args)

    def start(i, join=None, label=None):
        return g.run(label or f'start-n{i + 1}', nodes[i]['host'], node_cmd(nodes[i], 'start', join), 300)

    def status(via):
        # status exits nonzero while a member is down (DEGRADED); the text still lists every member.
        rc, text = q.mgmt(g, 'status', 'status', via, deadline=60)
        return parse_status(text) if 'Cluster nodes' in text else ({}, None)

    def raft(i):
        rc, text = q.mgmt(g, 'raft-status', 'raft-status', nodes[i]['addr'], deadline=30)
        return parse_raft(text) if rc == 0 else (None, {})

    def wait_members(count, via, label, deadline=None):
        end = time.monotonic() + (deadline or fx['a13_join_timeout_s'])
        while True:
            members, usable = status(via)
            if len(members) == count and all(v['up'] for v in members.values()):
                return members, usable
            if time.monotonic() > end:
                return None, f'{label}: {len(members)} members ({sum(v["up"] for v in members.values())} up) after ' \
                    f'{deadline or fx["a13_join_timeout_s"]} s, want {count} up'
            time.sleep(2)

    def wait_voters(want, label):
        """Every member that hosts a group agrees on its voters, no joint
        configuration left; want = {group: id set}; returns the views."""
        end = time.monotonic() + fx['a13_join_timeout_s']
        while True:
            views = {i: raft(i) for i in up}
            ok = all(any(gr in v[1] for v in views.values()) for gr in want) and \
                all(v[1][gr]['voters'] == mask(ids) and not v[1][gr]['joint']
                    for v in views.values() for gr, ids in want.items() if gr in v[1])
            hosts = {gr: sorted(v[0] for v in views.values() if gr in v[1]) for gr in want}
            if ok and all(hosts[gr] == sorted(ids) for gr, ids in want.items()):
                return views, None
            if time.monotonic() > end:
                return views, (f'{label}: voters/hosts not {dict((k, sorted(v)) for k, v in want.items())} '
                               f'within {fx["a13_join_timeout_s"]} s: ' +
                               json.dumps({i: v[1] for i, v in views.items()})[:300])
            time.sleep(2)

    m_src = shlex.quote(q.source(mnt['host']))
    mpath = shlex.quote(mnt['path'])
    g.step('restart', f'A13 gate cluster: format {len(nodes)} private stores under {root} (seed {seed})')
    for i in range(3):
        rc, text = start(i, join=seed if i else None)
        if rc:
            return FAIL, f'start of member {i + 1} failed: ' + text.strip()[-200:]
        if q.plan:
            break
    if q.plan:
        return NOTRUN, 'plan'
    up = [0, 1, 2]
    try:
        members, usable3 = wait_members(3, seed, 'formation', fx['a13_join_timeout_s'])
        if members is None:
            return FAIL, usable3
        views, why = wait_voters({0: {1, 2, 3}, 2: {1, 2, 3}}, 'formation')
        if why:
            return FAIL, why
        ids = {v['addr']: k for k, v in members.items()}
        if [ids.get(n['addr']) for n in nodes[:3]] != [1, 2, 3]:
            return FAIL, f'formation ids {ids}: want 1, 2, 3 in start order'
        g.fixture['capacity_3'] = usable3
        rc, text = g.run('mount', mnt['host'], f'mkdir -p {mpath} && cd {m_src} && {client_exports(q)} '
                         f'scripts/client.sh {shlex.quote(seed)} {mpath}', 300)
        if rc:
            return FAIL, 'gate mount failed: ' + text.strip()[-200:]
        before = f'{mnt["path"]}/qualify-{q.runid}/before'
        rc, text = g.run('write-before', mnt['host'], q.helper(mnt['host'], '_manifest', 'write', '--dir', before,
                                                                '--files', fx['a13_files'], '--sizes',
                                                                ','.join(map(str, fx['e1_sizes'])), '--seed', q.runid))
        doc = json_tail(text)
        if rc or 'files' not in doc:
            return FAIL, 'writes at 3 members failed: ' + str(doc)[:300]
        manifest = g.raw_file('manifest-before.json')
        manifest.write_text(json.dumps(doc) + '\n')
        scratch = q.remote_dir(g, mnt['host'], 'a13')
        stop_file = f'{scratch}/stop-load'
        load_dir = f'{mnt["path"]}/qualify-{q.runid}/load'
        load = Background(q, g, 'client-load', mnt['host'], helper2(
            q, mnt['host'], 'load', '--dir', load_dir, '--stop', stop_file, '--seed', q.runid,
            '--threads', fx['a13_threads'], '--kib', 64, '--max-s', g.remaining(),
            '--deadline-s', fx['a13_op_deadline_s']), g.remaining())
        time.sleep(3)
        try:
            # 3 -> 4 through member 2, interrupted: SIGKILL shortly after the
            # joiner starts; then the same command again (a retried join).
            g.step('restart', f'join {nodes[3]["addr"]} through {nodes[1]["addr"]}, SIGKILL after '
                              f'{fx["a13_interrupt_s"]} s (interrupted join), then the same join again')
            rc, text = g.run('join-n4-interrupted', nodes[3]['host'],
                             f'{node_cmd(nodes[3], "start", nodes[1]["addr"], timeout=0)} && sleep '
                             f'{float(fx["a13_interrupt_s"])} && {node_cmd(nodes[3], "kill")}', 120)
            if rc:
                return FAIL, 'interrupted join of member 4 could not be started/killed: ' + text.strip()[-200:]
            mid, _ = status(nodes[1]['addr'])
            g.fixture['members_after_interrupt'] = len(mid)
            # A product build interrupts by timing only: say which handoff it hit.
            interrupted = 'before its commit' if len(mid) == 3 else 'after its commit'
            g.fixture['interrupted'] = interrupted
            rc, text = start(3, join=nodes[1]['addr'], label='join-n4-retry')
            if rc:
                return FAIL, 'retried join of member 4 failed: ' + text.strip()[-200:]
            up.append(3)
            members, usable4 = wait_members(4, nodes[1]['addr'], 'retried join to 4')
            if members is None:
                return FAIL, usable4
            ids = {v['addr']: k for k, v in members.items()}
            if ids.get(nodes[3]['addr']) != 4:
                return FAIL, f'retried join: member 4 endpoint has id {ids.get(nodes[3]["addr"])} ({ids})'
            views, why = wait_voters({0: {1, 2, 3}, 2: {2, 3, 4}}, 'SHARD2 transition at 4')
            if why:
                return FAIL, why
            g.fixture.update(capacity_4=usable4, shard2_voters_4='0x%x' % mask({2, 3, 4}))
            # 4 -> 5 while the seed is down: join through member 3.
            g.step('restart', f'orderly stop of the seed {seed} (seed loss)')
            rc, text = g.run('stop-seed', nodes[0]['host'], node_cmd(nodes[0], 'stop'), 300)
            if rc:
                return FAIL, 'orderly stop of the seed failed: ' + text.strip()[-200:]
            up.remove(0)
            g.step('restart', f'join {nodes[4]["addr"]} through {nodes[2]["addr"]} with the seed down')
            rc, text = start(4, join=nodes[2]['addr'], label='join-n5-seed-down')
            if rc:
                return FAIL, 'join of member 5 with the seed down failed: ' + text.strip()[-200:]
            up.append(4)
            end = time.monotonic() + fx['a13_join_timeout_s']
            while True:
                members, _ = status(nodes[1]['addr'])
                ids = {v['addr']: k for k, v in members.items()}
                if len(members) == 5 and members.get(ids.get(nodes[4]['addr'], 0), {}).get('up'):
                    break
                if time.monotonic() > end:
                    return FAIL, f'member 5 not added with the seed down within {fx["a13_join_timeout_s"]} s: {ids}'
                time.sleep(2)
            if ids.get(nodes[4]['addr']) != 5:
                return FAIL, f'member 5 endpoint has id {ids.get(nodes[4]["addr"])}'
            g.step('restart', f'start of the seed {seed} again')
            rc, text = start(0, label='restart-seed')
            if rc:
                return FAIL, 'restart of the seed failed: ' + text.strip()[-200:]
            up.insert(0, 0)
            members, usable5 = wait_members(5, seed, 'seed return at 5')
            if members is None:
                return FAIL, usable5
            views, why = wait_voters({0: {1, 2, 3}, 2: {2, 3, 4}}, 'voters at 5')
            if why:
                return FAIL, why
            if views[4][1]:
                return FAIL, f'member 5 hosts metadata groups {sorted(views[4][1])}; want storage only'
            g.fixture['capacity_5'] = usable5
        finally:
            g.run('stop-load', mnt['host'], f'touch {shlex.quote(stop_file)}', 60)
            rc, text = load.wait(max(300, fx['a13_op_deadline_s'] * 4))
        wl = json_tail(text)
        g.fixture['load'] = {k: v for k, v in wl.items() if k not in ('files', 'failed')}
        if 'files' not in wl:
            return FAIL, 'client load produced no report: ' + str(wl)[:300]
        # Usable capacity: 2/3 of every member's quota, the added ones included.
        quota = sum(v.get('quota', 0) for v in members.values())
        want_cap = quota * 2 // 3
        cap3, cap5 = usable3['capacity'], usable5['capacity']
        g.fixture.update(quota_bytes_5=quota, usable_capacity_3=cap3, usable_capacity_5=cap5)
        if cap5 < 0.99 * want_cap or cap5 <= cap3:
            return FAIL, f'usable capacity {cap5} at 5 members; want 2/3 of {quota} = {want_cap} (was {cap3} at 3)'
        rc, text = g.run('statfs', mnt['host'], f'python3 -c "import os; s = os.statvfs({mnt["path"]!r}); '
                         f'print(s.f_blocks * s.f_frsize)"', 60)
        df = int(text.strip().splitlines()[-1]) if rc == 0 and text.strip().splitlines()[-1].isdigit() else 0
        g.fixture['client_df_bytes'] = df
        if df < 0.99 * want_cap:
            return FAIL, f'the client reports {df} bytes total; want the grown {want_cap}'
        # New writes reach the added members (usable, not just reported).
        used0 = {k: v.get('used', 0) for k, v in members.items()}
        after = f'{mnt["path"]}/qualify-{q.runid}/after'
        rc, text = g.run('write-after', mnt['host'], q.helper(mnt['host'], '_manifest', 'write', '--dir', after,
                                                               '--files', 48, '--sizes', 262144, '--seed', q.runid))
        doc_after = json_tail(text)
        if rc or 'files' not in doc_after:
            return FAIL, 'writes at 5 members failed: ' + str(doc_after)[:300]
        end = time.monotonic() + 120
        while True:
            members, _ = status(seed)
            grown = {k: members.get(k, {}).get('used', 0) - used0.get(k, 0) for k in (4, 5)}
            if all(v > 0 for v in grown.values()) or time.monotonic() > end:
                break
            time.sleep(5)
        g.fixture['used_growth_added_members'] = grown
        if not all(v > 0 for v in grown.values()):
            return FAIL, f'new writes placed nothing on the added members: used growth {grown}'
        for label, path, d in (('verify-before', manifest, None), ('verify-after', None, doc_after),
                               ('verify-load', None, dict(dir=wl['dir'], files=wl['files'],
                                                          errors=[dict(name=n) for n in wl.get('failed', [])]))):
            if path is None:
                path = g.raw_file(f'manifest-{label[7:]}.json')
                path.write_text(json.dumps(d) + '\n')
            ok, result = manifest_verify(q, g, mnt, path, label)
            g.fixture[label] = result
            if not ok:
                return FAIL, f'{label}: {result}'[:400]
        if wl.get('nerrors') or wl.get('nover'):
            return FAIL, (f'client load: {wl.get("nerrors")} of {wl.get("ops")} ops failed, {wl.get("nover")} over '
                          f'{fx["a13_op_deadline_s"]} s (max gap {wl.get("max_gap_s")} s): '
                          f'{(wl.get("errors") or wl.get("over_deadline"))[:1]}')[:400]
    finally:
        g.run('unmount', mnt['host'], f'mountpoint -q {mpath} || exit 0; cd {m_src} && scripts/client.sh stop '
              f'{mpath} || fusermount3 -uz {mpath}', 300)
        for n in reversed(nodes):
            g.run(f'stop-{n["addr"]}', n['host'], node_cmd(n, 'stop'), 300)
        for i, n in enumerate(nodes):
            store = Path(n['store'])
            g.fetch(n['host'], f'{store.parent}/{store.name}.log', f'efsd-a13-n{i + 1}.log')
        g.fetch(mnt['host'], f'{Path(mnt["path"]).parent}/efs-fuse-{Path(mnt["path"]).name}.log', 'efs-fuse-a13.log')
        if not q.args.keep_work:
            for host in sorted({n['host'] for n in nodes} | {mnt['host']}):
                g.run('delete-a13', host, delete_command(root), 900)
    shared = g.fixture['distinct_hosts'] < len(nodes)
    return PASS, (f'3 -> 4 (join killed {interrupted}, retried: same member 4; SHARD2 {{1,2,3}} -> {{2,3,4}}) -> 5 through '
                  f'member 3 with the seed down (member 5 storage only); usable {cap3} -> {cap5} B; '
                  f'{wl["acknowledged"]} load files and every manifest exact' +
                  (f'; members share {g.fixture["distinct_hosts"]} host(s) (A4: regression evidence)' if shared else ''))


def session_uuid(q, g, m, log):
    rc, text = g.run('session-uuid', m['host'], f'grep -o "session uuid=[0-9a-f]* epoch=[0-9]*" '
                     f'{shlex.quote(log)} | tail -1', 60)
    found = re.search(r'uuid=([0-9a-f]{32}) epoch=(\d+)', text)
    return (found.group(1), int(found.group(2))) if found else (None, None)


def session_get(q, g, uuid, label):
    rc, text = q.mgmt(g, label, 'raft-session', q.cluster['seed'], 'get', uuid, deadline=60)
    found = re.search(r'rc=(-?\d+) .*?epoch=(\d+) state=(\d+)', text)
    return (int(found.group(1)), int(found.group(2)), int(found.group(3))) if found else (None, None, None)


def g_c5(q, g):
    """C5 against the topology cluster with real clients and mgmt calls:
    W133 declaration gate and W140 fail-closed log tags over the wire on
    every member; removed settings (A6/A7) refused by the candidate's efsd
    and efs-fuse on every host; K5/W134 session fence of an extra mount
    (mutations EIO, reads answer, a peer mount unaffected); W72 clean
    unmount revoke (the session's epoch advances, the data stays)."""
    m = q.mount(0)
    nodes = q.cluster.get('nodes', [])
    if not m or not nodes:
        return NOTRUN, 'needs a mount and nodes'
    fx, out = q.fixture, g.fixture
    mhost = q.cluster.get('mgmt_host')
    rc, text = g.run('wire', mhost, helper2(q, mhost, 'wire', *[x for n in nodes for x in ('--node', n['addr'])]), 300)
    if q.plan:
        return NOTRUN, 'plan'
    wire = json_tail(text)
    out['wire'] = dict(passed=wire.get('passed'), total=wire.get('total'),
                       failed=[c for c in wire.get('checks', []) if not c.get('ok')][:10])
    if rc or not wire.get('total'):
        return FAIL, f'wire gate: {out["wire"]}'[:400]
    rc, text = q.mgmt(g, 'serving-after-wire', 'raft-getattr', q.cluster['seed'], 1, deadline=60)
    if rc:
        return FAIL, 'the cluster stopped serving after the wire probes: ' + text.strip()[-200:]
    hosts = sorted({n.get('host') or 'local' for n in nodes} | {mm['host'] or 'local' for mm in q.cluster['mounts']})
    out['refusals'] = {}
    for host in hosts:
        scratch = q.remote_dir(g, host, 'c5-refuse')
        rc, text = g.run(f'refuse-{host}', host, helper2(q, host, 'refuse', '--src', q.source(host),
                                                         '--scratch', scratch), 600)
        doc = json_tail(text)
        out['refusals'][host] = dict(passed=doc.get('passed'), total=doc.get('total'),
                                     failed=[c for c in doc.get('checks', []) if not c.get('ok')][:5])
        if rc or not doc.get('total'):
            return FAIL, f'removed settings on {host}: {out["refusals"][host]}'[:400]
    # K5: fence an extra mount's session; then the clean-unmount revoke (W72).
    src = shlex.quote(q.source(m['host']))
    scratch = q.remote_dir(g, m['host'], 'c5')
    fdir = f'qualify-{q.runid}/c5'
    rc, text = g.run('fixtures', m['host'], helper2(q, m['host'], 'c5-prepare', '--dir', f'{m["path"]}/{fdir}'), 120)
    if rc:
        return FAIL, 'fixtures failed: ' + text.strip()[-200:]
    extra = {name: dict(host=m['host'], path=f'{scratch}/mnt-{name}') for name in ('fence', 'revoke')}
    mounted = []
    try:
        for name, em in extra.items():
            g.step('session', f'extra mount {em["host"]}:{em["path"]} of the topology cluster ({name})')
            g.run('mkdir-extra-mount', em['host'], f'mkdir -p {shlex.quote(em["path"])}', 60)
            rc, text = q.control.client_start(g, dict(em))
            if rc:
                return FAIL, f'extra mount for {name} failed: ' + outcome_from_rc(rc, text)[1]
            mounted.append(em)
            em['uuid'], em['epoch'] = session_uuid(q, g, em, f'{scratch}/efs-fuse-mnt-{name}.log')
            if not em['uuid']:
                return FAIL, f'no session uuid in the {name} mount log'
            rc0, epoch, state = session_get(q, g, em['uuid'], f'session-{name}-at-mount')
            out[f'{name}_session_at_mount'] = dict(uuid=em['uuid'], rc=rc0, epoch=epoch, state=state)
            if (rc0, epoch, state) != (0, em['epoch'], 1):
                return FAIL, f'{name} mount session not active at its epoch: {out[f"{name}_session_at_mount"]}'
        f = extra['fence']
        g.step('session', f'raft-session fence of the extra mount session {f["uuid"]}')
        fenced = None
        for attempt in range(1, int(fx['c5_fence_attempts']) + 1):
            rc, text = q.mgmt(g, 'fence', 'raft-session', q.cluster['seed'], 'fence', f['uuid'], deadline=120)
            if re.search(r'status=0\b', text) and re.search(r'epoch=%d\b' % (f['epoch'] + 1), text) and \
                    'state=1' in text:
                fenced = attempt
                break
            time.sleep(10)
        out['fence_attempts'] = fenced
        if not fenced:
            return FAIL, f'raft-session fence not completed in {fx["c5_fence_attempts"]} attempts: ' + text.strip()[-200:]
        rc, text = g.run('fenced-mutations', f['host'], helper2(q, f['host'], 'c5-mutate', '--dir',
                                                                f'{f["path"]}/{fdir}', '--expect', 'eio'), 300)
        doc = json_tail(text)
        out['fenced'] = [f'{c["check"]}:{c["got"]}' for c in doc.get('checks', [])]
        if rc:
            return FAIL, 'fenced mount: ' + '; '.join(c['check'] + ' ' + str(c['got'])
                                                      for c in doc.get('checks', []) if not c['ok'])[:300]
        rc, text = g.run('peer-mutations', m['host'], helper2(q, m['host'], 'c5-mutate', '--dir',
                                                              f'{m["path"]}/{fdir}', '--expect', 'ok'), 300)
        if rc:
            return FAIL, 'peer mount after the fence: ' + str(json_tail(text))[:300]
        rc, text = g.run('fenced-log', f['host'], f'grep -m1 "fenced" {shlex.quote(scratch)}/efs-fuse-mnt-fence.log', 60)
        out['fence_latched_in_client_log'] = rc == 0
        if rc:
            return FAIL, 'the fenced client never logged the fence'
        rc, text = g.run('stop-fenced', f['host'], f'cd {src} && scripts/client.sh stop --force-discard '
                                                    f'{shlex.quote(f["path"])}', 300)
        if rc == 0:
            mounted.remove(f)
        # W72: a clean stop settles, then revokes its own session.
        r = extra['revoke']
        pend = f'{r["path"]}/{fdir}/revoke.bin'
        rc, text = g.run('revoke-write', r['host'], q.helper(r['host'], '_pending', 'write', pend, q.runid), 120)
        if rc:
            return FAIL, 'write through the revoke mount failed: ' + text.strip()[-200:]
        t0 = time.monotonic()
        rc, text = q.control.client_stop(g, r)
        out['revoke_stop_s'] = round(time.monotonic() - t0, 3)
        if rc:
            return stop_failure(q, text, 'clean stop (no force) of the revoke mount refused or failed: ', rc)
        mounted.remove(r)
        rc0, epoch, state = session_get(q, g, r['uuid'], 'session-after-unmount')
        out['revoke_session_after_unmount'] = dict(rc=rc0, epoch=epoch, state=state)
        if rc0 != 0 or epoch is None or epoch <= r['epoch']:
            return FAIL, f'unmount did not revoke its session: {out["revoke_session_after_unmount"]}'
        rc, text = g.run('revoke-verify', m['host'], q.helper(m['host'], '_pending', 'verify',
                                                              f'{m["path"]}/{fdir}/revoke.bin', q.runid), 120)
        if rc:
            return FAIL, 'data written before the revoking unmount is wrong: ' + text.strip()[-200:]
    finally:
        for em in mounted:
            g.run('cleanup-extra-mount', em['host'], f'mountpoint -q {shlex.quote(em["path"])} || exit 0; cd {src} && '
                  f'scripts/client.sh stop --force-discard {shlex.quote(em["path"])} || '
                  f'fusermount3 -uz {shlex.quote(em["path"])}', 300)
    return PASS, (f'wire {wire["passed"]}/{wire["total"]} on {len(nodes)} members; removed settings refused on '
                  f'{len(hosts)} host(s); fence in {fenced} attempt(s), mutations EIO, peer unaffected; '
                  f'clean unmount revoked its session in {out["revoke_stop_s"]} s')


def compaction(q, g, label):
    rc, text = q.mgmt(g, label, 'pack-compact', q.cluster['seed'], 0, deadline=120)
    totals = [int(x) for x in re.findall(r'compacted_total=(\d+)', text)]
    packed = [int(x) for x in re.findall(r'files_packed=(\d+)', text)]
    return dict(compacted_total=sum(totals), files_packed=sum(packed),
                segments=sum(int(x) for x in re.findall(r' segments=(\d+) live', text)),
                holed=sum(int(x) for x in re.findall(r'holed=(\d+)', text)))


def orderly_restart(q, g, what):
    """Client stops (no force), orderly cluster stop, start, readiness,
    remount (E1, D1). Returns a FAIL/NOTRUN reason or None."""
    if 'restart' not in q.allow:
        return NOTRUN, 'orderly restart not allowed (needs --allow restart)'
    if not q.control.has('stop_cluster', 'start_cluster'):
        return NOTRUN, 'control.stop_cluster/start_cluster not described'
    mounts = q.cluster.get('mounts', [])
    for mm in mounts:
        rc, text = g.run('fuser-before-stop', mm['host'], f'fuser -m {shlex.quote(mm["path"])} 2>/dev/null', 60)
        if rc == 0 and text.strip():
            return NOTRUN, f'{mm["host"]}:{mm["path"]} in use ({text.strip()[:120]}); not stopping it'
    g.step('restart', what)
    for mm in mounts:
        rc, text = q.control.client_stop(g, mm)
        if rc:
            return stop_failure(q, text, f'clean client stop of {mm["path"]} failed: ', rc)
    t0 = time.monotonic()
    rc, text = q.control.stop_cluster(g)
    g.fixture['stop_seconds'] = round(time.monotonic() - t0, 3)
    if rc:
        return FAIL, 'orderly cluster stop failed: ' + outcome_from_rc(rc, text)[1]
    t0 = time.monotonic()
    rc, text = q.control.start_cluster(g)
    ready_deadline = q.cluster.get('control', {}).get('ready_timeout_s', 300)
    if rc or not wait_cluster_ready(q, g, ready_deadline):
        return FAIL, f'cluster not ready within {ready_deadline}s after restart'
    g.fixture.update(ready_seconds=round(time.monotonic() - t0, 3), ready_deadline_s=ready_deadline,
                     stop_deadline_s=q.cluster.get('control', {}).get('stop_timeout_s', 300))
    for mm in mounts:
        rc, text = q.control.client_start(g, mm)
        if rc:
            return FAIL, f'remount of {mm["path"]} failed: {outcome_from_rc(rc, text)[1]}'
    return None


def g_d1(q, g):
    """D1 (and D2 on the same segments): small files packed for real
    (every row a packed reference of the file's length), fewer stored
    objects than files (inventory savings), exact reads before and after
    packing through every mount; then overwrite, truncate and unlink one
    in eight each, an explicit compaction, the rest exact and repacked;
    exact and still packed after an orderly cold restart."""
    m = q.mount(0)
    if not m:
        return NOTRUN, 'no mount described'
    fx, out = q.fixture, g.fixture
    r = q.mount(1)
    host = m['host']
    d = f'{m["path"]}/qualify-{q.runid}/d1'
    mgmt = f'{q.source(host)}/efs-mgmt'
    eps = [x for n in q.cluster.get('nodes', []) for x in ('--node', n['addr'])]
    rc, text = g.run('write', host, helper2(q, host, 'smallfiles', 'write', '--dir', d, '--files', fx['d1_files'],
                                            '--seed', q.runid, '--max-size', fx['d1_max_size']), 1800)
    if q.plan:
        return NOTRUN, 'plan'
    doc = json_tail(text)
    if rc or 'files' not in doc:
        return FAIL, 'small-file writes failed: ' + str(doc.get('errors') or doc)[:300]
    manifest = g.raw_file('manifest.json')
    manifest.write_text(json.dumps(doc) + '\n')
    out.update(files=len(doc['files']), max_size=fx['d1_max_size'], bytes=sum(f['size'] for f in doc['files']),
               pack_wait_s=fx['d1_pack_wait_s'])

    def rows(path, label):
        rc, text = g.run(label, host, f'{helper2(q, host, "smallfiles", "rows", "--mgmt", mgmt, *eps)}', 1800,
                         stdin_path=path)
        return json_tail(text)

    def verify_all(path, label):
        for i, mm in enumerate([m] + ([r] if r else [])):
            p = path
            if i:
                other = json.loads(path.read_text())
                other['dir'] = other['dir'].replace(m['path'], mm['path'], 1)
                p = g.raw_file(f'{path.stem}-{label}-peer.json')
                p.write_text(json.dumps(other) + '\n')
            ok, result = manifest_verify(q, g, mm, p, f'{label}-{"peer" if i else "writer"}')
            out[f'{label}{"_peer" if i else ""}'] = result
            if not ok:
                return f'{label} through {mm["host"]}:{mm["path"]}: {result}'[:400]
        return None

    def wait_packed(path, label, allow):
        end = time.monotonic() + fx['d1_pack_wait_s']
        while True:
            res = rows(path, label)
            if res.get('nbad'):
                return res, f'{label}: bad packed rows: {res.get("bad")}'[:400]
            if res.get('unpacked') == 0 and res.get('packed'):
                return res, None
            if time.monotonic() > end:
                return res, f'{label}: {res.get("unpacked")} of {res.get("files")} not packed after {allow} s'
            time.sleep(10)

    first = rows(manifest, 'rows-before')
    out['before_packing'] = {k: first.get(k) for k in ('packed', 'unpacked', 'stored_objects')}
    why = verify_all(manifest, 'verify-before-packing')
    if why:
        return FAIL, why
    c0 = compaction(q, g, 'pack-summary-before')
    packed, why = wait_packed(manifest, 'rows-packing', fx['d1_pack_wait_s'])
    out['after_packing'] = {k: packed.get(k) for k in ('packed', 'unpacked', 'stored_objects', 'segments')}
    if why:
        return FAIL, why
    saved = first['stored_objects'] - packed['stored_objects']
    out['inventory_objects_saved'] = saved
    if packed['segments'] < 1 or packed['stored_objects'] >= first['stored_objects']:
        return FAIL, (f'no inventory saving: {first["stored_objects"]} stored objects before packing, '
                      f'{packed["stored_objects"]} after')
    why = verify_all(manifest, 'verify-after-packing')
    if why:
        return FAIL, why
    # D2 on the shared segments.
    rc, text = g.run('mutate', host, helper2(q, host, 'smallfiles', 'mutate', '--seed', q.runid), 600,
                     stdin_path=manifest)
    doc2 = json_tail(text)
    if rc or 'files' not in doc2:
        return FAIL, 'overwrite/truncate/unlink of packed files failed: ' + str(doc2.get('errors') or doc2)[:300]
    manifest2 = g.raw_file('manifest-mutated.json')
    manifest2.write_text(json.dumps(doc2) + '\n')
    out['mutations'] = doc2['mutations']
    why = verify_all(manifest2, 'verify-after-mutation')
    if why:
        return FAIL, why
    c1 = compaction(q, g, 'pack-summary-mutated')
    q.mgmt(g, 'compact-request', 'pack-compact', q.cluster['seed'], 64, deadline=120)
    end = time.monotonic() + fx['d1_compact_wait_s']
    while True:
        c2 = compaction(q, g, 'pack-summary-compacting')
        if c2['compacted_total'] > c1['compacted_total']:
            break
        if time.monotonic() > end:
            out['compaction'] = dict(before=c1, after=c2)
            return FAIL, (f'requested compaction of the holed segments moved nothing within '
                          f'{fx["d1_compact_wait_s"]} s (compacted_total {c1["compacted_total"]})')
        time.sleep(5)
    out['compaction'] = dict(start=c0, mutated=c1, compacted=c2)
    repacked, why = wait_packed(manifest2, 'rows-repacked', fx['d1_pack_wait_s'])
    out['after_compaction'] = {k: repacked.get(k) for k in ('packed', 'clipped', 'unpacked', 'stored_objects',
                                                            'segments')}
    if why:
        return FAIL, why
    why = verify_all(manifest2, 'verify-after-compaction')
    if why:
        return FAIL, why
    res = orderly_restart(q, g, 'D1 cold restart: client stops (no force), orderly cluster stop, start, remount')
    if res:
        if res[0] == FAIL:
            return res
        return NOTRUN, (f'packed {packed["packed"]} files into {packed["segments"]} segments, {saved} stored objects '
                        f'saved, D2 mutations and compaction exact; cold restart NOTRUN: {res[1]}')
    after = rows(manifest2, 'rows-after-restart')
    out['after_restart'] = {k: after.get(k) for k in ('packed', 'unpacked', 'stored_objects', 'segments')}
    why = verify_all(manifest2, 'verify-after-restart')
    if why:
        return FAIL, why
    if after.get('unpacked') or after.get('nbad'):
        return FAIL, f'after the cold restart: {after.get("unpacked")} files unpacked, bad {after.get("bad")}'[:400]
    return PASS, (f'{packed["packed"]} small files in {packed["segments"]} segments ({first["stored_objects"]} -> '
                  f'{packed["stored_objects"]} stored objects); {doc2["mutations"]} on packed files, compaction '
                  f'moved {c2["compacted_total"] - c1["compacted_total"]} records; exact through '
                  f'{1 + bool(r)} mount(s) before/after packing, after compaction and after a cold restart')


def g_d3(q, g):
    """D3 under load: `.efs` find/ls/stats and an invalid request in
    concurrent loops on a fresh fixture (initial catalog creation) while
    writers do create/fsync/read/rename/list; exact answers, every op
    within its deadline, new generations published, the load tree
    converges to its exact count, payloads exact."""
    m = q.mount(0)
    if not m:
        return NOTRUN, 'no mount described'
    fx = q.fixture
    r = q.mount(1)
    peer = r['path'] if r and (r['host'] or 'local') == (m['host'] or 'local') else None
    d = f'{m["path"]}/qualify-{q.runid}/d3'
    args = ['d3', '--dir', d, '--mount', m['path'], '--seed', q.runid, '--static-files', fx['d3_static_files'],
            '--writers', fx['d3_writers'], '--queriers', fx['d3_queriers'], '--load-s', fx['d3_load_s'],
            '--io-deadline-s', fx['d3_io_deadline_s'], '--query-deadline-s', fx['d3_query_deadline_s'],
            '--index-wait-s', fx['d3_index_wait_s']] + (['--peer', peer] if peer else [])
    rc, text = g.run('efs-load', m['host'], helper2(q, m['host'], *args))
    if q.plan:
        return NOTRUN, 'plan'
    doc = json_tail(text)
    g.fixture.update({k: v for k, v in doc.items() if k not in ('bad_payloads',)})
    if g.commands[-1].get('timed_out'):
        return FAIL, 'deadline exceeded'
    if rc:
        why = doc.get('error') or '; '.join(
            f'{k}: {doc.get(k)[:2]}' for k in ('io_errors', 'io_over_deadline', 'query_errors',
                                               'query_over_deadline', 'bad_payloads') if doc.get(k))
        if not why and not doc.get('load_converged'):
            why = f'load tree never converged: {doc.get("load_stats")} want {doc.get("load_files")} files'
        return FAIL, (why or str(doc))[:400]
    return PASS, (f'{doc["query_ops"]} `.efs` queries (p99 {doc["query_p99_s"]} s, max {doc["query_max_s"]} s) '
                  f'exact over {doc["generations"]} generations while {doc["io_ops"]} I/O ops ran (max '
                  f'{doc["io_max_s"]} s <= {fx["d3_io_deadline_s"]} s); {doc["load_files"]} files converged in '
                  f'{doc["load_converge_s"]} s and read back exact')


def g_e4(q, g):
    """E4: declared small- and large-file fixtures sized by the member
    count, completed vs target, every file exact on a cold reader (another
    mount, or this one remounted) under concurrent `.efs` queries, the
    catalog converges to the exact count, then delete and reclaim (usable
    bytes fall by the deleted payload) with the rest exact."""
    m = q.mount(0)
    nodes = q.cluster.get('nodes', [])
    if not m or not nodes:
        return NOTRUN, 'needs a mount and nodes'
    fx, out = q.fixture, g.fixture
    n = len(nodes)
    small, large = n * int(fx['e4_small_per_member']), n * int(fx['e4_large_per_member'])
    logical = small * 4500 + large * int(fx['e4_large_mib']) * (1 << 20)
    out.update(members=n, target_small=small, target_large=large, large_mib=fx['e4_large_mib'],
               threads=fx['e4_threads'], declared_logical_bytes=logical)
    rc, text = q.mgmt(g, 'status-before', 'status', q.cluster['seed'], deadline=60)
    if q.plan:
        g.run('write', m['host'], helper2(q, m['host'], 'e4', 'write', '--dir', f'{m["path"]}/qualify-{q.runid}/e4',
                                          '--seed', q.runid, '--small', small, '--large', large,
                                          '--large-mib', fx['e4_large_mib'], '--threads', fx['e4_threads']))
        return NOTRUN, 'plan'
    _, usable = parse_status(text)
    if not usable:
        return FAIL, 'status reports no usable bytes: ' + text.strip()[-200:]
    out['usable_before'] = usable
    if usable['free'] < 2 * logical:
        return NOTRUN, (f'declared fixture ({small} small, {large} x {fx["e4_large_mib"]} MiB, ~{logical >> 20} MiB) '
                        f'needs twice its size free; {usable["free"] >> 20} MiB usable free')
    d = f'{m["path"]}/qualify-{q.runid}/e4'
    rc, text = g.run('write', m['host'], helper2(q, m['host'], 'e4', 'write', '--dir', d, '--seed', q.runid,
                                                 '--small', small, '--large', large, '--large-mib',
                                                 fx['e4_large_mib'], '--threads', fx['e4_threads']))
    doc = json_tail(text)
    if 'files' not in doc:
        return FAIL, 'fixture writer produced no manifest: ' + str(doc)[:300]
    manifest = g.raw_file('manifest.json')
    manifest.write_text(json.dumps(doc) + '\n')
    out.update({k: doc.get(k) for k in ('completed_small', 'completed_large', 'bytes', 'seconds', 'small_per_s',
                                        'nerrors')})
    if rc or doc.get('nerrors'):
        return FAIL, (f'completed {doc.get("completed_small")}/{small} small and {doc.get("completed_large")}/{large} '
                      f'large files; first error {(doc.get("errors") or [None])[0]}')[:400]
    # Cold reader: the second mount (never read these files), else this one remounted.
    reader = q.mount(1)
    if not reader:
        if 'remount' not in q.allow:
            return NOTRUN, f'{small} + {large} files written; cold read needs a second mount or --allow remount'
        g.step('remount', f'client stop (no force) and remount of {m["host"]}:{m["path"]} (E4 cold reads)')
        rc, text = q.control.client_stop(g, m)
        if rc:
            return stop_failure(q, text, 'clean client stop failed: ', rc)
        rc, text = q.control.client_start(g, m)
        if rc:
            return FAIL, 'remount failed: ' + outcome_from_rc(rc, text)[1]
        reader = m
    rdir = d.replace(m['path'], reader['path'], 1)
    cold = g.raw_file('manifest-reader.json')
    cold.write_text(json.dumps(dict(doc, dir=rdir)) + '\n')
    scratch = q.remote_dir(g, reader['host'], 'e4')
    stop_file = f'{scratch}/stop-queries'
    queries = Background(q, g, 'query-load', reader['host'], helper2(
        q, reader['host'], 'e4', 'queries', '--dir', rdir, '--stop', stop_file,
        '--deadline-s', fx['e4_query_deadline_s'], '--max-s', g.remaining()), g.remaining())
    t0 = time.monotonic()
    try:
        ok, result = manifest_verify(q, g, reader, cold, 'verify-cold')
    finally:
        g.run('stop-queries', reader['host'], f'touch {shlex.quote(stop_file)}', 60)
        qrc, qtext = queries.wait(fx['e4_query_deadline_s'] * 3 + 60)
    out.update(cold_verify=result, cold_verify_s=round(time.monotonic() - t0, 3))
    qdoc = json_tail(qtext)
    out['queries'] = qdoc
    if not ok:
        return FAIL, f'cold verify: {result}'[:400]
    if qrc:
        return FAIL, f'concurrent `.efs` queries: {qdoc}'[:400]
    # The catalog converges to the exact fixture.
    settle = max(30, 2 * int(fx['e4_refresh_s']) + 60)
    end, last = time.monotonic() + settle, None
    while True:
        rc, text = g.run('stats', reader['host'], f'cd / && python3 {shlex.quote(rdir)}/.efs/stats --json 2>/dev/null', 120)
        try:
            last = json.loads(text.strip().splitlines()[0])['stats'] if rc == 0 else last
        except (ValueError, IndexError, KeyError):
            pass
        if last and last['tree_files'] == len(doc['files']) and last['tree_bytes'] == doc['bytes']:
            break
        if time.monotonic() > end:
            return FAIL, f'catalog never converged within {settle} s: {last}, want {len(doc["files"])} files'
        time.sleep(5)
    out['catalog'] = last
    rc, text = q.mgmt(g, 'status-full', 'status', q.cluster['seed'], deadline=60)
    _, full = parse_status(text)
    out['usable_with_fixture'] = full
    rc, text = g.run('delete', m['host'], helper2(q, m['host'], 'e4', 'delete'), 1800, stdin_path=manifest)
    ddoc = json_tail(text)
    if rc or 'files' not in ddoc:
        return FAIL, 'deletes failed: ' + str(ddoc.get('errors') or ddoc)[:300]
    rest = g.raw_file('manifest-after-delete.json')
    rest.write_text(json.dumps(ddoc) + '\n')
    out.update(deleted=ddoc['deleted'], deleted_bytes=ddoc['deleted_bytes'])
    end = time.monotonic() + fx['e4_reclaim_s']
    while True:
        rc, text = q.mgmt(g, 'status-reclaim', 'status', q.cluster['seed'], deadline=60)
        _, now_usable = parse_status(text)
        dropped = usable_used = None
        if now_usable:
            usable_used = now_usable['used']
            dropped = (full or now_usable)['used'] - usable_used
        if dropped is not None and dropped >= 0.8 * ddoc['deleted_bytes']:
            break
        if time.monotonic() > end:
            out['usable_after_delete'] = now_usable
            return FAIL, (f'reclaim: usable used fell {dropped} B from {(full or {}).get("used")} B; want at '
                          f'least 0.8 x the {ddoc["deleted_bytes"]} B deleted within {fx["e4_reclaim_s"]} s')
        time.sleep(10)
    out['usable_after_delete'] = now_usable
    ok, result = manifest_verify(q, g, m, rest, 'verify-after-delete')
    out['after_delete'] = result
    if not ok:
        return FAIL, f'files left after the deletes: {result}'[:400]
    return PASS, (f'{doc["completed_small"]}/{small} small + {doc["completed_large"]}/{large} x '
                  f'{fx["e4_large_mib"]} MiB exact on a cold reader under {qdoc.get("queries")} queries (p99 '
                  f'{qdoc.get("p99_s")} s); catalog exact; {ddoc["deleted"]} deleted, reclaimed, rest exact')


def g_plugin(name, spec):
    def gate(q, g):
        host = spec.get('host', 'local')
        values = dict(source=q.source(host), cluster=str(q.out / 'cluster.json'), gate_dir=str(g.dir),
                      seed=q.cluster.get('seed', ''), mount=(q.mount(0) or {}).get('path', ''),
                      peer_mount=(q.mount(1) or {}).get('path', ''),
                      transport=q.cluster.get('transport', ''),
                      soak_hours=q.fixture['soak_hours'], soak_profile=q.fixture['soak_profile'], candidate=q.cluster.get('candidate', 'unknown'))
        if spec.get('requires'):
            need = spec['requires'].format(**values)
            rc, text = g.run('requires', host, f'test -e {shlex.quote(need)}', 60)
            if rc and not q.plan:
                return NOTRUN, f'{need} not present on {host}'
        for kind in spec.get('needs', ()):
            g.step(kind, f'plugin {name} ran with --allow {kind}')
        rc, text = g.run(name, host, fill(spec['command'], values), spec.get('deadline_s'))
        verdict, reason = outcome_from_rc(rc, text, g.commands[-1].get('timed_out'))
        own = g.dir / 'plugin-result.json'
        if own.exists():
            g.raw.append(own.name)
            try:
                doc = json.loads(own.read_text())
            except ValueError:
                return FAIL, 'plugin-result.json is not JSON'
            g.fixture.update(doc.get('fixture', {}))
            claimed = doc.get('verdict')
            if claimed not in RANK:
                return FAIL, f'plugin-result.json verdict {claimed!r} is not PASS/FAIL/NOTRUN'
            if RANK[claimed] > RANK[verdict]:
                verdict, reason = claimed, doc.get('reason', '') or reason
            elif claimed == PASS and verdict != PASS:
                reason = f'plugin claims PASS but {reason}'
        return verdict, reason
    return gate


def g_standalone_plugin(name, spec):
    """Run an independent private workload with the owned fixture stopped.

    The soak's quiet-host guard must still reject unrelated jobs. Only the
    driver's own fixture may be suspended, and restart permission is required.
    """
    plugin = g_plugin(name, spec)

    def gate(q, g):
        if not q.control.owned:
            return NOTRUN, 'standalone plugin cannot suspend an externally managed cluster'
        if 'restart' not in q.allow:
            return NOTRUN, 'standalone plugin needs --allow restart'
        if not q.control.has('stop_cluster', 'start_cluster'):
            return NOTRUN, 'standalone plugin needs stop/start controls'
        mounts = q.cluster.get('mounts', [])
        for m in mounts:
            rc, text = g.run('fuser-before-standalone', m['host'],
                             f'fuser -m {shlex.quote(m["path"])} 2>/dev/null', 60)
            if rc == 0 and text.strip():
                return NOTRUN, f'standalone fixture mount is in use: {m["path"]}'
        g.step('restart', f'suspend owned fixture for standalone {name}')
        for m in mounts:
            rc, text = q.control.client_stop(g, m)
            if rc:
                return stop_failure(q, text, 'standalone clean client stop failed: ', rc)
        rc, text = q.control.stop_cluster(g)
        if rc:
            return FAIL, 'standalone orderly cluster stop failed: ' + outcome_from_rc(rc, text)[1]
        result = (FAIL, 'standalone plugin did not complete')
        try:
            result = plugin(q, g)
        finally:
            rc, text = q.control.start_cluster(g)
            ready_s = q.cluster.get('control', {}).get('ready_timeout_s', 300)
            if rc or not wait_cluster_ready(q, g, ready_s):
                raise RuntimeError(f'fixture restore failed after {name}; plugin result: {result}; {text}')
            for m in mounts:
                rc, text = q.control.client_start(g, m)
                if rc:
                    raise RuntimeError(f'fixture remount failed after {name}: {m["path"]}; {text}')
        return result
    return gate


def gates(plugins):
    out = [
        Gate('identity', g_identity, 'A1, A2', 'Candidate and profile', deadline_s=600,
             doc='build identity of every daemon, client and harness tree, read live'),
        Gate('topology', g_topology, 'A3, A4, A5', 'Candidate and profile', deadline_s=300,
             doc='status members = described nodes, all up, distinct hosts; profile recorded'),
    ]
    for name, spec in plugins.items():
        factory = g_standalone_plugin if spec.get('standalone') else g_plugin
        out.append(Gate(name, factory(name, spec), spec.get('items', 'plugin'),
                        spec.get('ledger', 'Placement/capacity and decode'),
                        needs=spec.get('needs', ()), deadline_s=spec.get('deadline_s', 3600),
                        doc=spec.get('doc', 'pluggable gate: ' + spec['command'][:60])))
    out += [
        Gate('posix-core', g_posix('tests/posix/posix_suite.py', ' --suite core'), 'C1, C2, C3',
             'POSIX and concurrent I/O', deadline_s=7200, doc='posix_suite.py --suite core'),
        Gate('posix-extras', g_posix('tests/posix/posix_extras.py'), 'C1, D3',
             'POSIX and concurrent I/O', deadline_s=3600, doc='posix_extras.py (mmap, .efs)'),
        Gate('posix-2client', g_two_client, 'C1, C3', 'POSIX and concurrent I/O', deadline_s=3600,
             doc='posix_2client.py on the first two mounts'),
        Gate('posix-durability', g_durability, 'C2, E2', 'POSIX and concurrent I/O', needs=('remount',),
             deadline_s=3600, doc='posix_persist.py prepare, clean remount, verify'),
        Gate('append-failover', live_gate(b_append), 'C3, C4 (K15/K29)', 'POSIX and concurrent I/O',
             deadline_s=3600, doc='tests/live/append_failover.py, private cluster'),
        Gate('k14-torn-read', live_gate(b_k14), 'C4 (K14)', 'POSIX and concurrent I/O', deadline_s=3600,
             doc='scripts/k14_atomic_cluster.sh, private cluster'),
        Gate('k13-tickets', live_gate(b_k13), 'C4 (K13)', 'POSIX and concurrent I/O', deadline_s=3600,
             doc='tests/live/k13_ticket_cluster.py, private cluster'),
        Gate('fragment-integrity', live_gate(b_fragment), 'B2', 'Placement/capacity and decode',
             deadline_s=3600, doc='tests/live/fragment_integrity.py, private clusters'),
        Gate('gc-reclamation', live_gate(b_gc), 'D2', 'Packing/shared-fragment GC', deadline_s=7200,
             doc='tests/live/gc_reclamation.py, private four-node cluster'),
        Gate('d5-expiry', live_gate(b_d5), 'D5', 'Packing/shared-fragment GC', deadline_s=3600,
             doc='tests/posix/posix_expiry.py (cluster expiry policy, lifecycle, POSIX core), private four-node cluster'),
        Gate('e1-manifest', g_e1, 'E1, E2', 'Full payloads + orderly restart', partial_needs=('restart',),
             deadline_s=3600, doc='acknowledged-file manifest verified after orderly stop/start'),
        Gate('e2-client-stop', g_e2, 'E2', 'Full payloads + orderly restart', needs=('remount',),
             deadline_s=1800, doc='fuser -m, stop refused while busy, clean stop, remount'),
        Gate('b6-member-down', g_b6, 'B6', 'Placement/capacity and decode', needs=('restart',),
             deadline_s=3600, doc='one member stopped in order: reads, fsync, writes, W154 after restore'),
        Gate('e3-turnover', g_e3, 'E3', 'Full payloads + orderly restart', needs=('turnover',),
             partial_needs=('restart',), deadline_s=1800,
             doc='raft-step-down of every group leader under client load; member catch-up'),
        Gate('a13-node-addition', g_a13, 'A13', 'Placement/capacity and decode', needs=('restart',),
             deadline_s=3600, doc='own 3-member cluster grown to 4 (interrupted join) and 5 (seed down) under load'),
        Gate('c5-protocol-live', g_c5, 'C5', 'POSIX and concurrent I/O', deadline_s=1800,
             doc='W133 declaration gate, W140 tags, A7 removed settings, K5 fence, W72 unmount revoke'),
        Gate('d1-packing', g_d1, 'D1, D2', 'Packing/shared-fragment GC', partial_needs=('restart',),
             deadline_s=3600, doc='packed mappings, inventory savings, D2 mutations + compaction, cold restart'),
        Gate('d3-efs-load', g_d3, 'D3', 'Enabled `.efs` under load', deadline_s=2400,
             doc='.efs find/ls/stats and refusals in loops under create/fsync/rename I/O'),
        Gate('e4-scale', g_e4, 'E4', 'Scale/progress evidence (E4, D4)', partial_needs=('remount',),
             deadline_s=14400, doc='declared small/large fixtures, cold reads under queries, delete/reclaim'),
    ]
    return out


def b_checks_command(cluster):
    """scripts/b_checks.py on the first mount's host, through every mount of
    that host (it reads through each mount it is given); its JSON result is
    printed into the gate log."""
    mounts = cluster.get('mounts', [])
    host = mounts[0].get('host', 'local') if mounts else 'local'
    paths = [m['path'] for m in mounts if m.get('host', 'local') == host]
    esc = lambda v: shlex.quote(str(v)).replace('{', '{{').replace('}', '}}')
    res = '"${{TMPDIR:-/tmp}}/qualify-b-checks-$$.json"'
    return host, ('cd {source} && python3 scripts/b_checks.py --seed {seed} '
                  + ' '.join(f'--mount {esc(p)}' for p in paths)
                  + f' --expect-members {len(cluster.get("nodes", []))} --out {res}; rc=$?; '
                  f'cat {res} 2>/dev/null; rm -f {res}; exit $rc')


def default_plugins(cluster, extra):
    host, command = b_checks_command(cluster)
    plugins = {'b-checks': dict(command=command, host=host,
                                requires='{source}/scripts/b_checks.py', items='B1-B5',
                                ledger='Placement/capacity and decode', deadline_s=3600,
                                doc='scripts/b_checks.py through the first host\'s mounts'),
               'd4-progress': dict(command='cd {source} && python3 scripts/d4_progress.py --cluster {cluster} '
                                           '--out {gate_dir}',
                                   requires='{source}/scripts/d4_progress.py', items='D4',
                                   ledger='Scale/progress evidence (E4, D4)', deadline_s=7200,
                                   doc='D4 bounded progress under packing, indexing and KV compaction')}
    plugins['soak'] = dict(
        standalone=True, needs=('restart',),
        command='cd {source} && nice -n 10 python3 scripts/soak.py --work {gate_dir}/runtime '
                '--out {gate_dir}/soak --hours {soak_hours} --profile {soak_profile} --source {candidate}',
        host=cluster.get('private', {}).get('host', 'local'),
        requires='{source}/scripts/soak.py', items='E4, D4',
        ledger='Scale/progress evidence (E4, D4)', deadline_s=18000,
        doc='seeded five-to-seven-member private loopback soak; fixture.soak_hours/profile (default four-hour endurance); not multi-host/RDMA qualification')
    plugins.update(cluster.get('plugins', {}))
    for spec in extra:
        name, _, command = spec.partition('=')
        if not name or not command:
            raise SystemExit(f'--plugin {spec!r}: expected NAME=COMMAND')
        plugins[name] = dict(command=command)
    return plugins


# --------------------------------------------------------------------------
# Raw sums, summary draft and F3 ledger row.

def write_sums(out):
    lines = []
    for path in sorted(p for p in out.rglob('*') if p.is_file()):
        rel = path.relative_to(out).as_posix()
        if rel in ('SHA256SUMS', 'summary-draft.md', 'f3-row.md'):
            continue
        lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {rel}\n')
    (out / 'SHA256SUMS').write_text(''.join(lines))
    return dict(files=len(lines), sha256=hashlib.sha256((out / 'SHA256SUMS').read_bytes()).hexdigest())


def verify_sums(out):
    bad = []
    listed = set()
    for line in (out / 'SHA256SUMS').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        listed.add(rel)
        path = out / rel
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            bad.append(rel)
    for path in out.rglob('*'):
        rel = path.relative_to(out).as_posix()
        if path.is_file() and rel not in listed | {'SHA256SUMS', 'summary-draft.md', 'f3-row.md'}:
            bad.append(rel + ' (not listed)')
    return bad


def distinct_hosts(cluster):
    return sorted({n.get('host') or 'local' for n in cluster.get('nodes', [])})


def topology_column(cluster):
    n, hosts = len(cluster.get('nodes', [])), distinct_hosts(cluster)
    label = {3: '3 physical nodes', 4: '4 physical nodes'}.get(n, f'>4 physical nodes (N={n})')
    if len(hosts) < n or cluster.get('owned'):
        label += f' — NOT distinct hosts ({len(hosts)} host(s); A4: regression evidence only)'
    return label


def ledger_cells(results):
    cells = {}
    for r in results:
        cells.setdefault(r['ledger'], []).append(r)
    return {row: (worst(r['verdict'] for r in rs), rs) for row, rs in cells.items()}


def md(text):
    return str(text).replace('|', '\\|').replace('\n', ' ')


def write_summary(q, run, results, sums):
    c, out = q.cluster, q.out
    ident = getattr(q, 'identity_start', None)
    cells = ledger_cells(results)
    covered = {gate.ledger for gate in gates(q.plugins)}
    column = topology_column(c)
    f3 = [f'| Evidence | {column} |', '| --- | --- |']
    for row in LEDGER_ROWS:
        verdict, rs = cells.get(row, ('NOTRUN', []))
        gates_txt = ', '.join(f'{r["gate"]} {r["verdict"]}' for r in rs) or \
            ('no gate selected in this run' if row in covered else 'not covered by this driver')
        f3.append(f'| {row} | {verdict} ({gates_txt}; [summary](summary-draft.md)) |')
    (out / 'f3-row.md').write_text(
        f'<!-- scripts/qualify.py {run["end"]}: F3 ledger column for {c.get("name")}; '
        f'raw {out} SHA256SUMS {sums["sha256"]} -->\n' + '\n'.join(f3) + '\n')
    L = []
    L.append(f'# Stage 2 qualification: {c.get("name", "cluster")} ({run["start"][:10]}) — DRAFT\n')
    L.append('Generated by `scripts/qualify.py`. Review every TODO before committing this page '
             'under `docs/status/`; raw outputs stay where the Raw evidence section says.\n')
    L.append('## Purpose and verdict\n')
    L.append('- Purpose: alpha 1.0 Stage 2 qualification gates on one frozen candidate '
             '([checklist](alpha-1.0-2plus1-checklist.md) A1–A5, A13, B1–B6, C1–C5, D1–D4, E1–E4 as '
             'planned in [A11](a11-qualification-matrix-20261024.md); F3 topology ledger).')
    L.append(f'- Verdict: **{run["verdict"]}** ({run["counts"]["PASS"]} PASS, {run["counts"]["FAIL"]} FAIL, '
             f'{run["counts"]["NOTRUN"]} NOTRUN of {len(results)} gates).')
    L.append(f'- F3 column: {column}.')
    L.append(f'- Gate-sequence wall time (setup/cleanup included when controlled here): {run["seconds"]} s.\n')
    L.append('## Candidate and build identity\n')
    L.append(f'- Candidate commit: `{c.get("candidate") or "TODO (description gives none)"}`; '
             f'source tree `{c.get("source")}`; driver sha256 `{run["driver_sha256"][:16]}`.')
    if ident:
        L.append('\n| Role | Where | Version (live) |\n| --- | --- | --- |')
        for d in ident['daemons']:
            L.append(f'| efsd | {d["addr"]} ({d.get("host")}) | {md(d["version"] or "unreadable")} |')
        for cl in ident['clients']:
            ver = cl.get('version') or cl.get('error', 'unreadable')
            extra = f' exe sha256 `{cl["exe_sha256"][:16]}`' if cl.get('exe_sha256') else ''
            L.append(f'| efs-fuse pid {cl.get("pid", "?")} | {cl.get("host")}:{cl["mount"]} | {md(ver)}{extra} |')
        for h in ident['harness']:
            L.append(f'| harness {h["program"]} | {h["host"]}:{h["tree"]} | {md(h["version"] or "unreadable")} |')
    else:
        L.append('- Live identity: NOTRUN (identity gate not selected).')
    L.append('\n## Environment\n')
    hosts = distinct_hosts(c)
    L.append(f'- Nodes: {len(c.get("nodes", []))} on {len(hosts)} distinct host(s): '
             + ', '.join(f'{n["addr"]} ({n.get("host")})' for n in c.get('nodes', [])) + '.')
    L.append('- Mounts: ' + (', '.join(f'{m["host"]}:{m["path"]}' for m in c.get('mounts', [])) or 'none') + '.')
    L.append(f'- Transport {c.get("transport", "TODO")}, `O_DIRECT` fragment I/O (the only mode), features '
             f'`{json.dumps(c.get("features", {}))}`, quota {c.get("quota", "TODO")}.')
    L.append(f'- Hardware: {c.get("hardware", "TODO")}. Notes: {c.get("notes", "-")}.')
    L.append(f'- Live harnesses: private clusters on {q.private["host"]} under {q.private["root"]}, '
             f'ports from {q.private["port_base"]} (single host, TCP loopback: regression evidence, A4).')
    L.append(f'- Fixture: `{json.dumps(q.fixture)}`.\n')
    L.append('## Commands\n')
    L.append(f'Driver (on {run["driver_host"]}, cwd `{run["cwd"]}`):\n')
    L.append('```bash\n' + ' '.join(shlex.quote(a) for a in run['argv']) + '\n```\n')
    L.append('| Gate | Host | Command | Deadline (s) | Exit | Seconds |\n| --- | --- | --- | --- | --- | --- |')
    for r in results:
        for cmd in r['commands']:
            if cmd['label'].startswith(('mkdir-', 'ready-probe', 'leaders-', 'catchup-', 'tree-', 'version-',
                                        'client-identity')):
                continue
            L.append(f'| {r["gate"]} | {cmd["host"]} | `{md(cmd["command"])}` | {cmd["deadline_s"]} | '
                     f'{cmd.get("rc")}{" (deadline)" if cmd.get("timed_out") else ""} | {cmd.get("seconds", "")} |')
    L.append('\nEvery command, including probes, is in `gates/*/result.json` with start and end times.\n')
    L.append('## Destructive steps\n')
    L.append(f'- Allowed: {", ".join(run["allow"]) or "none"}; cluster owned by the driver: '
             f'{run["owned_cluster"]}.')
    for s in run['destructive_steps']:
        L.append(f'- {s["at"]} {s["gate"]}: {s["kind"]}: {s["what"]}')
    if not run['destructive_steps']:
        L.append('- None performed.')
    L.append('\n## Results\n')
    L.append('| Gate | Items | Verdict | Reason | Seconds / deadline | Fixture |\n| --- | --- | --- | --- | --- | --- |')
    for r in results:
        fx = {k: v for k, v in r['fixture'].items() if not isinstance(v, (dict, list)) or k in ('cases', 'checks')}
        L.append(f'| {r["gate"]} | {r["items"]} | {r["verdict"]} | {md(r["reason"]) or "-"} | '
                 f'{r["seconds"]} / {r["deadline_s"]} | `{md(json.dumps(fx))[:300]}` |')
    fails = [r for r in results if r['verdict'] == FAIL]
    L.append('\n## Failures\n')
    L.extend(f'- **{r["gate"]}**: {r["reason"]} (raw `gates/{r["gate"]}/`). TODO: triage; does it also fail '
             'on the base?' for r in fails)
    if not fails:
        L.append('- None.')
    L.append('\n## Baseline and comparison\n\n- TODO: base commit and its result for each FAIL; earlier runs.\n')
    L.append('## Limitations and remaining gates\n')
    for r in results:
        if r['verdict'] == NOTRUN:
            L.append(f'- {r["gate"]} NOTRUN: {r["reason"]}')
    if len(hosts) < len(c.get('nodes', [])) or c.get('owned'):
        L.append('- Daemons share hosts: not distinct failure domains (A4).')
    L.append('- The live harnesses run on TCP loopback only (no RDMA live harness).')
    L.append('- TODO: other limitations.\n')
    L.append('## Raw evidence\n')
    L.append(f'- `{run["driver_host"]}:{out}` — {sums["files"]} files listed in `SHA256SUMS` '
             f'(sha256 `{sums["sha256"]}`). Recheck: `python3 scripts/qualify.py verify-sums {out}`.')
    L.append('- Private stores and scratch were deleted at the end of the run; '
             'the commands above recreate them.\n')
    L.append('## Reproduction and cleanup\n')
    L.append('- Build the candidate tree on every host at the same path (or list `sources`), write the cluster '
             'description (`cluster.json` in the raw directory is the exact copy used), run the driver command '
             'above with an empty output directory outside Git.')
    L.append('- Cleanup: unless `--keep-work`, the driver deletes its scratch, the private harness trees '
             f'(`{q.private["root"]}/{q.runid}`) and its test directories `MOUNT/qualify-{q.runid}`; '
             'a driver-owned loopback cluster is stopped and its stores deleted. Nothing else is touched.')
    (out / 'summary-draft.md').write_text('\n'.join(L) + '\n')


# --------------------------------------------------------------------------
# Helpers run on mount hosts (python3 SRC/scripts/qualify.py _NAME ...).

def payload(seed, name, size):
    return random.Random(f'{seed}/{name}').randbytes(size)


def h_manifest_write(args):
    d = Path(args.dir)
    d.mkdir(parents=True, exist_ok=False)
    sizes = [int(s) for s in args.sizes.split(',')]
    files, errors = [], []
    for i in range(args.files):
        sub = d / f'd{i % 8}'
        sub.mkdir(exist_ok=True)
        name = f'd{i % 8}/f{i:06d}'
        size = sizes[i % len(sizes)]
        data = payload(args.seed, name, size)
        mode = (0o644, 0o600, 0o640, 0o755)[i % 4]
        try:
            fd = os.open(d / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                view = memoryview(data)
                while view:
                    view = view[os.write(fd, view):]
                os.fchmod(fd, mode)
                os.fsync(fd)
            finally:
                os.close(fd)
            st = os.stat(d / name)
        except OSError as exc:
            errors.append(dict(name=name, errno=exc.errno, error=str(exc)))
            continue
        files.append(dict(name=name, size=size, mode=mode, mtime_ns=st.st_mtime_ns,
                          sha256=hashlib.sha256(data).hexdigest()))
    for sub in sorted(d.iterdir()):
        fd = os.open(sub, os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    print(json.dumps(dict(dir=str(d), seed=args.seed, files=files, errors=errors)))
    return 0


def h_manifest_verify(args):
    doc = json.loads(sys.stdin.read())
    d = Path(doc['dir'])
    bad, checked, size = [], 0, 0
    for f in doc['files']:
        path = d / f['name']
        try:
            st = os.stat(path)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as exc:
            bad.append(dict(name=f['name'], error=str(exc)))
            continue
        checked += 1
        size += st.st_size
        diff = [k for k, v in (('size', st.st_size), ('sha256', digest), ('mode', st.st_mode & 0o7777),
                               ('mtime_ns', st.st_mtime_ns)) if k in f and f[k] != v]
        if diff:
            bad.append(dict(name=f['name'], differs=diff))
    expected = {f['name'] for f in doc['files']} | {e['name'] for e in doc.get('errors', [])}
    present = {p.relative_to(d).as_posix() for p in d.rglob('*') if p.is_file()} if d.is_dir() else set()
    extra = sorted(present - expected)
    ok = not bad and not extra and checked == len(doc['files'])
    print(json.dumps(dict(ok=ok, checked=checked, bytes=size, bad=bad[:20], nbad=len(bad), extra=extra[:20])))
    return 0 if ok else 1


def b6_payload(seed, name, size):
    return payload(seed, f'b6/{name}', size)


def h_b6(args):
    d = Path(args.dir)
    out = dict(files=[], errors=[], times={})
    if args.op == 'write':
        d.mkdir(parents=True, exist_ok=True)
        for name, size, sync in B6_SETS[args.set]:
            t = time.monotonic()
            try:
                fd = os.open(d / name, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
                try:
                    view = memoryview(b6_payload(args.seed, name, size))
                    while view:
                        view = view[os.write(fd, view):]
                    if sync:
                        os.fsync(fd)
                finally:
                    os.close(fd)
                out['files'].append(dict(name=name, size=size, synced=sync))
            except OSError as exc:
                out['errors'].append(dict(name=name, errno=exc.errno, error=str(exc)))
            out['times'][name] = round(time.monotonic() - t, 3)
        out['ok'] = not out['errors'] and max(out['times'].values()) <= args.bound
    else:
        sizes = {n: s for st in B6_SETS.values() for n, s, _ in st}
        for name in [n for n in args.names.split(',') if n]:
            t = time.monotonic()
            try:
                if args.op == 'fsync':
                    fd = os.open(d / name, os.O_RDWR)
                    try:
                        os.fsync(fd)
                    finally:
                        os.close(fd)
                    out['files'].append(dict(name=name, verdict='ok'))
                else:
                    got = (d / name).read_bytes()
                    want = b6_payload(args.seed, name, sizes[name])
                    if got != want:  # wrong or short bytes: never acceptable
                        out['errors'].append(dict(name=name, wrong=True, got=len(got), want=len(want)))
                    else:
                        out['files'].append(dict(name=name, verdict='exact'))
            except OSError as exc:
                out['errors'].append(dict(name=name, errno=exc.errno, error=str(exc)))
            out['times'][name] = round(time.monotonic() - t, 3)
        out['ok'] = not out['errors'] and (not out['times'] or max(out['times'].values()) <= args.bound)
    print(json.dumps(out))
    return 0 if out['ok'] else 1


def h_pending(args):
    data = payload(args.seed, 'pending', 3 * 1048576 + 17)
    if args.op == 'write':
        Path(args.path).parent.mkdir(parents=True, exist_ok=True)
        with open(args.path, 'xb') as f:  # no fsync: the clean stop must drain it
            f.write(data)
        print(f'wrote {len(data)} bytes without fsync')
        return 0
    got = Path(args.path).read_bytes()
    print(f'read {len(got)} bytes, expected {len(data)}: {"PASS" if got == data else "FAIL"}')
    return 0 if got == data else 1


def h_workload(args):
    d = Path(args.dir)
    d.mkdir(parents=True, exist_ok=False)
    end = time.monotonic() + args.seconds
    files, errors, last_ok, max_gap, i = [], [], time.monotonic(), 0.0, 0
    while time.monotonic() < end:
        name = f'w{i:06d}'
        data = payload(args.seed, name, args.kib * 1024)
        try:
            with open(d / name, 'xb') as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            if (d / name).read_bytes() != data:
                raise OSError(0, 'read-back mismatch')
            st = os.stat(d / name)
            files.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                              mtime_ns=st.st_mtime_ns))
            t = time.monotonic()
            max_gap, last_ok = max(max_gap, t - last_ok), t
        except OSError as exc:
            errors.append(dict(name=name, errno=exc.errno, error=str(exc), at=now()))
        i += 1
    max_gap = max(max_gap, time.monotonic() - last_ok)
    print(json.dumps(dict(dir=str(d), ops=i, acknowledged=len(files), errors=errors[:50],
                          failed=[e['name'] for e in errors], max_gap_s=round(max_gap, 3), files=files)))
    return 0


def h_client_identity(args):
    sys.path.insert(0, str(ROOT / 'scripts'))
    out = dict()
    try:
        import client_processes
        found = client_processes.controller(args.mount)
    except Exception as exc:
        print(json.dumps(dict(error=f'{type(exc).__name__}: {exc}')))
        return 1
    if not found:
        print(json.dumps(dict(error='no efs-fuse controls this mount')))
        return 1
    pid = found[0]
    out['pid'] = pid
    exe = os.readlink(f'/proc/{pid}/exe')
    out.update(exe=exe, exe_deleted=exe.endswith(' (deleted)'))
    try:
        out['exe_sha256'] = hashlib.sha256(Path(f'/proc/{pid}/exe').read_bytes()).hexdigest()
    except OSError as exc:
        out['exe_sha256_error'] = str(exc)
    if not out['exe_deleted']:
        r = subprocess.run([exe, '--version'], capture_output=True, text=True, timeout=30)
        out['version'] = (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else ''
    print(json.dumps(out))
    return 0


# --------------------------------------------------------------------------

def pid_alive(pid):
    """True while PID runs (a zombie, ours or not, counts as exited)."""
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid:
            return False
    except ChildProcessError:
        pass
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except (OSError, IndexError):
        return True


def node_identity(pid, addr, store):
    return dict(pid=pid, start=Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19],
                uid=os.getuid(), source=str(ROOT.resolve()), addr=addr, store=str(store.resolve()))


def signal_owned_node(pid, sig, addr, store, identity_path):
    """A stale/reused numeric pid is never authority to stop a process."""
    expected = json.loads(identity_path.read_text())
    if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
        raise RuntimeError('owned node signalling requires Linux pidfd support')
    fd = os.pidfd_open(pid)
    try:
        actual = node_identity(pid, addr, store)
        if expected != actual or Path(f'/proc/{pid}').stat().st_uid != os.getuid():
            raise RuntimeError('recorded node identity does not match live process')
        # /proc cmdline can be empty briefly during exec. This matters for an
        # interrupted join (--timeout 0); an empty read is not an identity.
        end = time.monotonic() + 1
        while True:
            args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if args[0]:break
            if time.monotonic() >= end:raise RuntimeError('node command identity remained unreadable')
            time.sleep(.005)
        binary = os.fsencode(str(ROOT / 'efsd'))
        # A shebang launcher may prepend interpreter arguments. The recorded
        # boot tick and pidfd still bind this to the child we started.
        if binary not in args or os.fsencode(str(store)) not in args or os.fsencode(addr) not in args:
            raise RuntimeError('recorded node command does not match live process')
        signal.pidfd_send_signal(fd, sig)
    finally:
        os.close(fd)


def h_node(a):
    """One efsd of a driver-owned private cluster, on this host from this
    tree: `start` launches it detached (seed: ADDR STORE; joiner: --join
    SEED --listen ADDR STORE; a restart uses the persisted identity) and
    waits until it listens (--timeout 0: returns once spawned); `stop`
    sends SIGTERM to the recorded pid and waits, SIGKILL only after
    --timeout (exit 124); `kill` sends SIGKILL at once (an interrupted join)."""
    store = Path(a.store)
    for endpoint in (a.addr, a.join):
        if not endpoint:continue
        if not re.fullmatch(r'[^\s:]+:\d+', endpoint) or \
                not 1024 < int(endpoint.rsplit(':', 1)[1]) < 65536 or \
                int(endpoint.rsplit(':', 1)[1]) in RESERVED_PORTS:
            print(f'_node: reserved or invalid endpoint {endpoint}')
            return 2
    pidfile, logfile = store.parent / f'{store.name}.pid', store.parent / f'{store.name}.log'
    if str(store.resolve()).startswith('/data1/efs'):
        print('_node: stores may not live under /data1/efs')
        return 2
    identity_path = pidfile.with_suffix('.identity.json')
    old = int(pidfile.read_text()) if pidfile.exists() and pidfile.read_text().strip().isdigit() else None
    def send(sig):
        try:
            signal_owned_node(old, sig, a.addr, store, identity_path)
            return True
        except ProcessLookupError:
            return True
        except (OSError, ValueError, RuntimeError, KeyError, IndexError) as exc:
            print(f'_node {a.op} {a.addr}: refusing signal: {exc}')
            return False
    if a.op == 'kill':  # A13: an interrupted join (no orderly stop)
        if old and pid_alive(old):
            if not send(signal.SIGKILL):return 2
            end = time.monotonic() + 30
            while time.monotonic() < end and pid_alive(old):
                time.sleep(0.1)
        print(f'_node kill {a.addr}: pid {old} ' + ('still alive' if old and pid_alive(old) else 'gone'))
        return 1 if old and pid_alive(old) else 0
    if a.op == 'stop':
        if not old or not pid_alive(old):
            print(f'_node stop {a.addr}: not running')
            return 0
        if not send(signal.SIGTERM):return 2
        end = time.monotonic() + a.timeout
        while time.monotonic() < end and pid_alive(old):
            time.sleep(0.2)
        if pid_alive(old):
            if not send(signal.SIGKILL):return 2
            print(f'_node stop {a.addr}: pid {old} ignored SIGTERM for {a.timeout}s; SIGKILL')
            return 124
        print(f'_node stop {a.addr}: pid {old} exited')
        return 0
    if old and pid_alive(old):
        print(f'_node start {a.addr}: pid {old} already running')
        return 1
    store.mkdir(parents=True, exist_ok=True)
    if a.join:
        argv = [str(ROOT / 'efsd'), '--join', a.join, '--listen', a.addr, str(store)]
    else:
        argv = [str(ROOT / 'efsd'), a.addr, str(store)]
    argv += ['--quota', a.quota, '--discovery-port', '0']
    env = dict(os.environ, **dict(e.split('=', 1) for e in a.env))
    with open(logfile, 'ab') as log:
        log.write(f'\n==== {now()} {" ".join(argv)}\n'.encode())
        log.flush()
        proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                env=env, start_new_session=True)
    identity_path.write_text(json.dumps(node_identity(proc.pid, a.addr, store)))
    pidfile.write_text(f'{proc.pid}\n')
    host, port = a.addr.rsplit(':', 1)
    end = time.monotonic() + a.timeout
    while time.monotonic() < end:
        if proc.poll() is not None:
            tail = logfile.read_text(errors='replace').splitlines()[-5:]
            print(f'_node start {a.addr}: efsd exited rc={proc.returncode}: ' + ' | '.join(tail))
            return 1
        try:
            socket.create_connection((host, int(port)), timeout=1).close()
            print(f'_node start {a.addr}: pid {proc.pid} listening')
            return 0
        except OSError:
            time.sleep(0.5)
    print(f'_node start {a.addr}: pid {proc.pid} alive, not listening after {a.timeout}s')
    return 0 if a.timeout == 0 else 1


# --------------------------------------------------------------------------
# The A11 matrix file (scripts/qualify_matrix.json): topologies the parent
# runs with one command each, and the Stage 2 item plan.

MATRIX = ROOT / 'scripts' / 'qualify_matrix.json'
STAGE2_ITEMS = ('A1', 'A2', 'A3', 'A4', 'A5', 'A13', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6',
                'C1', 'C2', 'C3', 'C4', 'C5', 'D1', 'D2', 'D3', 'D4', 'D5', 'E1', 'E2', 'E3', 'E4')
CHECK_KINDS = ('qualify', 'b_checks', 'posix', 'live', 'code')


def matrix_errors(matrix):
    """Everything the plan names must exist: qualify gates in this driver,
    files for the other kinds, every Stage 2 item mapped, every topology
    description loadable."""
    errors = []
    plugins = default_plugins({}, [])
    known = {g.name: g for g in gates(plugins)}
    items = matrix.get('items', {})
    for item in STAGE2_ITEMS:
        if item not in items:
            errors.append(f'item {item}: not in the plan')
        elif not any(c.startswith('qualify:') for c in items[item].get('checks', [])):
            errors.append(f'item {item}: no qualify gate decides it')
    for key, value in matrix.get('fixture', {}).items():
        if key != 'about' and DEFAULT_FIXTURE.get(key) != value:
            errors.append(f'fixture {key}: {value!r} differs from the driver default {DEFAULT_FIXTURE.get(key)!r}')
    for name, topo in matrix.get('topologies', {}).items():
        for key in topo.get('fixture', {}):
            if key not in DEFAULT_FIXTURE:
                errors.append(f'topology {name}: fixture {key!r} is not a driver fixture key')
    topologies = matrix.get('topologies', {})
    for item, spec in items.items():
        for check in spec.get('checks', []):
            kind, _, target = check.partition(':')
            if kind not in CHECK_KINDS:
                errors.append(f'item {item}: check {check!r}: unknown kind')
            elif kind == 'qualify' and target not in known:
                errors.append(f'item {item}: qualify gate {target!r} does not exist')
            elif kind == 'code' and not re.search(rf'^{re.escape(target.split()[-1])}:', (ROOT / 'Makefile').read_text(), re.M):
                errors.append(f'item {item}: make target {target!r} does not exist')
            elif kind in ('b_checks', 'posix', 'live') and not (ROOT / target).exists():
                errors.append(f'item {item}: {target} does not exist')
        for t in spec.get('topologies', []):
            if t not in topologies and t not in matrix.get('cells', {}):
                errors.append(f'item {item}: topology {t!r} is not described')
        for key in ('transports', 'duration', 'prerequisites'):
            if key not in spec:
                errors.append(f'item {item}: no {key}')
    for name, topo in topologies.items():
        for gate in topo.get('gates', []) + topo.get('skip', []):
            if gate not in known:
                errors.append(f'topology {name}: gate {gate!r} does not exist')
        for kind in topo.get('allow', []):
            if kind not in DESTRUCTIVE:
                errors.append(f'topology {name}: --allow {kind!r} unknown')
        if 'loopback' in topo:
            continue
        c = topo.get('cluster')
        if not c:
            errors.append(f'topology {name}: neither loopback nor cluster')
            continue
        if len(c.get('nodes', [])) < 3:
            errors.append(f'topology {name}: fewer than 3 nodes')
        for node in c.get('nodes', []):
            if not re.fullmatch(r'[^\s:]+:\d+', node.get('addr', '')):
                errors.append(f'topology {name}: node address {node.get("addr")!r} is not HOST:PORT')
        base = int(c.get('private', {}).get('port_base', 18700))
        if any(port in RESERVED_PORTS for port in range(base, base + 44)):
            errors.append(f'topology {name}: private port base {base} overlaps reserved ports')
        if c.get('private_cluster'):
            errors += [f'topology {name}: {e}' for e in check_private_cluster(c)]
        if c.get('ssh') and not Path(ssh_program(c['ssh'])).exists():
            errors.append(f'topology {name}: ssh wrapper {c["ssh"]} does not exist')
    return errors


def topology_command(name, topo, out='OUT'):
    argv = ['python3', 'scripts/qualify.py', 'run', '--matrix', 'scripts/qualify_matrix.json',
            '--topology', name, '--out', out]
    if topo.get('live_user_cluster'):
        argv.append('--user-approved')
    return ' '.join(argv)


def print_plan(matrix, only=None):
    known = {g.name: g for g in gates(default_plugins({}, []))}
    for name, topo in matrix.get('topologies', {}).items():
        if only and name != only:
            continue
        c = topo.get('cluster', {})
        n = topo.get('loopback') or len(c.get('nodes', []))
        hosts = 1 if 'loopback' in topo else len(distinct_hosts(c))
        print(f'== {name}: {n} members on {hosts} host(s), {topo.get("transport") or c.get("transport")}; '
              f'status {"BLOCKED" if topo.get("blocked") else "runnable with the user go-ahead (live cluster)" if topo.get("live_user_cluster") else "runnable"}')
        for b in topo.get('blocked', []):
            print(f'   blocked: {b}')
        names = topo.get('gates') or list(known)
        names = [g for g in names if g not in topo.get('skip', [])]
        print(f'   gates ({len(names)}): {", ".join(names)}')
        print(f'   allow: {",".join(topo.get("allow", [])) or "none"}; '
              f'estimated duration: {topo.get("duration", "?")}')
        for key in ('covers', 'cannot_cover'):
            for line in topo.get(key, []):
                print(f'   {key.replace("_", " ")}: {line}')
        print(f'   command: {topology_command(name, topo)}')
    if only:
        return
    print('== items')
    for item, spec in matrix.get('items', {}).items():
        print(f'{item:4} {", ".join(spec.get("checks", []))}')
        print(f'     on {", ".join(spec.get("topologies", []))} x {", ".join(spec.get("transports", []))}; '
              f'{spec.get("duration")}; needs {spec.get("prerequisites")}')


def matrix_topology(a, p):
    """`run --matrix F --topology T`: the topology's description and its
    gate, skip, allow and fixture defaults (command-line values win)."""
    matrix = json.loads(a.matrix.read_text())
    topo = matrix.get('topologies', {}).get(a.topology)
    if not topo:
        p.error(f'--topology {a.topology!r}: not in {a.matrix} '
                f'(known: {", ".join(matrix.get("topologies", {}))})')
    if topo.get('blocked') and not a.ignore_blocked:
        p.error(f'topology {a.topology} is blocked: ' + '; '.join(topo['blocked']) +
                ' (fix the blocker and update the matrix, or pass --ignore-blocked)')
    if topo.get('live_user_cluster') and not a.user_approved:
        p.error(f'topology {a.topology} is the user\'s live cluster: pass --user-approved only with '
                'the user\'s go-ahead for this run (and for each destructive step it allows)')
    a.gates = a.gates or topo.get('gates', [])
    a.skip = a.skip or topo.get('skip', [])
    a.allow = a.allow or list(topo.get('allow', []))
    for gate, sec in topo.get('deadlines', {}).items():
        a.deadline.setdefault(gate, float(sec))
    a.matrix_fixture = dict({k: v for k, v in matrix.get('fixture', {}).items() if k != 'about'},
                            **topo.get('fixture', {}))
    if 'loopback' not in topo:
        topo['cluster'] = dict(topo['cluster'], fixture=dict(a.matrix_fixture, **topo['cluster'].get('fixture', {})))
    if 'loopback' in topo:
        a.loopback = topo['loopback']
        # The command line wins: another host passes its own port range.
        a.port_base = a.port_base or topo.get('port_base')
        a.private_port_base = a.private_port_base or topo.get('private_port_base')
        a.loopback_quota = a.loopback_quota or topo.get('loopback_quota')
        return None
    return dict(topo['cluster'], name=topo['cluster'].get('name', a.topology))


def parse_deadlines(items):
    out = {}
    for item in items:
        name, _, sec = item.partition('=')
        try:
            out[name] = float(sec)
        except ValueError:
            raise SystemExit(f'--deadline {item!r}: expected GATE=SECONDS')
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--cluster', type=Path)
    r.add_argument('--candidate', help='exact frozen source commit for the evidence (9-40 hexadecimal characters)')
    r.add_argument('--loopback', type=int, default=0, help='start an owned private N-daemon cluster')
    r.add_argument('--port-base', type=int, help='loopback daemon ports (N of them; default 18740 '
                   'or the topology\'s)')
    r.add_argument('--private-port-base', type=int, help='live harness ports with --loopback '
                   '(default 18700 or the topology\'s)')
    r.add_argument('--loopback-quota', help='quota of each loopback daemon (default 1G or the topology\'s)')
    r.add_argument('--out', type=Path, required=True)
    r.add_argument('--work', type=Path, help='scratch and owned stores (default OUT.work)')
    r.add_argument('--gates', type=lambda v: [x for x in v.split(',') if x], default=[])
    r.add_argument('--skip', type=lambda v: [x for x in v.split(',') if x], default=[])
    r.add_argument('--allow', type=lambda v: list(DESTRUCTIVE) if v == 'all' else [x for x in v.split(',') if x],
                   default=[])
    r.add_argument('--deadline', action='append', default=[])
    r.add_argument('--plugin', action='append', default=[])
    r.add_argument('--plan', action='store_true', help='record the commands without running them')
    r.add_argument('--keep-work', action='store_true')
    r.add_argument('--matrix', type=Path, help='A11 matrix file (scripts/qualify_matrix.json)')
    r.add_argument('--topology', help='topology of --matrix to run')
    r.add_argument('--user-approved', action='store_true',
                   help="the user approved this run on their live cluster (matrix live_user_cluster)")
    r.add_argument('--ignore-blocked', action='store_true', help='run a topology the matrix marks blocked')
    sub.add_parser('list').add_argument('--cluster', type=Path)
    pl = sub.add_parser('plan', help='validate the A11 matrix and print each topology\'s command')
    pl.add_argument('--matrix', type=Path, default=MATRIX)
    pl.add_argument('--topology')
    pl.add_argument('--check', action='store_true', help='validate only')
    nd = sub.add_parser('_node')
    nd.add_argument('op', choices=('start', 'stop', 'kill'))
    nd.add_argument('--addr', required=True)
    nd.add_argument('--store', required=True)
    nd.add_argument('--join', default='')
    nd.add_argument('--quota', default='4G')
    nd.add_argument('--env', action='append', default=[])
    nd.add_argument('--timeout', type=float, default=120)
    v = sub.add_parser('verify-sums')
    v.add_argument('dir', type=Path)
    h = sub.add_parser('_manifest')
    h.add_argument('op', choices=('write', 'verify'))
    h.add_argument('--dir')
    h.add_argument('--files', type=int, default=0)
    h.add_argument('--sizes', default='0')
    h.add_argument('--seed', default='')
    pe = sub.add_parser('_pending')
    pe.add_argument('op', choices=('write', 'verify'))
    pe.add_argument('path')
    pe.add_argument('seed')
    w = sub.add_parser('_workload')
    w.add_argument('dir')
    w.add_argument('seconds', type=float)
    w.add_argument('kib', type=int)
    w.add_argument('seed')
    ci = sub.add_parser('_client-identity')
    ci.add_argument('mount')
    b6 = sub.add_parser('_b6')
    b6.add_argument('op', choices=('write', 'fsync', 'verify'))
    b6.add_argument('--dir', required=True)
    b6.add_argument('--seed', required=True)
    b6.add_argument('--set', choices=sorted(B6_SETS), default='before')
    b6.add_argument('--names', default='')
    b6.add_argument('--bound', type=float, default=90.0)
    a = p.parse_args(argv)

    if a.cmd == '_manifest':
        return h_manifest_write(a) if a.op == 'write' else h_manifest_verify(a)
    if a.cmd == '_pending':
        return h_pending(a)
    if a.cmd == '_workload':
        return h_workload(a)
    if a.cmd == '_client-identity':
        return h_client_identity(a)
    if a.cmd == '_b6':
        return h_b6(a)
    if a.cmd == '_node':
        return h_node(a)
    if a.cmd == 'plan':
        matrix = json.loads(a.matrix.read_text())
        errors = matrix_errors(matrix)
        if a.topology and a.topology not in matrix.get('topologies', {}):
            errors.append(f'--topology {a.topology!r}: not in {a.matrix}')
        if not a.check and not errors:
            print_plan(matrix, a.topology)
        for e in errors:
            print(f'matrix error: {e}')
        print(f'matrix {a.matrix}: ' + (f'{len(errors)} error(s)' if errors else
                                         f'{len(matrix["items"])} items, {len(matrix["topologies"])} topologies OK'))
        return 1 if errors else 0
    if a.cmd == 'verify-sums':
        bad = verify_sums(a.dir)
        for b in bad:
            print(f'MISMATCH {b}')
        print('SHA256SUMS ' + ('FAIL' if bad else 'PASS'))
        return 1 if bad else 0
    if a.cmd == 'list':
        cluster = json.loads(a.cluster.read_text()) if a.cluster else {}
        for gate in gates(default_plugins(cluster, [])):
            needs = ' '.join(f'--allow {n}' for n in gate.needs + gate.partial_needs) or '-'
            print(f'{gate.name:20} {gate.items:18} needs: {needs:34} {gate.doc}')
        return 0

    if a.candidate and not re.fullmatch(r'[0-9a-f]{9,40}', a.candidate):
        p.error('--candidate must be a 9-40 digit hexadecimal commit')
    a.deadline = parse_deadlines(a.deadline)
    described = None
    if a.matrix or a.topology:
        if not (a.matrix and a.topology) or a.cluster or a.loopback:
            p.error('--matrix needs --topology and excludes --cluster and --loopback')
        described = matrix_topology(a, p)
    a.port_base = a.port_base or 18740
    a.private_port_base = a.private_port_base or 18700
    a.loopback_quota = a.loopback_quota or '1G'
    for name in a.allow:
        if name not in DESTRUCTIVE:
            p.error(f'--allow {name}: expected {",".join(DESTRUCTIVE)} or all')
    out = a.out.resolve()
    if inside_git(out):
        p.error(f'--out {out} is inside a Git work tree ({inside_git(out)}); raw outputs stay outside Git')
    if out.exists() and any(out.iterdir()):
        p.error(f'--out {out} exists and is not empty')
    if (a.cluster is not None or described is not None) == bool(a.loopback):
        p.error('give exactly one of --cluster FILE, --loopback N or --matrix with --topology')
    work = (a.work or out.with_name(out.name + '.work')).resolve()
    if inside_git(work):
        p.error(f'--work {work} is inside a Git work tree')
    if work.exists() and any(work.iterdir()):
        p.error(f'--work {work} exists and is not empty')
    loop, controller = None, None
    if a.loopback:
        if a.loopback < 3:
            p.error('--loopback needs at least 3 daemons')
        loop = controller = Loopback(a, work)
        cluster = loop.description(a.private_port_base)
        cluster['fixture'] = dict(cluster.get('fixture', {}), **getattr(a, 'matrix_fixture', {}))
    else:
        cluster = described if described is not None else json.loads(a.cluster.read_text())
        if cluster.get('private_cluster'):
            for error in check_private_cluster(cluster):
                p.error(error)
        for node in cluster.get('nodes', []):
            if not re.fullmatch(r'[^\s:]+:\d+', node.get('addr', '')):
                p.error(f'node address {node.get("addr")!r} is not HOST:PORT')
    if a.candidate:
        described_candidate = cluster.get('candidate')
        if described_candidate and not (described_candidate.startswith(a.candidate) or a.candidate.startswith(described_candidate)):
            p.error('--candidate disagrees with the cluster description')
        cluster['candidate'] = a.candidate
    base = int(cluster.get('private', {}).get('port_base', 18700))
    if any(port in RESERVED_PORTS for port in range(base, base + 40)):
        p.error(f'private port base {base} overlaps reserved ports')
    if str(cluster.get('private', {}).get('root', '')).startswith('/data1/efs'):
        p.error('private harness stores may not live under /data1/efs')
    plugins = default_plugins(cluster, a.plugin)
    names = [gate.name for gate in gates(plugins)]
    for name in a.gates + a.skip:
        if name not in names:
            p.error(f'unknown gate {name!r}; known: {", ".join(names)}')
    out.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    if controller is not None and not a.plan:
        # Tracker/LocalSearch must not crawl driver-owned FUSE fixtures.
        # Its documented exclusion marker lives on the host, never in EFS.
        # Busy mount checks still refuse every external holder they observe.
        (work / '.nomedia').touch()
    q = Qualify(a, cluster, out, work, None, plugins)
    q.control = controller or (PrivateCluster(q) if cluster.get('private_cluster') else Controller(q))
    if loop:
        q.before_sums = loop.save_logs
    try:
        if loop and not a.plan:
            loop.start()
        verdict = q.run(gates(plugins))
    finally:
        if loop:
            loop.teardown()
        if not a.keep_work and not a.plan:
            subprocess.run(delete_command(work), shell=True, capture_output=True)
        elif a.plan:
            subprocess.run(delete_command(work), shell=True, capture_output=True)
    print(f'qualify: {verdict}; raw {out}; summary {out / "summary-draft.md"}', flush=True)
    return {PASS: 0, FAIL: 1, NOTRUN: 3}[verdict]


if __name__ == '__main__':
    sys.exit(main())
