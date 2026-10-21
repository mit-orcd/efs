#!/usr/bin/env python3
"""Freeze completed ORCD scale evidence, then verify every acknowledged payload.
Writer failures are preserved and do not become successful qualification results.
"""
import argparse
import json
import os
import pathlib
import socket
import sys
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('journal')
    p.add_argument('mail')
    p.add_argument('snapshot')
    p.add_argument('output')
    p.add_argument('verifier')
    p.add_argument('--wait-seconds', type=int, default=6500)
    a = p.parse_args()
    host = socket.gethostname().split('.')[0]
    hosts = ['fcstor003', 'fcstor004', 'fcstor005', 'fcstor006']
    assert host in hosts
    journal = json.loads(pathlib.Path(a.journal).read_text())
    scale = journal['scale']
    assert sorted(scale['jobs']) == hosts
    deadline = time.monotonic() + a.wait_seconds
    manifests = {}
    results = {}
    while time.monotonic() < deadline:
        try:
            for node in hosts:
                result = pathlib.Path(a.mail) / node / (scale['jobs'][node] + '.result.json')
                results[node] = json.loads(result.read_text())
                assert results[node]['rc'] in (0, 1), 'writer interrupted or harness failed'
                assert not results[node]['leftover'], 'writer children remain'
                manifest = pathlib.Path(scale['run']) / (node + '-scale.json')
                manifests[node] = json.loads(manifest.read_text())
                assert manifests[node].get('complete'), 'writer manifest incomplete'
                assert sum(manifests[node]['per_worker']) == manifests[node]['files']
            break
        except FileNotFoundError:
            time.sleep(5)
    else:
        raise SystemExit('writer completion deadline exceeded; no partial manifest verified')
    snapshot = pathlib.Path(a.snapshot) / host
    snapshot.mkdir(parents=True, exist_ok=False)
    output = pathlib.Path(a.output)
    assert not (output / (host + '.json')).exists(), 'never overwrite verification evidence'
    for node in hosts:
        (snapshot / (node + '-scale.json')).write_text(json.dumps(manifests[node], indent=2))
    (snapshot / 'writer-results.json').write_text(json.dumps(results, indent=2))
    print('Final manifests preserved; writer failures remain failures. Starting full verification.', flush=True)
    os.execv(sys.executable, [sys.executable, a.verifier, str(snapshot), str(output),
                             '--fixture-root', scale['fixture_root']])


if __name__ == '__main__':
    main()
