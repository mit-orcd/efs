#!/usr/bin/env python3
"""D39 admission with real efsd processes on loopback (make test-long).

Startup (C1/C2, W153a): a malformed configuration, an advertised endpoint
that is no member record, the removed --node-id/--join and a set
EFS_MD_RAFT_N each stop efsd before it listens; a second process on a
member's store stops "owned".

Formation (C10, D40): raft-mkfs needs --cluster, is refused while a
configured member is absent (naming it), then forms groups 0 and 2 with
one pair; a client is refused before formation and served after; a member
started with a different configuration is refused by every peer and never
learns the pair. A full restart re-derives formation from the applied
records before serving clients.

Ports: EFS_D39_PORT_BASE (default 18640) .. +5. Scratch under TMPDIR,
removed on success, kept and printed on failure.
"""
from pathlib import Path
import os, shutil, signal, socket, subprocess, sys, tempfile, time

root = Path(__file__).resolve().parents[1]
efsd, mgmt = root / 'efsd', root / 'efs-mgmt'
base = int(os.environ.get('EFS_D39_PORT_BASE', '18640'))
work = Path(tempfile.mkdtemp(prefix='efs-d39-daemons-'))
procs = []
passed = False


def conf_text(ports):
    return ''.join('%d 127.0.0.1:%d\n' % (i + 1, p) for i, p in enumerate(ports))


def start(conf, port, store, env=None, extra=()):
    store.mkdir(parents=True, exist_ok=True)
    log = (work / ('efsd-%d.log' % port)).open('a')
    p = subprocess.Popen([str(efsd), '--cluster', str(conf), '--addr', '127.0.0.1', '--port', str(port),
                          '--storage', str(store), '--quota', '1G', *extra],
                         stdout=log, stderr=subprocess.STDOUT, env=env or os.environ.copy(),
                         start_new_session=True)
    procs.append(p)
    deadline = time.monotonic() + 30
    while True:
        assert p.poll() is None, 'efsd :%d exited: %s' % (port, tail(port))
        try:
            with socket.create_connection(('127.0.0.1', port), 0.5):
                return p
        except OSError:
            assert time.monotonic() < deadline, 'efsd :%d did not listen: %s' % (port, tail(port))
            time.sleep(0.1)


def tail(port):
    path = work / ('efsd-%d.log' % port)
    return path.read_text()[-2500:] if path.exists() else ''


def stop(p):
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(60)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(10)


def refused(args, env=None, expect=''):
    r = subprocess.run([str(efsd), *args], capture_output=True, text=True, timeout=30,
                       env=env or os.environ.copy())
    text = r.stdout + r.stderr
    assert r.returncode != 0 and 'listening' not in text, (args, r.returncode, text[-600:])
    assert expect in text, (expect, text[-600:])
    return text


def mgmt_run(*args, conf=None, timeout=90):
    cmd = [str(mgmt)] + (['--cluster', str(conf)] if conf else []) + list(args)
    env = os.environ.copy()
    env.pop('EFS_CLUSTER_CONF', None)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)


try:
    ports = [base + i for i in range(3)]
    conf = work / 'cluster.conf'
    conf.write_text('# D39 test\n' + conf_text(ports))
    store = [work / ('n%d' % (i + 1)) for i in range(3)]

    # --- startup refusals (nothing listens, nothing is created) ---
    bad = work / 'bad.conf'
    bad.write_text('1 127.0.0.1:%d\n2 127.0.0.1:%d\n' % (ports[0], ports[1]))
    refused(['--cluster', str(bad), '--addr', '127.0.0.1', '--port', str(ports[0]),
             '--storage', str(work / 'x')], expect='cluster configuration refused')
    refused(['--cluster', str(conf), '--addr', '127.0.0.1', '--port', str(base + 9),
             '--storage', str(work / 'x')], expect='is not a member record')
    refused(['--cluster', str(conf), '--addr', '127.000.0.1', '--port', str(ports[0]),
             '--storage', str(work / 'x')], expect='is not a member record')
    refused(['--node-id', '1', '--addr', '127.0.0.1', '--port', str(ports[0]),
             '--storage', str(work / 'x')], expect='--node-id was removed (D39)')
    refused(['--cluster', str(conf), '--addr', '127.0.0.1', '--port', str(ports[0]),
             '--storage', str(work / 'x'), '--join', '127.0.0.1:1'], expect='--join was removed (D39)')
    env = dict(os.environ, EFS_MD_RAFT_N='3')
    refused(['--cluster', str(conf), '--addr', '127.0.0.1', '--port', str(ports[0]),
             '--storage', str(work / 'x')], env=env, expect='EFS_MD_RAFT_N was removed')
    assert not (work / 'x').exists(), 'a refused start created nothing'
    print('d39 daemons: malformed list, unconfigured endpoint, --node-id, --join, EFS_MD_RAFT_N refused before serving PASS', flush=True)

    # --- formation ---
    p1 = start(conf, ports[0], store[0])
    p2 = start(conf, ports[1], store[1])
    owned = refused(['--cluster', str(conf), '--addr', '127.0.0.1', '--port', str(ports[0]),
                     '--storage', str(store[0])], expect='owned by another running process')
    r = mgmt_run('status', '127.0.0.1:%d' % ports[0])
    assert r.returncode != 0, ('a client is not served before formation', r.stdout, r.stderr)
    r = mgmt_run('raft-mkfs', '127.0.0.1:%d' % ports[0])
    assert r.returncode != 0 and '--cluster' in r.stderr, r.stderr
    r = mgmt_run('raft-mkfs', '127.0.0.1:%d' % ports[0], '--wait', '3', conf=conf)
    assert r.returncode != 0 and 'absent configured members: 3 (127.0.0.1:%d)' % ports[2] in r.stderr, \
        (r.stdout, r.stderr)
    # Member 3 started with a different list (a fourth member added; its own
    # record unchanged) is refused by every peer and never forms
    # (s5_different_lists_cannot_form).
    other = work / 'other.conf'
    other.write_text(conf_text(ports + [ports[2] + 3]))
    odd_port = ports[2]
    p3x = start(other, odd_port, work / 'odd')
    time.sleep(3)
    r = mgmt_run('raft-mkfs', '127.0.0.1:%d' % ports[0], '--wait', '3', conf=conf)
    assert r.returncode != 0 and 'absent configured members' in r.stderr, r.stderr
    stop(p3x)
    assert 'configuration digest' in tail(ports[0]) and 'configuration digest' in tail(ports[1]), \
        'peers name the digest refusal'
    assert 'formation pair' not in tail(odd_port), 'the mismatched member never learned a pair'
    p3 = start(conf, ports[2], store[2])
    r = mgmt_run('raft-mkfs', '127.0.0.1:%d' % ports[1], '--wait', '60', conf=conf)
    assert r.returncode == 0 and 'formation pair salt=' in r.stdout, (r.stdout, r.stderr)
    salt = r.stdout.split('formation pair salt=')[1].split()[0]
    for g in ('0', '2'):
        f = mgmt_run('formation-read', g, conf=conf)
        assert f.returncode == 0 and 'salt=%s' % salt in f.stdout, (g, f.stdout, f.stderr)
    deadline = time.monotonic() + 30
    while True:
        r = mgmt_run('raft-create', '127.0.0.1:%d' % ports[2], '1', 'd39-file')
        if r.returncode == 0 and 'status=0' in r.stdout:
            break
        assert time.monotonic() < deadline, ('client served after formation', r.stdout, r.stderr)
        time.sleep(0.5)
    r = mgmt_run('raft-mkfs', '127.0.0.1:%d' % ports[2], conf=conf)
    assert r.returncode == 0 and 'salt=%s' % salt in r.stdout, ('a repeated mkfs returns the committed pair', r.stdout)
    print('d39 daemons: client refused unformed; mkfs needs --cluster and every member; mismatched member refused; formed with one pair; repeated mkfs keeps it PASS', flush=True)

    # --- restart: re-derive before service, incarnation advances ---
    for p in (p1, p2, p3):
        stop(p)
    p1, p2, p3 = (start(conf, ports[i], store[i]) for i in range(3))
    deadline = time.monotonic() + 30
    while True:
        r = mgmt_run('raft-create', '127.0.0.1:%d' % ports[0], '1', 'd39-after-restart')
        if r.returncode == 0 and 'status=0' in r.stdout:
            break
        assert time.monotonic() < deadline, ('served after restart', r.stdout, r.stderr)
        time.sleep(0.5)
    for i, p in enumerate(ports):
        t = (work / ('efsd-%d.log' % p)).read_text()
        assert 'store formed' in t, t[-800:]
        incs = [int(line.split('incarnation ')[1].split(',')[0]) for line in t.splitlines()
                if ', incarnation ' in line]
        assert len(incs) >= 2 and incs[-1] > incs[-2], ('incarnation advances', incs)
    for p in (p1, p2, p3):
        stop(p)
    print('d39 daemons: restart re-derives formation from the applied records, incarnation advances, clients served PASS', flush=True)
    passed = True
finally:
    for p in procs:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            p.wait(10)
    if passed:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('d39 daemons: scratch kept at %s' % work, file=sys.stderr)
