#!/usr/bin/env python3
"""Run independent W95 churn on unique private roots; never uses EFS mounts."""
import argparse
import itertools
import json
import re
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=60)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10000 or not 1 <= args.repetitions <= 100:
        parser.error('rounds/repetitions out of bounds')
    binary = args.binary.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='w95-churn-', dir=args.root))
    rows = []
    print(f'Evidence: {work}', flush=True)
    for rep, cohort, mixed in itertools.product(range(args.repetitions), (1, 32), (0, 1)):
        models = ['files', 'indexed']
        if (rep + cohort + mixed) % 2:
            models.reverse()
        for model in models:
            root = Path(tempfile.mkdtemp(prefix='owned-', dir=work))
            command = [str(binary), model, str(root), str(args.rounds), str(cohort), str(mixed)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=1200)
            log = f'{rep}-{cohort}-{mixed}-{model}.log'
            (work / log).write_text(result.stdout + result.stderr)
            if result.returncode or not result.stdout.startswith('CHURN_OK '):
                raise RuntimeError(f'Failed; preserve {root}: {result.stdout}{result.stderr}')
            row = dict(re.findall(r'(\w+)=([^\s]+)', result.stdout))
            assert int(row['verified']) == 128 and int(row['ops']) == args.rounds * 128
            row.update(rep=rep, command=command, log=log)
            rows.append(row)
            (work / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
            # Only this run's freshly created root is owned; no recursive deletion.
            for path in root.iterdir():
                assert path.is_file() and not path.is_symlink()
                path.unlink()
            root.rmdir()
            print(f'{rep} {model} cohort={cohort} mixed={mixed} '
                  f"ops/s={row['ops_s']} p99_us={row['p99_us']}", flush=True)
    print(f'PASS: {len(rows)} verified samples; {work}', flush=True)


if __name__ == '__main__':
    main()
