#!/usr/bin/env python3
"""W160 offline multi-host calibration; does not execute or alter any test.

Each --host-runs supplies a host and three clean result directories. Fit each
row to 1.25 times the largest host median, rounded upward to five seconds.
Declare specific absent observations with --unmeasured HOST COMMAND; never
substitute zero. Slots/reason prefixes and D44's target remain unchanged.
"""
import argparse
import ast
import csv
from decimal import Decimal, ROUND_CEILING
import heapq
import json
from pathlib import Path
import re
import statistics


def commands(path):
    result, current = [], ''
    for line in path.read_text().split('\n'):
        current += ('\n' if current else '') + line
        if not line.endswith('\\'):
            if current.strip():
                result.append(current)
            current = ''
    if current.strip():
        raise ValueError('unterminated command continuation: ' + str(path))
    return result


def command_identity(command):
    # make stamps inline compiles at enumeration time. Only that exact
    # informational ISO timestamp may differ; flags, source, build ID,
    # branch, command order and every other byte still have to match.
    return re.sub(r"""(-DEFS_BUILD_TIME=)'"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"'""",
                  r"\1'<enumeration-build-time>'", command)


def read_run(path):
    cmds = commands(path / 'cmds.txt')
    summary = (path / 'summary.txt').read_text().splitlines()
    match = re.match(r'TOTAL (\d+) FAIL (\d+) NOTRUN (\d+) STATUS (\w+) \((\d+)s\)', summary[0])
    if not match or not cmds or int(match[1]) != len(cmds):
        raise ValueError('incomplete summary: ' + str(path))
    with (path / 'timings.tsv').open() as stream:
        rows = list(csv.DictReader(stream, delimiter='\t'))
    if len(rows) != len(cmds) or {int(r['index']) for r in rows} != set(range(len(cmds))):
        raise ValueError('missing or duplicate command timings: ' + str(path))
    rows.sort(key=lambda r: int(r['index']))
    for i, row in enumerate(rows):
        if row['command'] != cmds[i].split('\n')[0][:150]:
            raise ValueError('timing does not identify enumerated command: ' + str(path))
        duration = Decimal(row['seconds'])
        if not duration.is_finite() or duration < 0:
            raise ValueError('invalid duration: ' + str(path))
    notrun_ids = {int(line.split('\t', 2)[1]) for line in summary[1:] if line.startswith('NOTRUN\t')}
    if len(notrun_ids) != int(match[3]) or not notrun_ids <= set(range(len(cmds))):
        raise ValueError('inconsistent NOTRUN accounting: ' + str(path))
    return dict(path=str(path), summary=summary[0], wall_s=int(match[5]),
                fail=int(match[2]), notrun=int(match[3]), notrun_ids=notrun_ids,
                commands=cmds, timings=rows)


def schedule(cmds, budgets, durations, exclusive, jobs=8):
    """Reproduce the runner's weighted priority order, assigning named slots.

    Each slot's previous reservation is a dependency of its next user.
    The longest dependency chain reaches the modeled pool makespan; these
    virtual slots do not imply CPU affinity or observed start timestamps.
    """
    pending = [i for i, c in enumerate(cmds) if not exclusive.search(c)]
    alone = [i for i, c in enumerate(cmds) if exclusive.search(c)]
    def head(i):
        return cmds[i].split('\n')[0].strip()
    def weight(i):
        return min(jobs, budgets.get(head(i), (0, 1, ''))[1])
    pending.sort(key=lambda i: (-weight(i), -budgets.get(head(i), (0, 1, ''))[0], i))
    free = set(range(jobs))
    previous = [None] * jobs
    running, spans = [], {}
    now = Decimal(0)
    while pending:
        pick = next((i for i in pending if weight(i) <= len(free)), None)
        if pick is None:
            now, _, _, slots = heapq.heappop(running)
            free.update(slots)
            continue
        pending.remove(pick)
        slots = sorted(free)[:weight(pick)]
        deps = sorted({previous[s] for s in slots if previous[s] is not None})
        start = now
        end = start + durations[pick]
        spans[pick] = dict(index=pick, command=head(pick), start_s=float(start),
                           end_s=float(end), duration_s=float(durations[pick]),
                           slots=[s + 1 for s in slots], dependencies=deps)
        for slot in slots:
            free.remove(slot)
            previous[slot] = pick
        heapq.heappush(running, (end, weight(pick), pick, slots))
    pool_end = max((Decimal(str(s['end_s'])) for s in spans.values()), default=Decimal(0))
    chain = []
    if spans:
        last = max(spans, key=lambda i: spans[i]['end_s'])
        while last is not None:
            chain.append(last)
            deps = spans[last]['dependencies']
            last = max(deps, key=lambda i: spans[i]['end_s']) if deps else None
        chain.reverse()
    exclusive_s = sum((durations[i] for i in alone), Decimal(0))
    slot_chains = [[s for s in spans.values() if slot in s['slots']] for slot in range(1, jobs + 1)]
    return dict(pool_s=float(pool_end), exclusive_s=float(exclusive_s),
                total_s=float(pool_end + exclusive_s),
                critical_chain=[spans[i] for i in chain],
                slots=slot_chains,
                exclusive=[dict(index=i, command=head(i), duration_s=float(durations[i])) for i in alone])


def read_host(name, paths):
    if len(paths) != 3 or len({Path(p).resolve() for p in paths}) != 3:
        raise ValueError(f'{name}: supply three distinct runs')
    runs = [read_run(Path(p)) for p in paths]
    cmds = runs[0]['commands']
    identities = [command_identity(c) for c in cmds]
    if any([command_identity(c) for c in r['commands']] != identities for r in runs[1:]):
        raise ValueError(f'{name}: three runs did not enumerate the identical command sequence')
    if any(r['fail'] or any(int(x['rc']) for x in r['timings']) for r in runs):
        raise ValueError(f'{name}: failed commands cannot calibrate successful workload costs')
    # Historical hosts may have different tables, but the three observations
    # within each host must use the same priority-setting budget values.
    priority = [x.get('budget', '') for x in runs[0]['timings']]
    if any([x.get('budget', '') for x in r['timings']] != priority for r in runs[1:]):
        raise ValueError(f'{name}: measured budget priorities changed between runs')
    medians = [statistics.median(Decimal(r['timings'][i]['seconds']) for r in runs)
               for i in range(len(cmds))]
    indices = {}
    for i, c in enumerate(cmds):
        indices.setdefault(c.split('\n')[0].strip(), []).append(i)
    return dict(name=name, runs=runs, commands=cmds, medians=medians, indices=indices)


def fit_rows(budgets, hosts, unmeasured=()):
    permitted = set(unmeasured)
    valid = {(h, c) for h in hosts for c in budgets}
    if permitted - valid:
        raise ValueError('unknown unmeasured host/command: ' + repr(sorted(permitted - valid)))
    fitted, rows = {}, []
    for command, (old, slots, original_reason) in budgets.items():
        host_values, host_medians, notrun, missing = {}, {}, {}, []
        for name, host in hosts.items():
            matches = host['indices'].get(command, [])
            if not matches:
                if (name, command) not in permitted:
                    raise ValueError(f'{name}: budget command absent: {command}; declare --unmeasured explicitly')
                host_values[name] = host_medians[name] = None
                missing.append(name)
                continue
            if len(matches) != 1:
                raise ValueError(f'{name}: repeated budget command: {command}')
            if (name, command) in permitted:
                raise ValueError(f'{name}: stale unmeasured declaration; command was observed: {command}')
            i = matches[0]
            values = [Decimal(r['timings'][i]['seconds']) for r in host['runs']]
            host_values[name] = [float(v) for v in values]
            host_medians[name] = statistics.median(values)
            notrun[name] = [n + 1 for n, r in enumerate(host['runs']) if i in r['notrun_ids']]
        observed = [v for v in host_medians.values() if v is not None]
        if not observed:
            raise ValueError('no measured host for budget command: ' + command)
        slowest = max(observed)
        slow_hosts = [h for h, v in host_medians.items() if v == slowest]
        new = max(5, int((slowest * Decimal('1.25') / 5).to_integral_value(rounding=ROUND_CEILING)) * 5)
        reason = original_reason
        if new > old or new > 300:
            note = (f'Task T: slowest host median {slowest} s ({", ".join(slow_hosts)}); '
                    f'three runs/host, 1.25x rounded up to 5 s; previous budget {old} s')
            reason += ('; ' if reason else '') + note
        fitted[command] = (Decimal(new), slots, reason)
        rows.append(dict(command=command, host_runs_s=host_values,
                         host_medians_s={h: float(v) if v is not None else None for h, v in host_medians.items()},
                         unmeasured_hosts=missing, max_host_median_s=float(slowest), slowest_hosts=slow_hosts,
                         old_budget_s=float(old), new_budget_s=new, slots=slots, notrun_runs=notrun,
                         classification='slack' if old > 2 * slowest else
                         'tight' if old < Decimal('1.25') * slowest else 'within-margin'))
    return fitted, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--budgets', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--host-runs', nargs=4, action='append', required=True,
                        metavar=('HOST', 'RUN1', 'RUN2', 'RUN3'))
    parser.add_argument('--unmeasured', nargs=2, action='append', default=[], metavar=('HOST', 'COMMAND'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    hosts = {}
    for name, *paths in args.host_runs:
        if name in hosts:
            raise ValueError('duplicate host: ' + name)
        hosts[name] = read_host(name, paths)
    budgets = {}
    lines = args.budgets.read_text().splitlines()
    for line in lines:
        if not line.strip() or line.startswith('#') or line.startswith('command\t'):
            continue
        fields = line.split('\t')
        if len(fields) != 4 or fields[0] in budgets:
            raise ValueError('malformed/duplicate budget row: ' + line)
        command, seconds, slots, reason = fields
        value = Decimal(seconds)
        if not value.is_finite() or value <= 0 or not 1 <= int(slots) <= 8:
            raise ValueError('invalid budget/slot count: ' + line)
        budgets[command] = (value, int(slots), reason)
    fitted, rows = fit_rows(budgets, hosts, map(tuple, args.unmeasured))
    source = args.runner.read_text()
    match = re.search(r'EXCLUSIVE=re\.compile\((.*?)\)\n', source, re.S)
    if not match:
        raise ValueError('runner EXCLUSIVE expression not found')
    exclusive = re.compile(ast.literal_eval('(' + match[1] + ')'))
    # Model the fullest measured gate, not an older host's smaller command
    # list. Cross-host matching uses logical commands; build identities and
    # compiler flags are never erased to manufacture shared observations.
    reference = max(hosts.values(), key=lambda h: len(h['commands']))
    cmds = reference['commands']
    if any(len(reference['indices'].get(c, [])) != 1 for c in budgets):
        raise ValueError('the fullest measured gate does not cover every current budget row')
    maximum_medians = []
    for command in cmds:
        identity = command_identity(command)
        values = []
        for h in hosts.values():
            matches = [i for i, c in enumerate(h['commands']) if command_identity(c) == identity]
            if len(matches) == 1:
                values.append(h['medians'][matches[0]])
        maximum_medians.append(max(values))
    modeled = {}
    for label, table in (('before', budgets), ('after', fitted)):
        declared = [table.get(c.split('\n')[0].strip(), (Decimal(1), 1, ''))[0] for c in cmds]
        modeled[label + '_budget'] = schedule(cmds, table, declared, exclusive)
        modeled[label + '_max_median'] = schedule(cmds, table, maximum_medians, exclusive)
    args.output.mkdir(parents=True, exist_ok=False)
    out = []
    for line in lines:
        key = line.split('\t')[0]
        if key in fitted:
            seconds, slots, reason = fitted[key]
            line = f'{key}\t{seconds}\t{slots}\t{reason}'
        out.append(line)
    (args.output / 'budgets.tsv').write_text('\n'.join(out) + '\n')
    report = dict(hosts={name: dict(runs=[{k: r[k] for k in ('path', 'summary', 'wall_s', 'fail', 'notrun')}
                                        for r in h['runs']]) for name, h in hosts.items()},
                  rows=rows, modeled=modeled, schedule_host=reference['name'],
                  unmeasured=[dict(host=h, command=c) for h, c in args.unmeasured],
                  limitation='Maximum-host medians and modeled slot schedules are synthetic bounds, not observed starts, post-refit walls, or qualification of NOTRUN subcases.')
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    names = list(hosts)
    table = ['| Command | ' + ' | '.join(n + ' median s' for n in names) + ' | Max s | Old s | New s | Slots |',
             '| --- | ' + ' | '.join('---:' for _ in names) + ' | ---: | ---: | ---: | ---: |']
    for row in rows:
        values = ['unmeasured' if row['host_medians_s'][n] is None else f'{row["host_medians_s"][n]:g}' for n in names]
        table.append('| `' + row['command'].replace('|', '\\|') + '` | ' + ' | '.join(values) +
                     f' | {row["max_host_median_s"]:g} | {row["old_budget_s"]:g} | {row["new_budget_s"]} | {row["slots"]} |')
    (args.output / 'medians.md').write_text('\n'.join(table) + '\n')
    print(json.dumps({key: {k: v for k, v in value.items() if k in ('pool_s', 'exclusive_s', 'total_s')}
                      for key, value in modeled.items()}, indent=2))


if __name__ == '__main__':
    main()
