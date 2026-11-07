#!/usr/bin/env python3
"""Task DW: the K38 shared-chunk writers and verifier on one client host.

The write and verify phases of tests/test_k38_shared_chunk_pressure.py, cut
out so two efs-fuse mounts on two hosts can share them: record k of writer r
is payload(f'k38-{r}-{k}', 47008) at offset (k * writers + r) * 47008 of one
file, writer r runs on the client with r % clients == index. Python 3.9.

  write  --mount M --index I [--clients 2 --writers 24 --records 200
         --fsyncs 1 --start-at EPOCH]   one JSON line: errors, seconds
  verify --mount M --what NAME           one JSON line: records that differ
  size   --mount M                        create/truncate the shared file
"""
import argparse, hashlib, json, os, sys, threading, time

XFER, CHUNK = 47008, 128 << 10


def record(r, k):
    return hashlib.shake_256(f'k38-{r}-{k}'.encode()).digest(XFER)


def writer(path, r, opt, out):
    res = {'writer': r, 'written': 0, 'fsyncs': 0, 'stage': 'open', 'errors': []}
    out[r] = res
    every = max(1, opt.records // opt.fsyncs)
    fd = None
    try:
        fd = os.open(path, os.O_WRONLY)
        for k in range(opt.records):
            res['stage'] = 'pwrite'
            n = os.pwrite(fd, record(r, k), (k * opt.writers + r) * XFER)
            if n != XFER:
                raise OSError(f'short pwrite {n}')
            res['written'] += 1
            if (k + 1) % every == 0:
                res['stage'] = 'fsync'
                os.fsync(fd)
                res['fsyncs'] += 1
        res['stage'] = 'final-fsync'
        os.fsync(fd)
        res['fsyncs'] += 1
    except Exception as e:  # noqa: BLE001 - keep the errno text (EIO is the K38 failure)
        res['errors'].append(f"{res['stage']}: {e!r}")
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except Exception as e:  # noqa: BLE001
                res['errors'].append(f'close: {e!r}')


def write(opt):
    path = os.path.join(opt.mount, 'shared')
    mine = [r for r in range(opt.writers) if r % opt.clients == opt.index]
    out = {}
    threads = [threading.Thread(target=writer, args=(path, r, opt, out)) for r in mine]
    if opt.start_at:
        time.sleep(max(0.0, opt.start_at - time.time()))   # both hosts start together
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=max(1.0, 600 - (time.time() - t0)))
    alive = sum(t.is_alive() for t in threads)
    errors = [f"writer {r} after {o['written']} records: {e}"
              for r, o in sorted(out.items()) for e in o['errors']]
    if alive:
        errors.append(f'{alive} writers did not finish in 600 s')
    return {'ok': not errors, 'writers': mine, 'seconds': round(time.time() - t0, 2),
            'start': t0, 'errors': errors, 'outcomes': [out[r] for r in sorted(out)]}


def verify(opt):
    with open(os.path.join(opt.mount, 'shared'), 'rb') as f:
        data = f.read()
    size = opt.writers * opt.records * XFER
    if len(data) != size:
        return {'ok': False, 'what': opt.what, 'size': len(data), 'want': size, 'bad': -1, 'lines': []}
    bad = [(r, k) for k in range(opt.records) for r in range(opt.writers)
           if data[(k * opt.writers + r) * XFER:(k * opt.writers + r + 1) * XFER] != record(r, k)]
    lines = []
    for r, k in bad[:4]:
        o = (k * opt.writers + r) * XFER
        got, want = data[o:o + XFER], record(r, k)
        diff = [i for i in range(XFER) if got[i] != want[i]]
        lines.append(f'writer {r} record {k}: bytes {o + diff[0]}..{o + diff[-1]} '
                     f'({len(diff)} differ, {got[diff[0]:diff[-1] + 1].count(0)} zero), '
                     f'chunks {(o + diff[0]) // CHUNK}..{(o + diff[-1]) // CHUNK}')
    return {'ok': not bad, 'what': opt.what, 'size': len(data), 'bad': len(bad),
            'records': opt.writers * opt.records, 'lines': lines}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('cmd', choices=('write', 'verify', 'size'))
    ap.add_argument('--mount', required=True)
    ap.add_argument('--index', type=int, default=0)
    ap.add_argument('--clients', type=int, default=2)
    ap.add_argument('--writers', type=int, default=24)
    ap.add_argument('--records', type=int, default=200)
    ap.add_argument('--fsyncs', type=int, default=1)
    ap.add_argument('--start-at', type=float, default=0)
    ap.add_argument('--what', default='')
    opt = ap.parse_args()
    if opt.cmd == 'size':
        with open(os.path.join(opt.mount, 'shared'), 'wb') as f:
            f.truncate(opt.writers * opt.records * XFER)
        res = {'ok': True}
    else:
        res = write(opt) if opt.cmd == 'write' else verify(opt)
    print(json.dumps(res))
    return 0 if res['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
