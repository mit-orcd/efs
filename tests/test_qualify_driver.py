#!/usr/bin/env python3
"""Stage 2 driver bookkeeping (scripts/qualify.py) with stub gates.

Stub plugin gates PASS, FAIL, NOTRUN (exit 77, missing script, destructive
step not allowed), overrun their deadline, or claim a verdict their exit
contradicts. Checks every result.json field the evidence summary needs, the
exit status, SHA256SUMS (and that tampering is caught: negative control),
the summary draft and the F3 row, the refusal of an output directory inside
Git, and --plan command lines for the real gates. Nothing touches a cluster.
"""
import contextlib, importlib.util, io, json, os, shutil, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('qualify', root / 'scripts/qualify.py')
Q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(Q)


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        try:
            rc = Q.main(['run', *argv])
        except SystemExit as exc:
            rc = exc.code
    return rc, out.getvalue()


def load(path):
    return json.loads(Path(path).read_text())


# Pure helpers.
assert 'edelete --force --delete' in Q.delete_command('/tmp/private tree')
assert "'/tmp/private tree'" in Q.delete_command('/tmp/private tree')
assert 'rm -rf' not in (root / 'scripts/qualify.py').read_text()
assert Q.identity_key('efsd v1-g123 (branch devel, built today on n1)') == 'v1-g123 (branch devel)'
assert Q.identity_key('efs-fuse v1-g123 (branch devel, built now on n2)') == 'v1-g123 (branch devel)'
assert Q.identity_key('efsd v1-g123-dirty (branch devel, built x)') != 'v1-g123 (branch devel)'
assert Q.outcome_from_rc(0, 'x')[0] == 'PASS'
assert Q.outcome_from_rc(77, 'NOTRUN: no RDMA\n') == ('NOTRUN', 'NOTRUN: no RDMA')
assert Q.outcome_from_rc(1, 'ok\nFAIL: torn read\nbye')[1] == 'exit 1: FAIL: torn read'
assert Q.outcome_from_rc(124, '', timed_out=True) == ('FAIL', 'deadline exceeded')
assert Q.count_marks('test\tresult\tdetail\na\tPASS\t\nb\tFAIL\tx\nc\tSKIP\t\nd\tPASS\t\n') == \
    {'PASS': 2, 'FAIL': 1, 'SKIP': 1}
assert Q.worst(['PASS', 'NOTRUN']) == 'NOTRUN' and Q.worst(['NOTRUN', 'FAIL', 'PASS']) == 'FAIL'
start_identity = dict(daemons=[dict(key='candidate')], clients=[dict(key='candidate')])
assert Q.identity_change_reason(start_identity, start_identity) == ''
assert 'not recorded' in Q.identity_change_reason(None, start_identity)
assert 'unreadable' in Q.identity_change_reason(start_identity,
    dict(daemons=[dict(key=None)], clients=[dict(key='candidate')]))
assert 'changed at the end' in Q.identity_change_reason(start_identity,
    dict(daemons=[dict(key='replacement')], clients=[dict(key='candidate')]))
assert 'replaced' in Q.identity_change_reason(start_identity,
    dict(daemons=[dict(key='candidate')], clients=[dict(key='candidate', exe_deleted=True)]))
assert 'count changed' in Q.identity_change_reason(start_identity, dict(daemons=[], clients=[]))
views = [('a', 'FOLLOWER', 1, 3), ('b', 'LEADER', 1, 3), ('c', 'FOLLOWER', 1, 3)]
assert Q.agreed(views, 1, 3) and not Q.agreed(views, 2, 3) and not Q.agreed(views, 1, 4)
assert not Q.agreed(views[:2] + [('c', 'CANDIDATE', 1, 3)], 1, 3) and not Q.agreed([], 1, 3)
three = dict(nodes=[dict(addr=f'10.0.0.{i}:17432', host=f'h{i}') for i in (1, 2, 3)])
assert Q.topology_column(three) == '3 physical nodes'
shared = dict(nodes=[dict(addr=f'10.0.0.1:{p}', host='h1') for p in (1, 2, 3, 4, 5)])
assert Q.topology_column(shared).startswith('>4 physical nodes (N=5) — NOT distinct hosts (1 host')

# Task V: failed cleanup is observable and never deletes live stores.
from types import SimpleNamespace
from unittest.mock import Mock
for failed_stage in ('teardown-unmount', 'stop'):
    calls = []
    qcleanup = SimpleNamespace(cluster=dict(private_cluster=dict(root='/tmp/owned'),
        nodes=[dict(host='n1', addr='10.0.0.1:19340', store='/tmp/owned/n1')],
        mounts=[dict(host='n1', path='/tmp/owned/mnt')]), source=lambda h: '/src')
    control = Q.PrivateCluster(qcleanup)
    def cleanup_run(label, host, cmd, deadline):
        calls.append(label)
        return (1 if label == failed_stage else 0), ''
    gcleanup = SimpleNamespace(run=cleanup_run, fetch=Mock())
    control.stop_cluster = lambda g: (1 if failed_stage == 'stop' else 0, '')
    assert control.owned_teardown(gcleanup)[0] == 1
    assert 'teardown-delete' not in calls
    if failed_stage == 'teardown-unmount':
        gcleanup.fetch.assert_not_called()

# Distinct client sources must be used on both serial and concurrent steps.
pspec = importlib.util.spec_from_file_location('posix_remote_paths', root / 'tests/posix/posix_2client.py')
P = importlib.util.module_from_spec(pspec)
pspec.loader.exec_module(P)
remote_calls = []
def fake_ssh(ssh, host, cmd):
    remote_calls.append((host, cmd))
    return 0, ('OK' if cmd.startswith('test -d') else 'RESULT\tPASS\t'), ''
with __import__('unittest.mock', fromlist=['patch']).patch.object(P, 'ssh_cmd', fake_ssh):
    P._remote_ab('ssh', ['/source-a/suite.py', '/source-b/suite.py'], 'a', 'b', 'case', 0, '/mnt/case')
    assert any(h == 'a' and '/source-a/suite.py' in c for h,c in remote_calls)
    assert any(h == 'b' and '/source-b/suite.py' in c for h,c in remote_calls)
    remote_calls.clear()
    P.TESTS = [('case', [('a', None), ('b', None)], '')]
    with contextlib.redirect_stdout(io.StringIO()):
        assert P.run_remote('a', 'b', '/mnt', 'ssh', ['/source-a/suite.py', '/source-b/suite.py'], None, '') == 0
    assert any(h == 'a' and '/source-a/suite.py' in c for h,c in remote_calls)
    assert any(h == 'b' and '/source-b/suite.py' in c for h,c in remote_calls)

tmp = Path(tempfile.mkdtemp(prefix='qualify-test-'))
try:
    liar = tmp / 'liar.sh'
    liar.write_text('printf \'{"verdict": "PASS"}\' > "$1/plugin-result.json"\nexit 1\n')
    lower = tmp / 'lower.sh'
    lower.write_text('printf \'{"verdict": "FAIL", "reason": "3 fragments on one node", '
                     '"fixture": {"files": 9}}\' > "$1/plugin-result.json"\n')
    cluster = dict(name='stub', source=str(root), nodes=[], mounts=[], plugins={
        'stub-pass': dict(command='echo PASS one'),
        'stub-fail': dict(command="echo 'FAIL: wrong bytes'; exit 1"),
        'stub-notrun': dict(command="echo 'NOTRUN: no RDMA device'; exit 77"),
        'stub-missing': dict(command='true', requires='{source}/scripts/no-such-b-checks.py'),
        'stub-liar': dict(command=f'bash {liar} {{gate_dir}}'),
        'stub-lower': dict(command=f'bash {lower} {{gate_dir}}'),
        'stub-slow': dict(command='sleep 20', deadline_s=0.3),
        'stub-destructive': dict(command='echo PASS restarted', needs=['restart']),
    })
    desc = tmp / 'cluster.json'
    desc.write_text(json.dumps(cluster))
    stubs = ','.join(cluster['plugins'])

    # Immediate mismatch evidence survives even if a later read is correct.
    hspec = importlib.util.spec_from_file_location('stage2_helpers', root / 'scripts/stage2_helpers.py')
    H = importlib.util.module_from_spec(hspec)
    hspec.loader.exec_module(H)
    # K52/K10: queries must use explicit posix_spawn even while mounted-I/O
    # threads exist. Prohibit both Python fork entry points; check outputs,
    # argument boundaries, nonzero status, timeout and child reaping.
    from unittest.mock import patch
    commands = tmp / 'spawn queries' / '.efs'
    commands.mkdir(parents=True)
    (commands / 'stats').write_text(
        'import sys,time\n'
        'if sys.argv[1] == "sleep": time.sleep(30)\n'
        'sys.stdout.buffer.write(b"x" * 131072 + sys.argv[1].encode())\n'
        'sys.stderr.write("diagnostic")\n'
        'sys.exit(2 if sys.argv[1] == "refuse" else 0)\n')
    spawned = []
    real_spawn = os.posix_spawn
    def tracked_spawn(*args, **kwargs):
        pid = real_spawn(*args, **kwargs)
        spawned.append(pid)
        return pid
    with patch.object(os, 'posix_spawn', tracked_spawn), \
         patch.object(os, 'fork', side_effect=AssertionError('fork is unsafe')), \
         patch.object(H.subprocess, '_fork_exec', side_effect=AssertionError('fork_exec is unsafe'), create=True):
        arg = 'literal ; $(false) space'
        rc, stdout, stderr, elapsed = H.efs_cmd(commands.parent, 'stats', arg)
        assert rc == 0 and stdout == b'x' * 131072 + arg.encode() and stderr == 'diagnostic'
        assert H.efs_cmd(commands.parent, 'stats', 'refuse')[0] == 2
        rc, stdout, stderr, elapsed = H.efs_cmd(commands.parent, 'stats', 'sleep', timeout=.03)
        assert rc == 124 and stderr == 'deadline' and elapsed < 2
    assert len(spawned) == 3
    for pid in spawned:
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass
        else:
            raise AssertionError('query child was not reaped')

    mismatch = tmp / 'mismatch'
    mismatch.write_bytes(b'bad')
    try:
        H.verify_payload(mismatch, b'expected')
        raise AssertionError('wrong payload accepted')
    except OSError as exc:
        original = dict(exc.payload_detail)
        assert exc.errno == 5 and original['path'] == str(mismatch)
        assert original['observed_len'] == 3 and original['expected_len'] == 8
        assert original['observed_sha256'] != original['expected_sha256']
    mismatch.write_bytes(b'expected')
    H.verify_payload(mismatch, b'expected')
    assert original['observed_len'] == 3, 'later success must not rewrite original failure evidence'

    # Cleanup failure lowers the run and preserves even controller scratch.
    from unittest.mock import patch
    qstop = object.__new__(Q.Qualify)
    qstop.out = tmp / 'cleanup-failure'
    qstop.cluster, qstop.allow, qstop.before_sums = {}, set(), None
    qstop.control = SimpleNamespace(owned=True)
    qstop.args = SimpleNamespace(keep_work=False)
    qstop.setup = lambda results: None
    qstop.run_gates = lambda gates, results, failed: results.append(dict(
        gate='work', verdict='PASS', reason='', destructive_steps=[]))
    qstop.teardown = lambda: dict(gate='_teardown', verdict='FAIL', reason='busy mount', destructive_steps=[])
    qstop.cleanup_scratch = Mock()
    with patch.object(Q, 'write_sums', return_value={}), patch.object(Q, 'write_summary'):
        assert qstop.run([]) == 'FAIL'
    qstop.cleanup_scratch.assert_not_called()
    assert qstop.args.keep_work

    # A failed final identity read must lower the aggregate verdict, not be
    # an unconsumed artifact after the last passing gate.
    end_out = tmp / 'end-identity'
    (end_out / 'gates').mkdir(parents=True)
    q = object.__new__(Q.Qualify)
    q.out, q.plan, q.cluster = end_out, False, dict(nodes=[dict(addr='stub')])
    q.identity_start = start_identity
    q.selected = lambda _: []
    q.read_identity = lambda *a, **k: dict(daemons=[dict(key='new-build')], clients=[dict(key='candidate')])
    results = []
    q.run_gates([], results, None)
    assert len(results) == 1 and results[0]['gate'] == '_identity-end'
    assert results[0]['verdict'] == 'FAIL' and 'changed at the end' in results[0]['reason']
    assert load(end_out / 'gates/_identity-end/result.json')['verdict'] == 'FAIL'

    # 1. Every outcome; no destructive step allowed.
    out1 = tmp / 'out1'
    t0 = time.monotonic()
    rc, text = run('--cluster', str(desc), '--out', str(out1), '--gates', stubs)
    assert rc == 1, (rc, text)
    assert time.monotonic() - t0 < 10, 'deadline did not stop the slow stub'
    report = load(out1 / 'run.json')
    want = {'stub-pass': 'PASS', 'stub-fail': 'FAIL', 'stub-notrun': 'NOTRUN', 'stub-missing': 'NOTRUN',
            'stub-liar': 'FAIL', 'stub-lower': 'FAIL', 'stub-slow': 'FAIL', 'stub-destructive': 'NOTRUN'}
    assert {g['gate']: g['verdict'] for g in report['gates']} == want, report['gates']
    assert report['verdict'] == 'FAIL' and report['counts'] == dict(PASS=1, FAIL=4, NOTRUN=3)
    assert report['destructive_steps'] == [] and report['allow'] == []
    for name in want:
        r = load(out1 / 'gates' / name / 'result.json')
        for key in ('gate', 'items', 'verdict', 'reason', 'start', 'end', 'deadline_s', 'commands',
                    'fixture', 'raw', 'destructive_steps'):
            assert key in r, (name, key)
        for cmd in r['commands']:
            for key in ('command', 'host', 'start', 'end', 'deadline_s', 'rc', 'output'):
                assert key in cmd, (name, key)
            assert (out1 / 'gates' / name / cmd['output']).is_file()
        assert r['verdict'] == 'PASS' or r['reason'], name
    reasons = {g['gate']: g['reason'] for g in report['gates']}
    assert reasons['stub-notrun'] == 'NOTRUN: no RDMA device'
    assert 'no-such-b-checks.py not present' in reasons['stub-missing']
    assert reasons['stub-destructive'] == 'destructive step not allowed: needs --allow restart'
    assert reasons['stub-fail'] == 'exit 1: FAIL: wrong bytes'
    assert reasons['stub-slow'] == 'deadline exceeded'
    assert reasons['stub-liar'].startswith('plugin claims PASS but exit 1')
    assert reasons['stub-lower'] == '3 fragments on one node'
    assert load(out1 / 'gates/stub-lower/result.json')['fixture'] == {'files': 9}
    slow = load(out1 / 'gates/stub-slow/result.json')['commands'][0]
    assert slow['timed_out'] and slow['deadline_s'] == 0.3 and slow['seconds'] < 5, slow
    assert load(out1 / 'gates/stub-pass/result.json')['commands'][0]['command'] == 'echo PASS one'
    # SHA256SUMS covers the raw files; tampering and an unlisted file are caught.
    assert Q.verify_sums(out1) == []
    listed = (out1 / 'SHA256SUMS').read_text()
    assert 'gates/stub-fail/01-stub-fail.log' in listed and 'run.json' in listed
    assert 'summary-draft.md' not in listed
    log = out1 / 'gates/stub-pass/01-stub-pass.log'
    log.write_text(log.read_text() + 'edited\n')
    (out1 / 'gates/stub-pass/extra.txt').write_text('x')
    assert sorted(Q.verify_sums(out1)) == ['gates/stub-pass/01-stub-pass.log',
                                           'gates/stub-pass/extra.txt (not listed)']
    with contextlib.redirect_stdout(io.StringIO()):
        assert Q.main(['verify-sums', str(out1)]) == 1
    summary = (out1 / 'summary-draft.md').read_text()
    for heading in ('## Purpose and verdict', '## Candidate and build identity', '## Environment',
                    '## Commands', '## Destructive steps', '## Results', '## Failures',
                    '## Baseline and comparison', '## Limitations and remaining gates', '## Raw evidence',
                    '## Reproduction and cleanup'):
        assert heading in summary, heading
    assert 'Verdict: **FAIL** (1 PASS, 4 FAIL, 3 NOTRUN of 8 gates)' in summary
    assert '- None performed.' in summary and 'stub-destructive NOTRUN' in summary
    assert '`echo PASS one`' in summary
    sums = __import__('hashlib').sha256(listed.encode()).hexdigest()
    assert sums in summary and sums in (out1 / 'f3-row.md').read_text()
    f3 = (out1 / 'f3-row.md').read_text()
    assert 'TODO-summary' not in f3 and '(summary-draft.md)' in f3
    assert '| Placement/capacity and decode | FAIL (stub-pass PASS, stub-fail FAIL' in f3, f3
    assert '| Full payloads + orderly restart | NOTRUN (no gate selected in this run' in f3
    # D4 is a gate now (d4-progress); this run did not select it.
    assert '| Scale/progress evidence (E4, D4) | NOTRUN (no gate selected in this run' in f3

    # 2. The destructive stub runs once allowed and the step is listed.
    out2 = tmp / 'out2'
    rc, text = run('--cluster', str(desc), '--out', str(out2), '--gates', 'stub-pass,stub-destructive',
                   '--allow', 'restart')
    assert rc == 0, (rc, text)
    report = load(out2 / 'run.json')
    assert report['verdict'] == 'PASS' and report['allow'] == ['restart']
    assert [s['gate'] for s in report['destructive_steps']] == ['stub-destructive']
    summary = (out2 / 'summary-draft.md').read_text()
    assert '- Allowed: restart' in summary and 'stub-destructive: restart: plugin stub-destructive' in summary

    # 3. NOTRUN without FAIL is exit 3; --skip and --deadline apply.
    rc, text = run('--cluster', str(desc), '--out', str(tmp / 'out3'), '--gates', 'stub-pass,stub-notrun,stub-slow',
                   '--skip', 'stub-slow', '--deadline', 'stub-pass=7')
    assert rc == 3, (rc, text)
    assert load(tmp / 'out3/gates/stub-pass/result.json')['deadline_s'] == 7.0
    assert not (tmp / 'out3/gates/stub-slow').exists()

    # 4. Refusals: output inside Git, a non-empty output, an unknown gate, reserved ports.
    (tmp / 'repo/.git').mkdir(parents=True)
    rc, text = run('--cluster', str(desc), '--out', str(tmp / 'repo/out'), '--gates', 'stub-pass')
    assert rc == 2 and 'inside a Git work tree' in text, text
    rc, text = run('--cluster', str(desc), '--out', str(out1), '--gates', 'stub-pass')
    assert rc == 2 and 'not empty' in text, text
    rc, text = run('--cluster', str(desc), '--out', str(tmp / 'out5'), '--gates', 'nope')
    assert rc != 0 and "unknown gate 'nope'" in text, text
    bad = dict(cluster, private=dict(port_base=17420))
    (tmp / 'bad.json').write_text(json.dumps(bad))
    rc, text = run('--cluster', str(tmp / 'bad.json'), '--out', str(tmp / 'out6'), '--gates', 'stub-pass')
    assert rc == 2 and 'reserved ports' in text, text

    # 5. --plan: the real gates' command lines, nothing executed.
    real = dict(name='plan', source='/src', seed='10.0.0.1:17432', transport='tcp',
                nodes=[dict(addr=f'10.0.0.{i}:17432', host='local') for i in (1, 2, 3)],
                mounts=[dict(path='/mnt/a', host='local'), dict(path='/mnt/b', host='local')],
                private=dict(host='local', root=str(tmp / 'live'), port_base=18700))
    (tmp / 'real.json').write_text(json.dumps(real))
    rc, text = run('--cluster', str(tmp / 'real.json'), '--out', str(tmp / 'out7'), '--plan')
    assert rc == 3, (rc, text)
    cmds = {}
    for d in (tmp / 'out7/gates').iterdir():
        if d.name.startswith('_'):
            continue
        cmds[d.name] = [c['command'] for c in load(d / 'result.json')['commands']]
    joined = {k: '\n'.join(v) for k, v in cmds.items()}
    assert 'python3 tests/posix/posix_suite.py /mnt/a --results' in joined['posix-core']
    assert '--suite core' in joined['posix-core']
    assert 'posix_2client.py --local /mnt/a /mnt/b' in joined['posix-2client']
    assert '--port 18710' in joined['k13-tickets'] and '--rel /src' in joined['append-failover']
    assert '--direct' not in joined['append-failover'] and '--mode' not in joined['gc-reclamation']
    assert "K14_PHASES='1 2 3 7' scripts/k14_atomic_cluster.sh run /src" in joined['k14-torn-read']
    assert '/src/efs-mgmt version 10.0.0.2:17432' in joined['identity']
    assert joined['e2-client-stop'] == '' and joined['posix-durability'] == ''
    reasons = {g['gate']: g['reason'] for g in load(tmp / 'out7/run.json')['gates']}
    assert all(r == 'plan only (--plan): nothing ran' for r in reasons.values()), reasons
    assert not (tmp / 'live').exists()

    # 6. Topology gate against a stub efs-mgmt: PASS on distinct hosts,
    # NOTRUN when daemons share a host, FAIL on a down or missing member.
    status = ('Cluster state: healthy (3 up, 0 down)\n'
              '  node 1: 10.0.0.1:17432  up  used=1M  quota=4G (0.1%)  storage(1):\n'
              '  node 2: 10.0.0.2:17432  up  used=1M  quota=unlimited  storage(2):\n'
              '  node 3: 10.0.0.3:17432  {s}  used=1M  quota=4G (0.1%)  storage(1):\n')
    assert Q.parse_members(status.format(s='up')) == {'10.0.0.1:17432': (True, 1), '10.0.0.2:17432': (True, 2),
                                                      '10.0.0.3:17432': (True, 1)}
    fake = tmp / 'fake-src'
    fake.mkdir()
    def topo(name, st, hosts, rc_want, verdict, reason):
        (fake / 'efs-mgmt').write_text(f'#!/bin/sh\ncat <<\'X\'\n{st}X\n')
        (fake / 'efs-mgmt').chmod(0o755)
        c = dict(name=name, source=str(fake), seed='10.0.0.1:17432', transport='tcp',
                 nodes=[dict(addr=f'10.0.0.{i}:17432', host=h) for i, h in zip((1, 2, 3), hosts)], mounts=[])
        (tmp / f'{name}.json').write_text(json.dumps(c))
        rc, text = run('--cluster', str(tmp / f'{name}.json'), '--out', str(tmp / f'out-{name}'), '--gates', 'topology')
        r = load(tmp / f'out-{name}/gates/topology/result.json')
        assert rc == rc_want and r['verdict'] == verdict and reason in r['reason'], (name, rc, r['reason'], text)
        return r
    r = topo('t-pass', status.format(s='up'), ['local'] * 3, 3, 'NOTRUN', '3 members on 1 host(s)')
    assert r['fixture']['members']['10.0.0.2:17432'] == dict(up=True, storage_roots=2)
    topo('t-down', status.format(s='DOWN'), ['local'] * 3, 1, 'FAIL', 'members down: 10.0.0.3:17432')
    topo('t-missing', status.format(s='up').replace('10.0.0.3', '10.0.0.9'), ['local'] * 3, 1, 'FAIL',
         'status reports members')
    rc, text = run('--cluster', str(desc), '--out', str(tmp / 'out-t0'), '--gates', 'topology')
    assert rc == 1 and 'alpha 1.0 needs at least 3' in load(tmp / 'out-t0/gates/topology/result.json')['reason']

    # 7. No stub gate is left: every Stage 2 gate has a harness. Against a
    # description without nodes or mounts each says what it needs.
    names = [g.name for g in Q.gates(Q.default_plugins({}, []))]
    assert not hasattr(Q, 'STUBS') and not hasattr(Q, 'g_stub'), 'stub gates are gone'
    assert len(names) == len(set(names)), 'gate names must be unique'
    new = ['a13-node-addition', 'c5-protocol-live', 'd1-packing', 'd3-efs-load', 'e4-scale']
    assert set(new) <= set(names) and not any(g.doc.startswith('STUB') for g in Q.gates(Q.default_plugins({}, [])))
    rc, text = run('--cluster', str(desc), '--out', str(tmp / 'out-new'), '--gates', ','.join(new), '--allow', 'all')
    assert rc == 3, (rc, text)
    reasons = {g['gate']: g['reason'] for g in load(tmp / 'out-new/run.json')['gates']}
    assert all('no harness' not in r for r in reasons.values()), reasons
    assert reasons['a13-node-addition'].startswith('needs a driver-owned cluster'), reasons
    assert reasons['c5-protocol-live'] == 'needs a mount and nodes' and reasons['d1-packing'] == 'no mount described'
    assert reasons['d3-efs-load'] == 'no mount described' and reasons['e4-scale'] == 'needs a mount and nodes'
    assert [g.needs for g in Q.gates({}) if g.name == 'a13-node-addition'] == [('restart',)]
    # Plan commands of the new gates (the description of section 5).
    joined7 = joined
    assert "stage2_helpers.py wire --node 10.0.0.1:17432 --node 10.0.0.2:17432 --node 10.0.0.3:17432" in \
        joined7['c5-protocol-live'], joined7['c5-protocol-live']
    assert 'stage2_helpers.py smallfiles write --dir /mnt/a/qualify-out7/d1 --files 400' in joined7['d1-packing']
    assert '--max-size 8192' in joined7['d1-packing']
    assert 'stage2_helpers.py d3 --dir /mnt/a/qualify-out7/d3 --mount /mnt/a' in joined7['d3-efs-load']
    assert '--peer /mnt/b' in joined7['d3-efs-load'] and '--load-s 300' in joined7['d3-efs-load']
    assert '--small 15000 --large 6 --large-mib 256' in joined7['e4-scale'], joined7['e4-scale']
    assert '/src/efs-mgmt status 10.0.0.1:17432' in joined7['e4-scale']
    # Fixture defaults are declared in the matrix with the driver's values.
    mfx = load(root / 'scripts/qualify_matrix.json')['fixture']
    assert {k: v for k, v in mfx.items() if k != 'about'} == {k: Q.DEFAULT_FIXTURE[k] for k in mfx if k != 'about'}
    assert {'a13_members', 'd1_files', 'd3_load_s', 'e4_small_per_member', 'e4_large_mib'} <= set(mfx)

    # 7b. Status, raft-status and getchunks parsers.
    st = ('Cluster nodes (2):\n  node 1: 10.0.0.1:1  up  used=0 B  quota=2.00 GiB (0.0%)  storage(1):\n'
          '      bytes: used=5 quota=300 capacity=300 available=295 valid=1\n'
          '  node 4: 10.0.0.4:1  DOWN\nUsable bytes: used=7 free=93 capacity=100\n')
    assert Q.parse_status(st) == ({1: dict(addr='10.0.0.1:1', up=True, used=5, quota=300, capacity=300),
                                   4: dict(addr='10.0.0.4:1', up=False)}, dict(used=7, free=93, capacity=100))
    rs = ('node 3 root=1 salt=1 groups=2\n  group 0 hosted=1 role=FOLLOWER leader=0 term=2 commit=8 applied=8 '
          'voters=0x7 joint=0 retired=0\n  group 2 hosted=1 role=LEADER leader=2 term=3 commit=9 applied=9 '
          'voters=0xe joint=1 retired=0\n  group 5 hosted=1 role=FOLLOWER leader=0 term=1 commit=3 applied=3 '
          'voters=0x7 joint=1 retired=1\n')
    assert Q.parse_raft(rs) == (3, {0: dict(role='FOLLOWER', voters=7, joint=0), 2: dict(role='LEADER', voters=14, joint=1)})
    assert Q.mask({2, 3, 4}) == 0xe and Q.mask({1, 2, 3}) == 7
    # Five-member membership retains retired lane state for diagnostics.
    # Its stale leader/term/index must not enter turnover or catch-up proofs.
    class RetiredViews:
        cluster = dict(nodes=[dict(addr='retired'), dict(addr='active')])
        def mgmt(self, g, label, op, addr, deadline=30):
            if addr == 'retired':
                return 0, 'group 2 hosted=1 role=LEADER leader=0 term=99 commit=10 applied=10 voters=0x7 joint=0 retired=1\n'
            return 0, 'group 2 hosted=1 role=LEADER leader=1 term=3 commit=20 applied=20 voters=0xe joint=0 retired=0\n'
    led, active_views = Q.leaders(RetiredViews(), None, 'probe')
    assert led == {2: ('active', 3, 1, [1, 2, 3])}, led
    assert Q.agreed(active_views[2], 1, 3)
    assert Q.applied(RetiredViews(), None, 'retired', 'probe') == {}
    assert Q.applied(RetiredViews(), None, 'active', 'probe') == {2: (20, 20)}
    hspec = importlib.util.spec_from_file_location('stage2_helpers', root / 'scripts/stage2_helpers.py')
    H = importlib.util.module_from_spec(hspec)
    hspec.loader.exec_module(H)
    gc = ('raft-getchunks status=0 primary=1 ino=9 count=2 cis=0\n'
          '  ci=0 nodes=3,1,2 base_gen=5 obj_gen=5 ck0=424ef2fe spans=0 seq=0\n'
          '    packed segment=77 gen=1 group=0 offset=1044 len=300\n'
          '  ci=1 nodes=0,0,0 base_gen=0 obj_gen=0 ck0=00000000 spans=1 seq=1\n'
          '    span off=0 len=300 gen=99 seq=1 nodes=1,2,3\n')
    status, rows = H.parse_getchunks(gc)
    assert status == 0 and rows[0]['packed'] == dict(segment=77, gen=1, group=0, offset=1044, len=300)
    assert H.stored_objects(rows) == {('segment', 77, 1), ('span', 99, (1, 2, 3))}

    # The GC crash gate must follow K36 verified recovery, never demand a
    # permanent crash fence after legitimate automatic recovery completed.
    import ast, re
    gc_tree = ast.parse((root / 'tests/live/gc_reclamation.py').read_text())
    ready_fn = next(n for n in gc_tree.body if isinstance(n, ast.FunctionDef) and n.name == 'recovery_fsync_ready')
    namespace = {'re': re}
    exec(compile(ast.Module(body=[ready_fn], type_ignores=[]), 'gc-readiness', 'exec'), namespace)
    ready = namespace['recovery_fsync_ready']
    eps = ['127.0.0.1:19001', '127.0.0.1:19002']
    reports = [ep + ': window 100 ms / 8 MiB\n  root: async\n  recovery verified phase=rebuild 50 ms: groups proven=2/2\n' for ep in eps]
    assert ready(''.join(reports), eps)
    assert not ready(reports[0], eps)
    assert not ready(reports[0] * 2, eps)
    assert not ready(''.join(reports).replace('verified', 'running'), eps)
    assert not ready(''.join(reports).replace('root: async', 'root: async ERROR(barrier)'), eps)
    assert not ready(''.join(reports) + '    recovery uncertain: fenced\n', eps)

    # The offline-member GC case is an intentional crash, not an E2
    # clean-stop gate. It signals only its held child and detaches its own
    # mount. Missing ownership refuses before signalling or detaching.
    from unittest.mock import Mock
    crash_fn = next(n for n in gc_tree.body if isinstance(n, ast.FunctionDef) and n.name == 'crash_unmount')
    child = Mock()
    child.poll.return_value = None
    detach = Mock()
    ns = dict(client=child, mount=Path('/tmp/owned-gc-mount'), run=detach, print=Mock())
    exec(compile(ast.Module(body=[crash_fn], type_ignores=[]), 'gc-crash-unmount', 'exec'), ns)
    ns['crash_unmount']()
    child.kill.assert_called_once_with()
    child.wait.assert_called_once_with(timeout=15)
    detach.assert_called_once_with(['fusermount3', '-uz', Path('/tmp/owned-gc-mount')])
    assert ns['client'] is None
    try:ns['crash_unmount']()
    except AssertionError:pass
    else:raise AssertionError('crash detach without a held child accepted')
    assert detach.call_count == 1
    assert "crash_unmount();stop(crash=True)\n    inspector=" in (root / 'tests/live/gc_reclamation.py').read_text()

    # 7c. The wire probe against fake daemons: a correct gate (leader
    # refuses the tags, follower redirects) passes every check; a daemon
    # that serves undeclared requests and applies any tag fails them.
    import socket as _s, struct as _st, threading as _th

    def fake_daemon(leader, lax=False):
        srv = _s.socket()
        srv.setsockopt(_s.SOL_SOCKET, _s.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', 0))
        srv.listen(16)

        def serve(c):
            ok = False
            with c:
                while True:
                    head = c.recv(5, _s.MSG_WAITALL)
                    if len(head) < 5:
                        return
                    n, t = _st.unpack('>IB', head)
                    body = c.recv(n - 1, _s.MSG_WAITALL) if n > 1 else b''
                    if t == 0:
                        c.sendall(_st.pack('>IB', 1, 1))
                    elif t == 165:
                        good = body[:4] == b'EFSP' and _st.unpack('<I', body[4:8])[0] == 5
                        c.sendall(_st.pack('>IB', 9, 166) + _st.pack('<iI', 0 if good else -7, 5))
                        if not good:
                            return
                        ok = True
                    elif not ok and not lax:
                        return
                    elif t == 95:
                        rc, hint = (0, -1) if lax else ((-7, -1) if leader is None else (-15, leader))
                        c.sendall(_st.pack('>IB', 33, 96) + _st.pack('<iiQQQ', rc, hint, 0, 0, 0))
        def accept():
            while True:
                try:
                    c, _ = srv.accept()
                except OSError:
                    return
                _th.Thread(target=serve, args=(c,), daemon=True).start()
        _th.Thread(target=accept, daemon=True).start()
        return srv, f'127.0.0.1:{srv.getsockname()[1]}'

    lead, ep0 = fake_daemon(None)
    foll, ep1 = fake_daemon(0)
    checks = [c for ep in (ep0, ep1) for c in H.wire_node(ep, [ep0, ep1], 2)]
    assert checks and all(c['ok'] for c in checks), [c for c in checks if not c['ok']]
    assert any(c.get('via') == [ep1, ep0] for c in checks), 'the follower redirect was followed'
    lax, ep2 = fake_daemon(None, lax=True)
    failed = {c['check'] for c in H.wire_node(ep2, [ep2], 2) if not c['ok']}
    assert 'unknown-tag-254-group-0-refused' in failed and 'reserved-tag-46-group-2-refused' in failed, failed
    for sock in (lead, foll, lax):
        sock.close()

    # 7d. Removed-settings probe against fake binaries: one that names the
    # setting and exits passes; one that ignores it (and touches the store) fails.
    for kind, script, want in (('strict', 'for v in EFS_COMMANDS EFS_MD_RAFT_N EFS_READ_VERIFY EFS_METADATA_BALANCE '
                                          'EFS_UNMOUNT_REVOKE EFS_FUSE_EXPORT EFS_EXPIRY; do [ -n "${!v+x}" ] && '
                                          '{ echo "x: $v was removed (A6)"; exit 1; }; done\nfor a; do case $a in '
                                          '--no-direct-io|--direct-io|--no-persist|--node-id) echo "$a was removed"; '
                                          'exit 1;; esac; done\nexit 0\n', 0),
                               ('lax', 'mkdir -p "$2"; exit 0\n', 1)):
        tree = tmp / f'refuse-{kind}'
        tree.mkdir()
        for prog in ('efsd', 'efs-fuse'):
            (tree / prog).write_text('#!/bin/bash\n' + script)
            (tree / prog).chmod(0o755)
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            assert H.main(['refuse', '--src', str(tree), '--scratch', str(tmp / f'scr-{kind}')]) == want
        doc = json.loads(buf.getvalue().splitlines()[-1])
        assert doc['total'] == len(H.REFUSALS) and (doc['passed'] == doc['total']) == (want == 0), doc

    # 7e. Host helpers on a plain directory: the load, the D1 fixture and
    # its D2 mutations and the E4 fixture all verify with _manifest verify.
    def helper(*argv, stdin=None):
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            if stdin is not None:
                saved_in, sys.stdin = sys.stdin, io.StringIO(json.dumps(stdin))
            try:
                rc = H.main(list(map(str, argv)))
            finally:
                if stdin is not None:
                    sys.stdin = saved_in
        return rc, json.loads(buf.getvalue().splitlines()[-1])

    def verify(doc):
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            saved_in, sys.stdin = sys.stdin, io.StringIO(json.dumps(doc))
            try:
                rc = Q.main(['_manifest', 'verify'])
            finally:
                sys.stdin = saved_in
        return rc, json.loads(buf.getvalue().splitlines()[-1])

    stop = tmp / 'stop-load'
    _th.Timer(0.6, stop.touch).start()
    rc, doc = helper('load', '--dir', tmp / 'h/load', '--stop', stop, '--seed', 's', '--threads', 2, '--kib', 1)
    assert rc == 0 and doc['acknowledged'] > 0 and doc['acknowledged'] == doc['ops'], doc
    assert verify(dict(dir=doc['dir'], files=doc['files'], errors=[]))[0] == 0
    rc, small = helper('smallfiles', 'write', '--dir', tmp / 'h/small', '--files', 40, '--seed', 's', '--max-size', 900)
    assert rc == 0 and len(small['files']) == 40 and all(f['ino'] for f in small['files']), small
    assert verify(small)[0] == 0
    rc, mut = helper('smallfiles', 'mutate', '--seed', 's', stdin=small)
    assert rc == 0 and mut['mutations'] == dict(overwritten=5, truncated=5, unlinked=5, retained=25), mut
    assert verify(mut)[0] == 0 and verify(small)[0] == 1, 'mutations change what reads back'
    assert any(f['size'] == 0 for f in mut['files']), 'one in sixteen truncated to 0'
    rc, e4 = helper('e4', 'write', '--dir', tmp / 'h/e4', '--seed', 's', '--small', 30, '--large', 2,
                    '--large-mib', 1, '--threads', 4)
    assert rc == 0 and (e4['completed_small'], e4['completed_large']) == (30, 2) and verify(e4)[0] == 0, e4
    rc, rest = helper('e4', 'delete', stdin=e4)
    assert rc == 0 and rest['deleted'] == 2 + 15 and len(rest['files']) == 15 and verify(rest)[0] == 0, rest

    # 8. The b-checks plugin matches scripts/b_checks.py's CLI.
    host, cmd = Q.b_checks_command(dict(nodes=three['nodes'], mounts=[
        dict(path='/m/a', host='h1'), dict(path='/m/b', host='h2'), dict(path='/m/c', host='h1')]))
    filled = cmd.format(source='/src', seed='S')
    assert host == 'h1' and '--mount /m/a --mount /m/c --expect-members 3' in filled and '/m/b' not in filled
    assert 'b_checks.py --seed S' in filled and 'exit $rc' in filled and '${TMPDIR:-/tmp}' in filled

    # 9. ssh wrapper replaces ssh; relative wrappers resolve in the tree.
    from unittest.mock import patch
    with patch.object(Q, 'is_local', return_value=False):
        argv = Q.Runner(ssh='scripts/orcd_mini_ssh.sh').argv('node9901', 'echo hi', 10)
    assert argv[:2] == [str(root / 'scripts/orcd_mini_ssh.sh'), 'node9901'] and 'echo hi' in argv[2]
    with patch.object(Q, 'is_local', return_value=True):
        assert Q.Runner(ssh='scripts/orcd_mini_ssh.sh').argv('node9901', 'echo hi', 10) == ['bash', '-c', 'echo hi']

    # 10. The committed matrix validates; broken ones are caught.
    with contextlib.redirect_stdout(io.StringIO()) as buf:
        assert Q.main(['plan', '--check']) == 0, buf.getvalue()
    matrix = load(root / 'scripts/qualify_matrix.json')
    assert Q.matrix_errors(matrix) == []
    # Plan generation is read-only and must work when the real topology's
    # ports are occupied. It must never claim those ports or need a daemon.
    from unittest.mock import patch
    with patch.object(Q.socket, 'socket', side_effect=AssertionError('plan must not bind')):
        rc, text = run('--loopback', '5', '--out', str(tmp / 'occupied-plan'), '--plan',
                       '--candidate', '123456789')
    assert rc == 3, text
    assert load(tmp / 'occupied-plan/cluster.json')['candidate'] == '123456789'
    assert load(tmp / 'occupied-plan/run.json')['seconds'] >= 0
    rc, text = run('--loopback', '3', '--out', str(tmp / 'bad-candidate'), '--plan', '--candidate', 'HEAD')
    assert rc == 2 and '--candidate must' in text, text
    # All owned members are stopped even if the first one fails.
    loop = object.__new__(Q.Loopback)
    loop.procs = {0: object(), 1: object(), 2: object()}
    called = []
    loop.stop_daemon = lambda i: called.append(i) or (1 if i == 0 else 0)
    loop._logged = lambda g, label, fn: fn()
    assert loop.stop_cluster(None) is False and called == [0, 1, 2]
    # False compares equal to integer 0 in Python, but is a failed controller
    # operation, not a successful stop or a successful readiness predicate.
    from types import SimpleNamespace
    private_q = SimpleNamespace(control=SimpleNamespace(owned=True))
    observed = 'ERROR: /private/mnt is in use by: localsearch-3(123), gvfs-udisks2-vo(456); stop refused (no force)'
    assert Q.stop_failure(private_q, observed, 'stop: ')[0] == 'NOTRUN'
    assert 'desktop mount observer' in Q.stop_failure(private_q, observed, 'stop: ')[1]
    assert Q.stop_failure(private_q, observed.replace('localsearch-3', 'python3'), 'stop: ')[0] == 'FAIL'
    assert Q.stop_failure(private_q, 'ERROR: drain timed out', 'stop: ')[0] == 'FAIL'
    assert Q.stop_failure(private_q, observed, 'stop: ', 124) == ('FAIL', 'stop: deadline exceeded')
    assert Q.stop_failure(SimpleNamespace(control=SimpleNamespace(owned=False)), observed, 'stop: ')[0] == 'FAIL'
    for value, expected in ((False, 1), (True, 0), (None, 0), (0, 0), (1, 1)):
        commands = SimpleNamespace(commands=[])
        rc, why = Q.Loopback._logged(loop, commands, 'controller-result', lambda: value)
        assert rc == expected and commands.commands[-1]['rc'] == expected, (value, rc)
    four = matrix['topologies']['fcstor-4x1-tcp']
    assert len(Q.distinct_hosts(four['cluster'])) == 4
    assert len({m['host'] for m in four['cluster']['mounts']}) == 2
    assert four['fixture'] == matrix['topologies']['loopback-5']['fixture']
    assert four['cluster']['seed'] == '10.1.223.66:19340'
    assert not four.get('live_user_cluster')
    assert 'soak' in four['gates'], 'E4 topologies retain the endurance companion in their full plan'
    five = matrix['topologies']['loopback-5']
    assert five['loopback'] == 5 and five['transport'] == 'tcp'
    assert five['gates'] == matrix['topologies']['loopback-3']['gates']
    assert {'a13-node-addition', 'e3-turnover', 'e4-scale'} <= set(five['gates'])
    assert five['fixture'] == matrix['topologies']['loopback-3']['fixture']
    assert five['fixture']['soak_hours'] == 0.25
    assert five['fixture']['soak_profile'] == 'rehearsal'
    assert five['port_base'] >= five['private_port_base'] + 140
    rc, text = run('--matrix', str(root / 'scripts/qualify_matrix.json'), '--topology', 'loopback-5',
                   '--out', str(tmp / 'five-plan'), '--plan')
    assert rc == 3, text
    five_cluster = load(tmp / 'five-plan/cluster.json')
    assert len(five_cluster['nodes']) == 5
    assert len(five_cluster['mounts']) == 2
    assert set(five['gates']) == {g.name for g in Q.gates(Q.default_plugins(five_cluster, []))}
    assert set(matrix['items']) == set(Q.STAGE2_ITEMS)
    broken = json.loads(json.dumps(matrix))
    del broken['items']['E4']
    broken['items']['C1']['checks'].append('qualify:no-such-gate')
    broken['items']['D2']['checks'].append('live:tests/live/no_such.py')
    broken['topologies']['loopback-3']['gates'].append('nope')
    broken['topologies']['bare-metal-3-tcp']['cluster']['nodes'][1]['store'] = '/data1/efs/n2'
    errors = '\n'.join(Q.matrix_errors(broken))
    for want in ('item E4: not in the plan', "qualify gate 'no-such-gate' does not exist",
                 'tests/live/no_such.py does not exist', "gate 'nope' does not exist",
                 "store '/data1/efs/n2' is not under private_cluster.root"):
        assert want in errors, (want, errors)

    # 11. run --matrix: blocked and live topologies are refused without the
    # flags; a driver-owned topology plans formation (seed, then --join) and
    # a teardown that deletes its root on every host.
    rc, text = run('--matrix', str(root / 'scripts/qualify_matrix.json'), '--topology', 'bare-metal-5-tcp',
                   '--out', str(tmp / 'm1'), '--plan')
    assert rc == 2 and 'is blocked: firewalld' in text, text
    rc, text = run('--matrix', str(root / 'scripts/qualify_matrix.json'), '--topology', 'orcd-mini-rdma',
                   '--out', str(tmp / 'm2'), '--plan')
    assert rc == 2 and "user's live cluster" in text, text
    rc, text = run('--matrix', str(root / 'scripts/qualify_matrix.json'), '--topology', 'bare-metal-4-tcp',
                   '--out', str(tmp / 'm3'), '--plan', '--ignore-blocked')
    assert rc == 3, (rc, text)
    setup = [c['command'] for c in load(tmp / 'm3/gates/_setup/result.json')['commands']]
    starts = [c for c in setup if '_node start' in c]
    assert len(starts) == 4 and '--join' not in starts[0] and all('--join 10.1.0.204:18960' in c for c in starts[1:])
    teardown = [(c['host'], c['command']) for c in load(tmp / 'm3/gates/_teardown/result.json')['commands']]
    deleted = {h for h, c in teardown if 'edelete --force --delete /home/maik/efs-qual-matrix/bare-metal-4-tcp' in c}
    assert deleted == {'nuc', 'intel1', 'intel2', 'xorinox'}, teardown
    assert load(tmp / 'm3/run.json')['allow'] == ['remount', 'restart', 'turnover']
    a13 = [c['command'] for c in load(tmp / 'm3/gates/a13-node-addition/result.json')['commands']]
    assert any('_node start --addr 10.1.0.204:19060 --store /home/maik/efs-qual-matrix/bare-metal-4-tcp/a13/n1'
               in c and '--quota 2G' in c and '--join' not in c for c in a13), a13

    # The owned loopback tree excludes desktop crawlers before formation,
    # without adding a marker inside the mounted filesystem or bypassing stop.
    seen_marker = []
    def formation_failure(loop):
        seen_marker.append((loop.work / '.nomedia').is_file())
        raise RuntimeError('test formation failure')
    with patch.object(Q.Loopback, 'start', formation_failure), patch.object(Q.Loopback, 'teardown'):
        try:run('--loopback', '3', '--out', str(tmp / 'marker-out'), '--work', str(tmp / 'marker-work'))
        except RuntimeError as exc:assert str(exc) == 'test formation failure'
        else:raise AssertionError('formation failure hidden')
    assert seen_marker == [True]

    # 12. _node: start waits until the daemon listens, refuses a second
    # start, stop sends SIGTERM and waits (a stub efsd from a stub tree).
    tree = tmp / 'tree'
    tree.mkdir()
    (tree / 'efsd').write_text('#!/usr/bin/env python3\nimport socket, sys, signal, time\nfrom pathlib import Path\n'
                               'addr = next(a for a in sys.argv[1:] if a.count(":") == 1 and not a.startswith("-"))\n'
                               'h, p = addr.rsplit(":", 1)\n'
                               'time.sleep(60 if Path(__file__).with_name("hold-listen").exists() else 0.5)\ns = socket.socket()\n'
                               's.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\ns.bind((h, int(p)))\n'
                               's.listen()\nsignal.signal(signal.SIGTERM, lambda *a: sys.exit(0))\n'
                               'while True:\n    time.sleep(1)\n')
    (tree / 'efsd').chmod(0o755)
    import socket as _socket
    with _socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    saved, Q.ROOT = Q.ROOT, tree
    try:
        node = ['--addr', f'127.0.0.1:{port}', '--store', str(tmp / 'pc/n1'), '--timeout', '20']
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            assert Q.main(['_node', 'start', *node, '--env=EFS_TRANSPORT=tcp']) == 0, buf.getvalue()
            assert Q.main(['_node', 'start', *node]) == 1
            pid = int((tmp / 'pc/n1.pid').read_text())
            assert Q.pid_alive(pid)
            assert Q.main(['_node', 'stop', *node]) == 0
            assert Q.main(['_node', 'stop', *node]) == 0
            # A private pidfile alone does not own another process. Neither
            # a missing record nor a forged identity may signal the child.
            import subprocess
            unrelated = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'])
            pidfile = tmp / 'pc/n1.pid'
            identity = tmp / 'pc/n1.identity.json'
            saved_identity = identity.read_text()
            try:
                pidfile.write_text(str(unrelated.pid))
                identity.unlink()
                assert Q.main(['_node', 'kill', *node]) == 2 and unrelated.poll() is None
                forged = Q.node_identity(unrelated.pid, f'127.0.0.1:{port}', tmp / 'pc/n1')
                forged['start'] = str(int(forged['start']) + 1)
                identity.write_text(json.dumps(forged))
                assert Q.main(['_node', 'stop', *node]) == 2 and unrelated.poll() is None
                forged['start'] = Q.node_identity(unrelated.pid, f'127.0.0.1:{port}', tmp / 'pc/n1')['start']
                identity.write_text(json.dumps(forged))
                assert Q.main(['_node', 'kill', *node]) == 2 and unrelated.poll() is None
                # Empty /proc cmdline during exec is never permission to
                # signal. Wait for a readable command, retaining the pidfd.
                command = b'python3\0-u\0' + os.fsencode(str(tree / 'efsd')) + b'\0' + os.fsencode(f'127.0.0.1:{port}') + b'\0' + os.fsencode(str(tmp / 'pc/n1')) + b'\0'
                with patch.object(Path, 'read_bytes', side_effect=[b'', command]) as read, \
                     patch.object(Q.time, 'sleep') as pause, \
                     patch.object(Q.signal, 'pidfd_send_signal') as send:
                    Q.signal_owned_node(unrelated.pid, Q.signal.SIGTERM, f'127.0.0.1:{port}', tmp / 'pc/n1', identity)
                    assert read.call_count == 2 and pause.call_count == 1
                    send.assert_called_once()
                assert unrelated.poll() is None
            finally:
                unrelated.terminate();unrelated.wait()
                identity.write_text(saved_identity)
            # kill: an interrupted join (A13); --timeout 0 returns once spawned.
            assert Q.main(['_node', 'start', *node[:4], '--timeout', '0']) == 0
            pid = int((tmp / 'pc/n1.pid').read_text())
            assert Q.main(['_node', 'kill', *node]) == 0 and not Q.pid_alive(pid), buf.getvalue()
            (tree / 'hold-listen').touch()
            try:
                assert Q.main(['_node', 'start', *node[:4], '--timeout', '0.05']) == 1
                assert Q.main(['_node', 'stop', *node]) == 0
            finally:
                (tree / 'hold-listen').unlink()
            refused = tmp / 'reserved-node'
            assert Q.main(['_node', 'start', '--addr', '127.0.0.1:17432', '--store', str(refused)]) == 2
            assert Q.main(['_node', 'start', '--addr', f'127.0.0.1:{port}', '--store', str(refused),
                           '--join', '127.0.0.1:27432']) == 2
            assert not refused.exists()
        out = buf.getvalue()
        assert 'listening' in out and 'already running' in out and 'exited' in out and 'not running' in out, out
        assert f'efsd 127.0.0.1:{port} {tmp / "pc/n1"} --quota 4G --discovery-port 0' in (tmp / 'pc/n1.log').read_text()
        with contextlib.redirect_stdout(io.StringIO()):
            assert Q.main(['_node', 'start', '--addr', '127.0.0.1:1', '--store', '/data1/efs/x']) == 2
    finally:
        Q.ROOT = saved
finally:
    shutil.rmtree(tmp)
print('qualify driver: stub pass/fail/not-run/deadline/liar verdicts, result fields, SHA256SUMS '
      'and tamper control, summary and F3 row, refusals, --plan commands, topology gate, no stub gates '
      '(A13/C5/D1/D3/E4 plans, parsers, wire probe vs fake daemons, removed-settings probe, host helpers), '
      'b-checks CLI, ssh wrapper, A11 matrix validation and topologies, _node start/stop PASS')
