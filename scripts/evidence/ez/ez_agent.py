#!/usr/bin/env python3
"""Task EZ: per-host process agent for multi-host soaks without host-to-host ssh.

The fcstor hosts share an NFS home but refuse ssh between each other, so a
controller on one host drives the other hosts' daemons through this agent,
run in screen on every host:

  python3 ez_agent.py CTL HOST

Commands are JSON lines the controller appends to CTL/HOST/cmd.jsonl (one
writer); results are JSON lines this agent appends to CTL/HOST/res.jsonl,
state (each owned process: pid, exit status, VmRSS) is rewritten to
CTL/HOST/state.json every 0.5 s. Files are reopened on every poll, so NFS
close-to-open consistency makes the other host's appends visible.

  spawn  {name, argv, env, log, cwd}  start a process in its own session;
                                      `==== start` header in the log first
  signal {name, sig}                  signal an owned process; records the time
  run    {argv, timeout}              run a short command, return rc and output

Only processes this agent spawned are ever signalled. CTL/HOST/stop ends the
agent: it SIGKILLs the owned processes still running, then exits.
"""
import json, os, re, signal, subprocess, sys, time
from pathlib import Path


def rss_kb(pid):
    try:
        m = re.search(r'^VmRSS:\s+(\d+) kB', Path(f'/proc/{pid}/status').read_text(), re.M)
        return int(m.group(1)) if m else None
    except OSError:
        return None


def main(ctl, host):
    d = Path(ctl) / host
    d.mkdir(parents=True, exist_ok=True)
    cmd, res, state = d / 'cmd.jsonl', d / 'res.jsonl', d / 'state.json'
    cmd.touch()
    res.touch()
    offset = cmd.stat().st_size      # a restarted agent never replays old commands
    procs = {}                       # name -> Popen

    def answer(c, **out):
        with open(res, 'a') as f:
            f.write(json.dumps(dict(id=c.get('id'), t=time.time(), **out)) + '\n')

    while not (d / 'stop').exists():
        with open(cmd, 'rb') as f:
            f.seek(offset)
            data = f.read()
        end = data.rfind(b'\n') + 1          # only whole lines
        offset += end
        for line in data[:end].splitlines():
            c = json.loads(line)
            try:
                if c['op'] == 'spawn':
                    old = procs.get(c['name'])
                    assert old is None or old.poll() is not None, f"{c['name']} still running"
                    with open(c['log'], 'a') as log:
                        log.write(f"\n==== start {time.time():.3f} {' '.join(c['argv'])}\n")
                        log.flush()
                        p = subprocess.Popen(c['argv'], stdout=log, stderr=subprocess.STDOUT,
                                             stdin=subprocess.DEVNULL, cwd=c.get('cwd'),
                                             env=dict(os.environ, **c.get('env', {})),
                                             start_new_session=True)
                    procs[c['name']] = p
                    answer(c, ok=True, pid=p.pid)
                elif c['op'] == 'signal':
                    p = procs[c['name']]
                    alive = p.poll() is None
                    if alive:
                        p.send_signal(c['sig'])
                    answer(c, ok=True, pid=p.pid, was_alive=alive, sig=c['sig'])
                elif c['op'] == 'run':
                    r = subprocess.run(c['argv'], capture_output=True, text=True,
                                       timeout=c.get('timeout', 60))
                    answer(c, ok=True, rc=r.returncode, output=(r.stdout + r.stderr)[-20000:])
                else:
                    answer(c, ok=False, error=f"unknown op {c['op']}")
            except Exception as e:  # noqa: BLE001 - reported to the controller
                answer(c, ok=False, error=repr(e))
        snap = {n: dict(pid=p.pid, rc=p.poll(), rss_kb=rss_kb(p.pid) if p.returncode is None else None)
                for n, p in procs.items()}
        tmp = d / f'state.json.{os.getpid()}'
        tmp.write_text(json.dumps(dict(t=time.time(), host=host, agent=os.getpid(), procs=snap)))
        os.replace(tmp, state)
        time.sleep(0.5)
    for p in procs.values():
        if p.poll() is None:
            p.kill()
            p.wait()
    answer({'id': 'stop'}, ok=True, stopped=sorted(procs))
    return 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
