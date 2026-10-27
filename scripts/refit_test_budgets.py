#!/usr/bin/env python3
"""W160 offline three-run calibration; does not execute or alter any test.

Read three summary-runner result directories (cmds.txt, timings.tsv,
summary.txt). Emit a proposed budgets.tsv, aggregate medians and deterministic
slot-reservation schedules. Measured gate walls and modeled schedules stay
separate: timings.tsv does not record actual command start times.
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
    if not match or int(match[1]) != len(cmds):
        raise ValueError('incomplete summary: ' + str(path))
    with (path / 'timings.tsv').open() as stream:
        rows = list(csv.DictReader(stream, delimiter='\t'))
    if len(rows) != len(cmds) or {int(r['index']) for r in rows} != set(range(len(cmds))):
        raise ValueError('missing or duplicate command timings: ' + str(path))
    rows.sort(key=lambda r: int(r['index']))
    for i, row in enumerate(rows):
        if row['command'] != cmds[i].split('\n')[0][:150]:
            raise ValueError('timing does not identify enumerated command: ' + str(path))
        if Decimal(row['seconds']) < 0:
            raise ValueError('negative duration: ' + str(path))
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--budgets', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('runs', type=Path, nargs=3)
    args = parser.parse_args()
    runs = [read_run(p) for p in args.runs]
    cmds = runs[0]['commands']
    if any([command_identity(c) for c in run['commands']] !=
           [command_identity(c) for c in cmds] for run in runs[1:]):
        raise ValueError('three runs did not enumerate the identical command sequence')
    # A failed/partial test's duration is not a successful workload cost.
    if any(run['fail'] or any(int(row['rc']) for row in run['timings']) for run in runs):
        raise ValueError('failed commands: preserve/report failures; do not fit truncated workloads')
    rows = []
    budgets = {}
    lines = args.budgets.read_text().splitlines()
    for line in lines:
        if not line.strip() or line.startswith('#') or line.startswith('command\t'):
            continue
        fields = line.split('\t')
        if len(fields) != 4 or fields[0] in budgets:
            raise ValueError('malformed/duplicate budget row: ' + line)
        command, seconds, slots, reason = fields
        if Decimal(seconds) <= 0 or not 1 <= int(slots) <= 8:
            raise ValueError('invalid budget/slot count: ' + line)
        budgets[command] = (Decimal(seconds), int(slots), reason)
    indices = {}
    for i, command in enumerate(cmds):
        indices.setdefault(command.split('\n')[0].strip(), []).append(i)
    for run in runs:
        for i, row in enumerate(run['timings']):
            command = cmds[i].split('\n')[0].strip()
            expected = budgets.get(command)
            observed = row.get('budget', '')
            if ((expected is None) != (observed == '') or
                    (expected is not None and Decimal(observed) != expected[0])):
                raise ValueError('measurement used another budget table: ' + command)
    medians = [statistics.median([Decimal(run['timings'][i]['seconds']) for run in runs]) for i in range(len(cmds))]
    fitted = {}
    for command, (old, slots, reason) in budgets.items():
        if len(indices.get(command, [])) != 1:
            raise ValueError('budget command absent or repeated in gate: ' + command)
        i = indices[command][0]
        values = [Decimal(run['timings'][i]['seconds']) for run in runs]
        median = medians[i]
        new = max(5, int((median * Decimal('1.25') / 5).to_integral_value(rounding=ROUND_CEILING)) * 5)
        if median > old or (new > 300 and not reason):
            note = f'Task J: {args.host} three-run median {median} s; previous budget {old} s; 1.25x rounded up to 5 s'
            reason += ('; ' if reason else '') + note
        fitted[command] = (Decimal(new), slots, reason)
        rows.append(dict(command=command, runs_s=[float(v) for v in values], median_s=float(median),
                         old_budget_s=float(old), new_budget_s=new, slots=slots,
                         notrun_runs=[n + 1 for n, run in enumerate(runs) if i in run['notrun_ids']],
                         classification='slack' if old > 2 * median else 'tight' if old < Decimal('1.25') * median else 'within-margin'))
    source = args.runner.read_text()
    match = re.search(r'EXCLUSIVE=re\.compile\((.*?)\)\n', source, re.S)
    if not match:
        raise ValueError('runner EXCLUSIVE expression not found')
    exclusive = re.compile(ast.literal_eval('(' + match[1] + ')'))
    modeled = {}
    for label, table in (('before', budgets), ('after', fitted)):
        declared = [table.get(c.split('\n')[0].strip(), (Decimal(1), 1, ''))[0] for c in cmds]
        modeled[label + '_budget'] = schedule(cmds, table, declared, exclusive)
        modeled[label + '_median'] = schedule(cmds, table, medians, exclusive)
    args.output.mkdir(parents=True, exist_ok=False)
    out = []
    for line in lines:
        key = line.split('\t')[0]
        if key in fitted:
            seconds, slots, reason = fitted[key]
            line = f'{key}\t{seconds}\t{slots}\t{reason}'
        out.append(line)
    (args.output / 'budgets.tsv').write_text('\n'.join(out) + '\n')
    report = dict(host=args.host, runs=[{k:r[k] for k in ('path','summary','wall_s','fail','notrun')} for r in runs],
                  rows=rows, modeled=modeled,
                  limitation='Schedules are deterministic models using rounded measured durations, not observed command start times or new post-refit wall measurements.')
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    table = ['| Command | Run 1 s | Run 2 s | Run 3 s | Median s | Old s | New s | Slots | Assessment | NOTRUN runs |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |']
    for row in rows:
        values = row['runs_s']
        table.append('| `' + row['command'].replace('|', '\\|') + '` | ' + ' | '.join(f'{v:g}' for v in values) +
                     f" | {row['median_s']:g} | {row['old_budget_s']:g} | {row['new_budget_s']} | {row['slots']} | {row['classification']} | {','.join(map(str, row['notrun_runs'])) or 'none'} |")
    (args.output / 'medians.md').write_text('\n'.join(table) + '\n')
    print(json.dumps({key: {k:v for k,v in value.items() if k in ('pool_s','exclusive_s','total_s')} for key,value in modeled.items()}, indent=2))


if __name__ == '__main__':
    main()
