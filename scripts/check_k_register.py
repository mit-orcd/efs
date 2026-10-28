#!/usr/bin/env python3
"""Check the two alpha K-register tables; this validates structure, not evidence.

Usage: python3 scripts/check_k_register.py [CHECKLIST] [--json]
Each table must have exactly one row per K number, from K1 through its last
number, with a plain canonical state first. Both tables must agree on states.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

STATES = ('FIXED', 'VERIFIED', 'VERIFY', 'DECIDED', 'CLOSED', 'OPEN')
DEFAULT = Path(__file__).resolve().parents[1] / 'docs/status/alpha-1.0-2plus1-checklist.md'
HEADERS = {
    ('ID', 'Item', 'Kind', 'Disposition'): 'register',
    ('Row', 'Finding on devel'): 'recheck',
}


def check(text):
    """Return diagnostics and per-table states without changing the document."""
    errors, tables = [], {}
    active = None
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.startswith('|'):
            active = None
            continue
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        header = tuple(cells)
        if header in HEADERS:
            active = HEADERS[header]
            if active in tables:
                errors.append(f'line {lineno}: repeated {active} table')
            tables.setdefault(active, {})
            continue
        if active is None:
            if re.match(r'K[0-9]', cells[0]):
                errors.append(f'line {lineno}: K row outside a recognized table')
            continue
        if all(re.fullmatch(r':?-+:?', c) for c in cells):
            continue
        if not cells[0].startswith('K'):
            errors.append(f'line {lineno}: invalid {active} row id {cells[0]!r}')
            continue
        match = re.fullmatch(r'K([1-9][0-9]*)', cells[0])
        if not match:
            errors.append(f'line {lineno}: one K number required per row: {cells[0]!r}')
            continue
        number = int(match[1])
        # Status is the last cell; existing register prose has no unescaped pipes.
        expected_columns = 4 if active == 'register' else 2
        if len(cells) != expected_columns:
            errors.append(f'line {lineno}: expected {expected_columns} cells in {active}')
            continue
        state_match = re.match(r'(' + '|'.join(STATES) + r')(?=$|[\s:.,;(])', cells[-1])
        state = state_match[1] if state_match else None
        if state is None:
            errors.append(f'line {lineno}: K{number} {active} status must start with a canonical word')
        if number in tables[active]:
            errors.append(f'line {lineno}: duplicate K{number} in {active}')
        else:
            tables[active][number] = state
    for name in HEADERS.values():
        if not tables.get(name):
            errors.append(f'missing or empty {name} table')
    numbers = set().union(*(set(rows) for rows in tables.values())) if tables else set()
    if numbers:
        for name in HEADERS.values():
            missing = set(range(1, max(numbers) + 1)) - set(tables.get(name, {}))
            if missing:
                errors.append(f'{name} missing ' + ', '.join(f'K{k}' for k in sorted(missing)))
        for number in sorted(numbers):
            a = tables.get('register', {}).get(number)
            b = tables.get('recheck', {}).get(number)
            if a and b and a != b:
                errors.append(f'K{number}: states disagree ({a} / {b})')
    return errors, tables


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checklist', nargs='?', type=Path, default=DEFAULT)
    parser.add_argument('--json', action='store_true', help='print states/counts and diagnostics as JSON')
    args = parser.parse_args()
    try:
        errors, tables = check(args.checklist.read_text())
    except OSError as exc:
        parser.exit(1, f'K register: {exc}\n')
    counts = Counter(tables.get('register', {}).values())
    result = {'counts': {s: counts[s] for s in STATES},
              'rows': {f'K{k}': v for k, v in sorted(tables.get('register', {}).items())},
              'errors': errors}
    if args.json:
        print(json.dumps(result, indent=2))
    elif errors:
        print('\n'.join('FAIL K register: ' + e for e in errors))
    else:
        print('PASS K register: ' + str(len(result['rows'])) + ' rows in each table; ' +
              ', '.join(f'{s}={counts[s]}' for s in STATES))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
