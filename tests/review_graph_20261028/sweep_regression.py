#!/usr/bin/env python3
"""Separate negative controls for the sweep deadline and retired-object checks.

Default tests working-tree production source; --against substitutes one Git
revision in a temporary component build, never a store/wire compatibility test.
Fresh external --out is mandatory; old failures are never overwritten.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--against')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    path = 'src/client/data_commit.c'
    source = (subprocess.check_output(['git', 'show', args.against + ':' + path],
              cwd=ROOT, text=True) if args.against else (ROOT / path).read_text())
    product = out / 'data_commit.c'
    product.write_text(source)
    test = (ROOT / 'tests/test_data_commit.c').read_text()
    fixture = out / 'fixture.c'
    fixture.write_text(test.replace('"../src/client/data_commit.c"', json.dumps(str(product))))
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
             '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src/client')]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    build = cc + flags + [str(fixture), '-o', str(out / 'sweep')]
    env = dict(os.environ)
    # Apple ASan does not implement LSan; no leak-check claim is made here.
    if platform.system() == 'Darwin':
        env['ASAN_OPTIONS'] = 'detect_leaks=0'
    results = {'production': args.against or 'working tree',
               'checkout': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
               'platform': platform.platform(), 'sanitize': args.sanitize,
               'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
               'test_sha256': hashlib.sha256(test.encode()).hexdigest(), 'build': build, 'cases': {}}
    with (out / 'build.log').open('w') as log:
        results['build_exit'] = subprocess.run(build, env=env, stdout=log,
                                               stderr=subprocess.STDOUT, timeout=90).returncode
    if results['build_exit'] == 0:
        for case in ['deadline', 'retirement']:
            with (out / (case + '.log')).open('w') as log:
                results['cases'][case] = subprocess.run([str(out / 'sweep'), case], env=env,
                    stdout=log, stderr=subprocess.STDOUT, timeout=30).returncode
    results['verdict'] = ('PASS' if results['build_exit'] == 0 and
                          len(results['cases']) == 2 and not any(results['cases'].values()) else 'FAIL')
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(results['verdict'], results['cases'])
    return results['verdict'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
