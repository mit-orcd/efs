#!/usr/bin/env python3
"""Offline check of a failed scripts/soak.py run's retained stores.

Restarts the run's own members from WORK/cluster/n* on their original
private endpoints with the same build, waits for full protection, mounts two
fresh readers and reads every acknowledged current version (the last
`ack-fsync` row per name in OUT/timeline.jsonl) and every acknowledged shared
append. A name whose last write after its acknowledgement failed in the run
is reported separately: the failed call may have changed bytes, so it is
neither a loss nor a pass. Stops everything it started; never fences,
cleans up sessions or edits the stores beyond what a daemon start does.

    python3 scripts/soak_verify_retained.py WORK OUT RESULT_DIR
"""
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import soak as S


def main(work, out, res):
    work, out, res = Path(work), Path(out), Path(res)
    res.mkdir(parents=True)
    result = json.loads((out/'result.json').read_text())
    port_base = result['config']['port_base']
    rows = [json.loads(l) for l in (out/'timeline.jsonl').read_text().splitlines() if l.strip()]
    expected, appends, suspect = {}, {0: [], 1: []}, {}
    for r in rows:
        if r['kind'] == 'ack-fsync':
            expected[r['request']['name']] = r['expected']
            suspect.pop(r['request']['name'], None)
        elif r['kind'] == 'ack-append-fsync':
            appends[r['writer']].append(r['seq'])
        elif r['kind'] == 'command' and r['rc'] and r['label'] in ('create', 'overwrite', 'truncate'):
            suspect[r['label']+'@'+str(r['t'])] = r['output'][-400:]
    # The failed name is in the child's request; the parent's first-failure trace names it.
    failed = {r['trace'].split("'name': '")[1].split("'")[0] for r in rows
              if r['kind'] == 'first-failure' and "'name': '" in r['trace']}
    env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_', 'LD_PRELOAD'))}
    env['EFS_TRANSPORT'] = 'tcp'
    ids = sorted(int(p.name[1:]) for p in (work/'cluster').glob('n[0-9]') if p.is_dir())
    c = S.Cluster(res, 'shell', ids, env)
    for h in c.held:
        h.close()
    c.held, c.dir = [], work/'cluster'
    c.ports = {i: port_base+i-1 for i in ids}
    doc = dict(source=result['source'], members=ids, failed_names=sorted(failed),
               failed_writes=suspect, acknowledged_files=len(expected),
               acknowledged_appends={w: len(v) for w, v in appends.items()})
    mounts, began = [], time.monotonic()
    try:
        for i in ids:
            c.restart(i, quota=result['config']['quota'])
            c.wait_log(i, r'node \d+ listening on ', timeout=300)
        end = time.monotonic()+600
        while True:
            text = S.subprocess.run([str(S.MGMT), 'status', c.ep(1)], capture_output=True,
                                    text=True, timeout=60, env=env).stdout
            prot = S.protection(text)
            if S.clean_protection(prot):
                break
            assert time.monotonic() < end, 'protection never full: '+text[-2000:]
            time.sleep(5)
        doc['protection'] = prot
        doc['settle_seconds'] = time.monotonic()-began
        mounts = [S.Mount(c, 1, 'diag-reader-0'), S.Mount(c, 2, 'diag-reader-1')]
        bad, skipped = [], []
        for m in mounts:
            for name, want in sorted(expected.items()):
                got = S.digest(m.path/name)
                if got != want:
                    (skipped if name in failed else bad).append(dict(mount=str(m.path.name), name=name, got=got, want=want))
            ok, app = S.APP.verify(m.path/'soak-shared', appends, {})
            doc.setdefault('appends', []).append(dict(mount=m.path.name, ok=ok, detail=app))
            if not ok:
                bad.append(dict(mount=m.path.name, name='soak-shared', detail=app))
        doc.update(mismatches=bad, failed_name_reads=skipped,
                   verdict='PASS' if not bad else 'FAIL', seconds=time.monotonic()-began)
    except BaseException as e:
        doc.update(verdict='FAIL', reason=repr(e))
        raise
    finally:
        for m in mounts:
            m.close()
        c.stop_all()
        (res/'result.json').write_text(json.dumps(doc, indent=2, default=str))
        print(json.dumps({k: doc.get(k) for k in ('verdict', 'acknowledged_files', 'acknowledged_appends',
                                                     'failed_names', 'mismatches', 'failed_name_reads', 'seconds')},
                         default=str))
    return 0 if doc['verdict'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:4]))
