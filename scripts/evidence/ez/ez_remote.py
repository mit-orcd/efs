"""Task EZ: controller side of ez_agent.py (multi-host daemons over a shared home).

Agent        appends commands to CTL/HOST/cmd.jsonl and waits for the answer.
RemoteProc   the Popen surface k33_daemons.Cluster uses (poll, wait,
             send_signal, kill, pid, returncode) for an agent's process.
RemoteCluster  k33_daemons.Cluster whose members run on other hosts: start()
             spawns through the member's agent, logs are written by the agent
             into a shared-home directory and read from there.
"""
import json, os, re, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
import k33_daemons as K  # noqa: E402


class Agent:
    def __init__(self, ctl, host):
        self.d = Path(ctl) / host
        self.host = host
        self.mu = threading.Lock()
        self.n = 0
        self.res_off = 0
        self.answers = {}
        self.cached = (0.0, None)

    def state(self, max_age=0.2):
        t, st = self.cached
        if time.monotonic() - t > max_age:
            st = json.loads((self.d / 'state.json').read_text())
            assert time.time() - st['t'] < 20, f'agent on {self.host} silent for {time.time()-st["t"]:.0f}s'
            self.cached = (time.monotonic(), st)
        return st

    def call(self, op, wait=120, **kw):
        with self.mu:
            self.n += 1
            cid = f'{os.getpid()}-{time.time_ns()}-{self.n}'
            with open(self.d / 'cmd.jsonl', 'a') as f:
                f.write(json.dumps(dict(id=cid, op=op, **kw)) + '\n')
            end = time.monotonic() + wait
            while time.monotonic() < end:
                try:
                    with open(self.d / 'res.jsonl', 'rb') as f:
                        f.seek(self.res_off)
                        data = f.read()
                except FileNotFoundError:   # before the agent's first answer
                    data = b''
                cut = data.rfind(b'\n') + 1
                self.res_off += cut
                for line in data[:cut].splitlines():
                    r = json.loads(line)
                    self.answers[r['id']] = r
                if cid in self.answers:
                    r = self.answers.pop(cid)
                    assert r.get('ok'), f'{self.host} {op}: {r}'
                    return r
                time.sleep(0.1)
            raise AssertionError(f'{self.host} {op}: no answer in {wait}s')

    def run(self, argv, timeout=60):
        r = self.call('run', wait=timeout + 30, argv=[str(a) for a in argv], timeout=timeout)
        return r['rc'], r['output']


class RemoteProc:
    def __init__(self, agent, name, pid):
        self.agent, self.name, self.pid = agent, name, pid
        self.returncode = None

    def poll(self):
        if self.returncode is None:
            st = self.agent.state()['procs'].get(self.name)
            if st and st['pid'] == self.pid:
                self.returncode = st['rc']
        return self.returncode

    def rss_kb(self):
        st = self.agent.state()['procs'].get(self.name)
        return st and st['pid'] == self.pid and st['rss_kb']

    def wait(self, timeout=None):
        end = None if timeout is None else time.monotonic() + timeout
        while self.poll() is None:
            if end is not None and time.monotonic() > end:
                raise subprocess.TimeoutExpired(self.name, timeout)
            time.sleep(0.2)
        return self.returncode

    def send_signal(self, sig):
        return self.agent.call('signal', name=self.name, sig=int(sig))

    def kill(self):
        return self.send_signal(9)


class RemoteCluster(K.Cluster):
    """placement: {i: (host, addr, port)}. Stores under store_root on each
    member's host (local disk); logs under logdir (shared home)."""

    def __init__(self, work, name, placement, env, ctl, logdir, efsd, store_root):
        super().__init__(work, name, sorted(placement), env,
                         hosts={i: a for i, (_, a, _) in placement.items()})
        for h in self.held:
            h.close()
        self.held = []
        self.ports = {i: p for i, (_, _, p) in placement.items()}
        self.where = {i: h for i, (h, _, _) in placement.items()}
        self.agents = {h: Agent(ctl, h) for h in set(self.where.values())}
        self.logdir, self.efsd, self.store_root = Path(logdir), str(efsd), store_root
        self.logdir.mkdir(parents=True, exist_ok=True)

    def agent(self, i):
        return self.agents[self.where[i]]

    def start(self, i, join=None, store=None, extra_env=None, listen=True, args=(), quota='1G'):
        store = f'{self.store_root}/{store or f"n{i}"}'
        rc, out = self.agent(i).run(['mkdir', '-p', store])
        assert rc == 0, out
        if join is None:
            argv = [self.efsd, self.ep(i), store]
        else:
            argv = [self.efsd, '--join', self.ep(join) if isinstance(join, int) else join]
            if listen:
                argv += ['--listen', self.ep(i)]
            argv += [store]
        if quota:
            argv += ['--quota', quota]
        argv += ['--writers', '2', '--discovery-port', str(self.disc_port), *args]
        self.expected.discard(i)
        if self._wd is None:
            self._wd = threading.Thread(target=self._watch, daemon=True)
            self._wd.start()
        env = dict(self.env, **(extra_env or {}))
        name = f'{Path(self.store_root).name}-n{i}'   # unique per run on a shared agent
        r = self.agent(i).call('spawn', name=name, argv=argv, env=env, log=str(self.logdir / f'n{i}.log'))
        self.procs[i] = RemoteProc(self.agent(i), name, r['pid'])
        return self.procs[i]

    def log(self, i):
        p = self.logdir / f'n{i}.log'
        return p.read_text(errors='replace') if p.exists() else ''

    def others(self, i):
        return ''
