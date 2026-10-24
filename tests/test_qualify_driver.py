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
views = [('a', 'FOLLOWER', 1, 3), ('b', 'LEADER', 1, 3), ('c', 'FOLLOWER', 1, 3)]
assert Q.agreed(views, 1, 3) and not Q.agreed(views, 2, 3) and not Q.agreed(views, 1, 4)
assert not Q.agreed(views[:2] + [('c', 'CANDIDATE', 1, 3)], 1, 3) and not Q.agreed([], 1, 3)
three = dict(nodes=[dict(addr=f'10.0.0.{i}:17432', host=f'h{i}') for i in (1, 2, 3)])
assert Q.topology_column(three) == '3 physical nodes'
shared = dict(nodes=[dict(addr=f'10.0.0.1:{p}', host='h1') for p in (1, 2, 3, 4, 5)])
assert Q.topology_column(shared).startswith('>4 physical nodes (N=5) — NOT distinct hosts (1 host')

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
    assert '| Placement/capacity and decode | FAIL (stub-pass PASS, stub-fail FAIL' in f3, f3
    assert '| Full payloads + orderly restart | NOTRUN (no gate selected in this run' in f3
    assert '| Scale/progress evidence (E4, D4) | NOTRUN (not covered by this driver' in f3

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
    real = dict(name='plan', source='/src', seed='10.0.0.1:17432', transport='tcp', direct_io=True,
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
    assert '--direct' in joined['append-failover'] and '--mode direct' in joined['gc-reclamation']
    assert "K14_PHASES='1 2 3 7' scripts/k14_atomic_cluster.sh run /src" in joined['k14-torn-read']
    assert '/src/efs-mgmt version 10.0.0.2:17432' in joined['identity']
    assert joined['e2-client-stop'] == '' and joined['posix-durability'] == ''
    reasons = {g['gate']: g['reason'] for g in load(tmp / 'out7/run.json')['gates']}
    assert all(r == 'plan only (--plan): nothing ran' for r in reasons.values()), reasons
    assert not (tmp / 'live').exists()
finally:
    shutil.rmtree(tmp)
print('qualify driver: stub pass/fail/not-run/deadline/liar verdicts, result fields, SHA256SUMS '
      'and tamper control, summary and F3 row, refusals, --plan commands PASS')
