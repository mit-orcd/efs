#!/usr/bin/env python3
"""K114 fix 1: the false cut. A request that the K106 (a) cut now covers (truncate SETATTR, APPEND, CREATE_RETAINED
probe=0, REPORT with a publication id) is in flight to a member that is stopped (SIGSTOP). The client's waiter finds the
member silent, cuts the wait and sends the request again to another member. The stopped member is then resumed and the
first copy, which it was holding, runs late. Nothing may be done twice, undone or left half done.

Usage: test_k114_false_cut.py [truncate|append|create] (one scenario each, no argument runs all three; make test-long)

Three loopback daemons (tests/k33_daemons.py, the EFS_FAULTS efsd), one efs-fuse mount. Both Raft groups are led by node 3.
The first copy is held on the server by EFS_FAULT_HOLD_REQUEST_FILE (a fifo the test opens when it wants the copy to run;
no timer): the test waits for the daemon's line that the request is held, stops the member, waits for the client's
`node-waiter-cut` line, resumes the member, waits for the operation to return, lets the client go on (a later operation that
would be undone by a repeated first copy), moves leadership back to the resumed member (a deposed leader would refuse the
late copy by routing, which shows nothing about the identity), and only then releases the held first copy and waits for the
daemon's line that it was dispatched. It checks the operation succeeded in about 6 s (not the 30 s receive bound), the effect
is there exactly once, size, mtime and bytes read through a fresh mount are the same before and after the late copy, and a
mount that talks to the resumed member reads the same bytes.
  truncate  truncate to 50000 bytes, then 10000 bytes written after it: a late truncate would cut them off;
  append    O_APPEND write, then a second append: a late reserve would add the first one's bytes again;
  create    create a name, then unlink it and create it again with other content: a late create would resurrect or double.
The requests go to the leader (node 3, mount on node 1). A REPORT is not cut (K122), so it has no scenario."""
import os, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k  # noqa: E402

root = Path(__file__).resolve().parents[1]
KINDS = ('truncate', 'append', 'create')
kind = sys.argv[1] if len(sys.argv) > 1 else ''
if not kind:
    for kd in KINDS:
        rc = subprocess.run([sys.executable, __file__, kd]).returncode
        if rc:
            sys.exit(rc)
    sys.exit(0)
assert kind in KINDS, kind
HOLD_NODE, MOUNT_NODE = 3, 1


def layout(work):
    """Message types and body offsets from the real headers."""
    src = work / 'layout.c'
    src.write_text('#include <stddef.h>\n#include <stdio.h>\n#include "efs/protocol.h"\n'
                   'int main(void){printf("%u %u %u %u %zu %zu %u",(unsigned)EFS_MSG_INODE_SETATTR,'
                   '(unsigned)EFS_MSG_INODE_APPEND,(unsigned)EFS_MSG_REPORT_CHUNKS,(unsigned)EFS_MSG_CREATE_RETAINED,'
                   'offsetof(struct efs_msg_inode_setattr,mask),offsetof(struct efs_msg_create_retained,probe),'
                   '(unsigned)(EFS_SETATTR_SIZE|EFS_SETATTR_MTIME));return 0;}\n')
    subprocess.run(['cc', '-std=gnu11', '-I' + str(root / 'include'), str(src), '-o', str(work / 'layout')], check=True)
    v = subprocess.run([str(work / 'layout')], check=True, capture_output=True, text=True).stdout.split()
    return dict(zip(('setattr', 'append', 'report', 'create', 'mask_off', 'probe_off', 'size_mtime'), map(int, v)))


def main():
    if not k.fuse_ok():
        print('NOTRUN: needs FUSE (fusermount3 and /dev/fuse)')
        return 0
    work = Path(tempfile.mkdtemp(prefix=f'efs-k114-{kind}-', dir=os.environ.get('TMPDIR')))
    L = layout(work)
    hold, fifo = work / 'hold', work / 'release'
    os.mkfifo(fifo)
    spec = {'truncate': f"{L['setattr']} {L['mask_off']} {L['size_mtime']} {fifo}",
            'append': f"{L['append']} -1 0 {fifo}",
            'create': f"{L['create']} {L['probe_off']} 0 {fifo}"}[kind]
    env = {x: v for x, v in os.environ.items() if not x.startswith('EFS_FAULT_')}
    env['EFS_TRANSPORT'] = 'tcp'
    c = k.Cluster(work, 'c', (1, 2, 3), env)
    ok = False
    out = {}
    try:
        for i in (1, 2, 3):
            c.start(i, join=None if i == 1 else 1,
                    extra_env={'EFS_FAULT_HOLD_REQUEST_FILE': str(hold)} if i == HOLD_NODE else None)
        c.wait_voters((1, 2, 3), lambda i, v: v == {0: (1, 7), 2: (1, 7)})
        k.place_leaders(c, 3, groups=(0, 2), timeout=15)
        m = k.Mount(c, MOUNT_NODE, 'mnt')
        d = m.path / 'd'
        d.mkdir()
        f = d / 'f'

        def run_op(op):
            """Arm the hold, run op in a thread, stop the member when it holds the first copy, wait for the cut,
            resume the member, return (seconds, op result). The first copy is still held."""
            hold.write_text(spec + '\n')
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
            c.wait_log(HOLD_NODE, r'EFS_FAULT_HOLD_REQUEST_FILE holds type=')
            c.procs[HOLD_NODE].send_signal(signal.SIGSTOP)
            try:
                m.wait_log(r'node-waiter-cut: node=%d ' % c.node_id(HOLD_NODE), timeout=40)
            finally:
                c.procs[HOLD_NODE].send_signal(signal.SIGCONT)
            t.join(timeout=60 * k.SLOW)
            assert not t.is_alive(), 'operation did not return after the member was resumed'
            assert 'error' not in res, res
            assert res['seconds'] < 20, f"operation took {res['seconds']:.1f} s (the bound is 30 s; a cut ends it in about 6 s)"
            out['seconds'] = round(res['seconds'], 2)
            return res.get('value')

        def release(paths):
            """Let the held first copy run now, on a leader, and return the (size, mtime_ns, bytes) of paths read
            through fresh mounts before and after it ran."""
            k.place_leaders(c, HOLD_NODE, groups=(0, 2), timeout=30)
            pre = k.Mount(c, MOUNT_NODE, 'pre')
            before = {p: snap(pre.path / p.relative_to(m.path)) for p in paths}
            fd = os.open(fifo, os.O_WRONLY)
            os.close(fd)
            c.wait_log(HOLD_NODE, r'EFS_FAULT_HOLD_REQUEST_FILE dispatched type=')
            return before

        def snap(p):
            try:
                st = p.stat()
            except FileNotFoundError:
                return None
            return st.st_size, st.st_mtime_ns, p.read_bytes()

        def read(p):
            return p.read_bytes()

        if kind == 'truncate':
            f.write_bytes(b'A' * 200000)
            run_op(lambda: os.truncate(f, 50000))
            assert f.stat().st_size == 50000 and read(f) == b'A' * 50000
            with open(f, 'r+b') as h:
                h.seek(50000)
                h.write(b'Z' * 10000)
            want = b'A' * 50000 + b'Z' * 10000
            before = release([f])
            files = {f: want}
        elif kind == 'append':
            f.write_bytes(b'B' * 1000)

            def op():
                fd = os.open(f, os.O_WRONLY | os.O_APPEND)
                try:
                    os.write(fd, b'C' * 777)
                finally:
                    os.close(fd)
            run_op(op)
            assert read(f) == b'B' * 1000 + b'C' * 777 and f.stat().st_size == 1777
            fd = os.open(f, os.O_WRONLY | os.O_APPEND)
            os.write(fd, b'D' * 100)
            os.close(fd)
            want = b'B' * 1000 + b'C' * 777 + b'D' * 100
            before = release([f])
            files = {f: want}
        else:
            def op():
                fd = os.open(d / 'new', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
                try:
                    os.write(fd, b'N' * 500)
                finally:
                    os.close(fd)
            run_op(op)
            assert sorted(os.listdir(d)) == ['new'] and read(d / 'new') == b'N' * 500
            ino = (d / 'new').stat().st_ino
            os.unlink(d / 'new')
            (d / 'new').write_bytes(b'M' * 5)
            assert (d / 'new').stat().st_ino != ino
            before = release([d / 'new'])
            files = {d / 'new': b'M' * 5}
            out['names'] = sorted(os.listdir(d))
            assert out['names'] == ['new'], out
        # The late copy has run. Same bytes through this mount and through one that talks to the resumed member.
        c.leader_placement(1)
        m2 = k.Mount(c, HOLD_NODE, 'mnt2')
        for p, want in files.items():
            q = m2.path / p.relative_to(m.path)
            assert read(p) == want and read(q) == want, (p, len(read(p)), len(read(q)), len(want))
            assert p.stat().st_size == len(want) == q.stat().st_size
            assert before[p] is not None and before[p][2] == want, (p, before[p] and len(before[p][2]))
            after = snap(q)
            assert after[:2] == before[p][:2], f'{p.name}: size and mtime moved by the late first copy: {before[p][:2]} -> {after[:2]}'
        if kind == 'create':
            assert sorted(os.listdir(m2.path / 'd')) == ['new']
        print(f"K114 false cut {kind}: operation returned in {out['seconds']} s, effect once, later operation stands after "
              f"the late first copy, resumed member reads the same bytes PASS", flush=True)
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
