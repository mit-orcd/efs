#!/usr/bin/env python3
"""K114 fix 2, live: a read-only request in flight to a halted leader is cut and repeated, the result is the same.

Three loopback daemons and a mount on node 1; node 3 leads. The held member (node 3 for GETCHUNKS, node 1 for a query) carries the hold hook (EFS_FAULT_HOLD_REQUEST_FILE).
The hook holds the first request of the type under test inside that member; the test stops it (SIGSTOP: the
connection stays open and silent), waits for the client's node-waiter-cut line, resumes it, and asserts that the
operation returned the answer it gave before the stop, in about 6 s and not the 30 s transport timeout.
Usage: test_k114_read_sites_live.py getchunks|query (one scenario each, no argument runs both; make test-long):
 getchunks  reading a file through a fresh mount (no cached chunk map) sends GETCHUNKS: the bytes are the ones written;
 query      the `.efs/find` command of the mount sends QUERY_SNAPSHOT (an unpinned first page): the same names as before.
Readdir, writer view, lane writer view, publication status and the receipt query are not covered (no repeat on another
member after a failed receive; docs/status/2026-11/k114-fix2-read-sites-20261111.md), so they have no live scenario."""
import json, os, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent / 'live'))
from query_timing import wait_indexed  # noqa: E402

root = Path(__file__).resolve().parents[1]
KINDS = ('getchunks', 'query')
kind = sys.argv[1] if len(sys.argv) > 1 else ''
if not kind:
    for kd in KINDS:
        rc = subprocess.run([sys.executable, __file__, kd]).returncode
        if rc:
            sys.exit(rc)
    sys.exit(0)
assert kind in KINDS, kind
MOUNT_NODE = 1
# GETCHUNKS goes to the leader (node 3); a query to the first dual voter, node 1 (a follower)
HOLD_NODE = 3 if kind == 'getchunks' else 1


def msg_type(work, name):
    src = work / 'layout.c'
    src.write_text('#include <stdio.h>\n#include "efs/protocol.h"\n'
                   f'int main(void){{printf("%u",(unsigned){name});return 0;}}\n')
    subprocess.run(['cc', '-std=gnu11', '-I' + str(root / 'include'), str(src), '-o', str(work / 'layout')], check=True)
    return int(subprocess.run([str(work / 'layout')], check=True, capture_output=True, text=True).stdout)


def main():
    if not k.fuse_ok():
        print('NOTRUN: needs FUSE (fusermount3 and /dev/fuse)')
        return 0
    work = Path(tempfile.mkdtemp(prefix=f'efs-k114r-{kind}-', dir=os.environ.get('TMPDIR')))
    mtype = msg_type(work, {'getchunks': 'EFS_MSG_INODE_GETCHUNKS', 'query': 'EFS_MSG_QUERY_SNAPSHOT'}[kind])
    hold, fifo = work / 'hold', work / 'release'
    os.mkfifo(fifo)
    env = {x: v for x, v in os.environ.items() if not x.startswith('EFS_FAULT_')}
    env['EFS_TRANSPORT'] = 'tcp'
    env['EFS_QUERY_REFRESH_MS'] = '2000'  # the default 60 s catalog refresh would be the whole run time
    c = k.Cluster(work, 'c', (1, 2, 3), env)
    ok = False
    try:
        for i in (1, 2, 3):
            c.start(i, join=None if i == 1 else 1,
                    extra_env={'EFS_FAULT_HOLD_REQUEST_FILE': str(hold)} if i == HOLD_NODE else None)
        c.wait_voters((1, 2, 3), lambda i, v: v == {0: (1, 7), 2: (1, 7)})
        k.place_leaders(c, 3, groups=(0, 2), timeout=15)
        m = k.Mount(c, MOUNT_NODE, 'mnt')
        d = m.path / 'd'
        d.mkdir()
        want = bytes(range(251)) * 800  # 200800 bytes
        (d / 'f.txt').write_bytes(want)
        if kind == 'getchunks':
            reader = k.Mount(c, MOUNT_NODE, 'rd')  # fresh mount: no chunk map cached
            target = reader
            op = lambda: (reader.path / 'd' / 'f.txt').read_bytes()  # noqa: E731
            expect = want
        else:
            target = m
            for i in (1, 2, 3):  # every member serves queries from its own catalog: wait until each one has indexed the file
                c.wait_log(i, r'query-publish: .* files=[1-9]', timeout=120)
            wait_indexed(m.path, 300)

            def op():
                r = subprocess.run([sys.executable, str(m.path / '.efs' / 'find'), 'f', '--match', 'prefix', '--json'], cwd='/',
                                   capture_output=True, timeout=120)
                assert r.returncode == 0, r.stderr
                return json.loads(r.stdout)['entries']  # the catalog's names, without the per-call snapshot fields
            expect = op()  # the answer before the stop
        hold.write_text(f'{mtype} -1 0 {fifo}\n')
        res = {}

        def body():
            t0 = time.monotonic()
            try:
                res['value'] = op()
            except BaseException as e:  # noqa: BLE001 - reported below
                res['error'] = repr(e)
            res['seconds'] = time.monotonic() - t0
        t = threading.Thread(target=body)
        t.start()
        c.wait_log(HOLD_NODE, r'EFS_FAULT_HOLD_REQUEST_FILE holds type=%d\b' % mtype)
        c.procs[HOLD_NODE].send_signal(signal.SIGSTOP)
        try:
            target.wait_log(r'node-waiter-cut: node=%d ' % c.node_id(HOLD_NODE), timeout=40)
        finally:
            c.procs[HOLD_NODE].send_signal(signal.SIGCONT)
        t.join(timeout=60 * k.SLOW)
        assert not t.is_alive(), 'operation did not return after the member was resumed'
        assert 'error' not in res, res
        assert res['seconds'] < 20, f"operation took {res['seconds']:.1f} s (the bound is 30 s; a cut ends it in about 6 s)"
        assert res['value'] == expect, f'{kind}: answer after the cut {res["value"]!r} differs from the answer before the stop {expect!r}'
        fd = os.open(fifo, os.O_WRONLY)  # let the held first copy run: it is a read
        os.close(fd)
        c.wait_log(HOLD_NODE, r'EFS_FAULT_HOLD_REQUEST_FILE dispatched type=')
        print(f"K114 read site {kind}: operation returned in {res['seconds']:.2f} s with the answer it gave before the stop PASS",
              flush=True)
        ok = True
    finally:
        for p in c.procs.values():
            if p.poll() is None:
                p.send_signal(signal.SIGCONT)
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            os.close(fd)
        except OSError:
            pass
        c.stop_all()
        if ok and not os.environ.get('K114_KEEP'):
            subprocess.run(['rm', '-rf', str(work)])
        else:
            print(f'kept {work}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
