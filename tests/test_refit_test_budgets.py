#!/usr/bin/env python3
"""Calibration uses the maximum host median, preserves scope and rejects bad data."""
from decimal import Decimal
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('refit', root / 'scripts/refit_test_budgets.py')
R = importlib.util.module_from_spec(spec)
spec.loader.exec_module(R)
command = 'python3 tests/slow.py'


def host(name, values, present=True):
    return dict(name=name, indices={command: [0]} if present else {},
                runs=[dict(timings=[dict(seconds=str(v))], notrun_ids=set()) for v in values])


def refused(fn, message):
    try:
        fn()
    except ValueError as e:
        assert message in str(e), str(e)
    else:
        raise AssertionError('invalid calibration accepted: ' + message)


budgets = {command: (Decimal(10), 2, 'retain this reason')}
hosts = {n: host(n, v) for n, v in
         [('intel1', [1, 100, 2]), ('nuc', [8, 9, 10]), ('intel2', [4, 5, 6])]}
fitted, rows = R.fit_rows(budgets, hosts)
assert fitted[command][0] == 15, 'must use max(2, 9, 5), not pooled median or a single-run maximum'
assert fitted[command][1] == 2 and fitted[command][2].startswith('retain this reason')
assert rows[0]['host_medians_s'] == dict(intel1=2, nuc=9, intel2=5)
assert rows[0]['slowest_hosts'] == ['nuc']
for value, expected in [('0', 5), ('4', 5), ('4.001', 10), ('240', 300), ('240.001', 305)]:
    h = {'nuc': host('nuc', [value] * 3)}
    f, _ = R.fit_rows(budgets, h)
    assert f[command][0] == expected, (value, f)
    assert f[command][1] == 2
    if expected > 300:
        assert 'slowest host median' in f[command][2]
hosts['intel1'] = host('intel1', [], present=False)
refused(lambda: R.fit_rows(budgets, hosts), 'declare --unmeasured explicitly')
fitted, rows = R.fit_rows(budgets, hosts, [('intel1', command)])
assert fitted[command][0] == 15 and rows[0]['unmeasured_hosts'] == ['intel1']
assert rows[0]['host_medians_s']['intel1'] is None
refused(lambda: R.fit_rows(budgets, hosts, [('absent', command)]), 'unknown unmeasured')
refused(lambda: R.fit_rows(budgets, {'nuc': hosts['nuc']}, [('nuc', command)]), 'stale unmeasured')
refused(lambda: R.fit_rows(budgets, {'intel1': hosts['intel1']}, [('intel1', command)]), 'no measured host')

with tempfile.TemporaryDirectory(prefix='efs-budget-fit-') as tmp:
    d = Path(tmp)

    def run_dir(name, value, *, fail=0, budget=10, extra=False):
        p = d / name
        p.mkdir()
        commands = [command] + (['python3 tests/new.py'] if extra else [])
        (p / 'cmds.txt').write_text('\n'.join(commands) + '\n')
        (p / 'summary.txt').write_text(f'TOTAL {len(commands)} FAIL {fail} NOTRUN 0 STATUS {"FAIL" if fail else "PASS"} (50s)\n')
        text = 'index\tseconds\trc\tcommand\tbudget\n'
        text += f'0\t{value}\t{fail}\t{command}\t{budget}\n'
        if extra:
            text += '1\t0.2\t0\tpython3 tests/new.py\t\n'
        (p / 'timings.tsv').write_text(text)
        return p

    first = [run_dir(f'a-{i}', v) for i, v in enumerate([1, 100, 2])]
    second = [run_dir(f'b-{i}', v, budget=20, extra=True) for i, v in enumerate([8, 9, 10])]
    refused(lambda: R.read_host('a', [first[0]] * 3), 'three distinct')
    bad = run_dir('bad', 1, fail=1)
    refused(lambda: R.read_host('a', [first[0], first[1], bad]), 'failed commands')
    bad_priority = run_dir('priority', 1, budget=11)
    refused(lambda: R.read_host('a', [first[0], first[1], bad_priority]), 'priorities changed')
    refused(lambda: R.read_host('a', [first[0], first[1], second[0]]), 'identical command sequence')
    bad_duration = run_dir('nan', 'NaN')
    refused(lambda: R.read_run(bad_duration), 'invalid duration')
    budget_file = d / 'budgets.tsv'
    budget_file.write_text('command\tbudget_s\tslots\treason\n' + command + '\t10\t2\tretain this reason\n')
    runner = d / 'runner.sh'
    runner.write_text('EXCLUSIVE=re.compile(r"never-matches")\n')
    output = d / 'fit'
    subprocess.run([sys.executable, str(root / 'scripts/refit_test_budgets.py'),
                    '--budgets', str(budget_file), '--runner', str(runner),
                    '--host-runs', 'intel1', *map(str, first),
                    '--host-runs', 'nuc', *map(str, second), '--output', str(output)],
                   check=True, stdout=subprocess.DEVNULL)
    report = json.loads((output / 'report.json').read_text())
    assert report['rows'][0]['new_budget_s'] == 15
    assert report['schedule_host'] == 'nuc', 'must model the fullest measured gate'
    assert report['unmeasured'] == []
print('budget refit: maximum host median, rounding, explicit coverage gaps, identity, failed-input refusal and multi-host CLI PASS')
