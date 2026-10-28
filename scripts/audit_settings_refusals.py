#!/usr/bin/env python3
"""Task Z: record every removed-name refusal without starting a cluster.

Build efsd and efs-fuse from this source, then run with --expected-build ID.
Each probe omits required arguments, so a missing refusal fails the audit
without binding, opening a store or mounting. Help/version deliberately
bypass startup validation and are identity/control checks only. JSON lines: redirect
outside Git. Existing test_env_removed.py separately checks real startup
arguments, no store creation, and the clean-environment negative controls.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expected-build', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root/'src/common/common.c').read_text()
    table = text.split('removed_env[] = {', 1)[1].split('\n};', 1)[0]
    names = re.findall(r'\{ "(EFS_[A-Z0-9_]+)"', table)
    if not names or len(names) != len(set(names)):
        raise RuntimeError('missing or duplicate removed-name table')
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}
    failures = 0
    for program in ('efsd', 'efs-fuse'):
        binary = root/program
        version = subprocess.run([str(binary), '--version'], env=env,
                                 capture_output=True, text=True, timeout=20)
        identity = version.stdout + version.stderr
        if version.returncode or args.expected_build not in identity:
            raise RuntimeError(f'{program}: expected build {args.expected_build}: {identity}')
        print(json.dumps(dict(program=program, identity=identity.strip())), flush=True)
        control = subprocess.run([str(binary), '--help'], env=env,
                                 capture_output=True, text=True, timeout=20)
        if control.returncode or ' was removed (' in control.stderr:
            raise RuntimeError(f'{program}: clean help control failed: {control.stderr}')
        control = subprocess.run([str(binary)], env=env,
                                 capture_output=True, text=True, timeout=20)
        if control.returncode == 0 or ' was removed (' in control.stderr:
            raise RuntimeError(f'{program}: missing-arguments control failed: {control.stderr}')
        for name in names:
            for value in ('1', ''):
                result = subprocess.run([str(binary)], env=dict(env, **{name: value}),
                                        capture_output=True, text=True, timeout=20)
                ok = (result.returncode == 1 and
                      f'{program}: {name} was removed (' in result.stderr and
                      'Unset it and start again.' in result.stderr)
                failures += not ok
                print(json.dumps(dict(program=program, name=name, value=value,
                                      returncode=result.returncode, stderr=result.stderr.strip(),
                                      verdict='PASS' if ok else 'FAIL')), flush=True)
    print(json.dumps(dict(removed_names=len(names), probes=len(names)*4,
                          failures=failures, verdict='FAIL' if failures else 'PASS')), flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
