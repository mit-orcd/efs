"""Task GD helpers for the controller scripts that run on the Mac and reach the fcstor hosts through task_gd_ssh.sh."""
import json, re, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SSH = str(HERE / 'task_gd_ssh.sh')


def ssh(host, cmd, stdin=None, timeout=300):
    r = subprocess.run([SSH, host, cmd], input=stdin, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def mgmt(cl, source, *args):
    return ssh(cl['mgmt_host'], ' '.join([f"{source}/efs-mgmt", *map(str, args)]), timeout=60)[:2]


def leaders(cl, source):
    """{group: (node index, addr, term)} from every node's own raft-status."""
    out = {}
    for i, n in enumerate(cl['nodes']):
        rc, text = mgmt(cl, source, 'raft-status', n['addr'])
        for m in re.finditer(r'group (\d+) hosted=1 role=(\w+) leader=-?\d+ term=(\d+)([^\n]*)', text):
            if 'retired=1' in m.group(4) or m.group(2) != 'LEADER':
                continue
            g, t = int(m.group(1)), int(m.group(3))
            if t >= out.get(g, (0, '', -1))[2]:
                out[g] = (i, n['addr'], t)
    return out


def member_pid(node):
    rc, out, _ = ssh(node['host'], 'for p in $(pgrep -u $(id -u) -x efsd); do tr "\\0" " " </proc/$p/cmdline | '
                     f'grep -q -- "{node["store"]}" && echo $p; done')
    pids = out.split()
    if len(pids) != 1:
        raise SystemExit(f'member pid of {node["addr"]}: {pids!r}')
    return int(pids[0])


def collect(cl, out):
    """Copy each host's daemon and client logs of the driver-owned cluster into OUT/logs/<host>/ (they die with the teardown)."""
    import base64, io, tarfile
    root = (cl.get('private_cluster') or {}).get('root', '/tmp/efs-taskGD/cluster')
    for h in sorted({n['host'] for n in cl['nodes']} | {m['host'] for m in cl['mounts']}):
        rc, text, _ = ssh(h, f'cd {root} && tar -czf - *.log 2>/dev/null | base64 -w0')
        try:
            d = Path(out) / 'logs' / h; d.mkdir(parents=True, exist_ok=True)
            tarfile.open(fileobj=io.BytesIO(base64.b64decode(text)), mode='r:gz').extractall(d)
        except Exception as e:
            (Path(out) / 'logs' / f'{h}.error').write_text(str(e))


def finish(gate_dir, verdict, reason, doc):
    d = Path(gate_dir)
    (d / 'result.json').write_text(json.dumps(dict(doc, verdict=verdict, reason=reason), indent=1))
    (d / 'plugin-result.json').write_text(json.dumps(dict(verdict=verdict, reason=reason, fixture=dict(
        (k, v) for k, v in doc.items() if k in ('summary',))), indent=1))
    print(f'{verdict}: {reason}')
    sys.exit(0 if verdict == 'PASS' else 1)
