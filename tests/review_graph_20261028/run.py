#!/usr/bin/env python3
"""Reproduce three current defects and discriminate temporary fail-closed controls.

Exit 0 means the selected known defects were observed AND their controls stopped
those observations. It never means EFS passes these correctness requirements.
Production files are read only; controls exist solely in a temporary directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CC = shlex.split(os.environ.get('CC', 'cc'))


def run(cmd):
    print('+ ' + shlex.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True, timeout=90)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('source changed; review control patch before rerunning: ' + old[:70])
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    flags = ['-std=gnu11', '-O1', '-g', '-I' + str(ROOT/'include'), '-pthread']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    print('source=' + subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT).decode().strip(), flush=True)
    run(CC + ['--version'])
    watched = ['src/common/request_receipt.c', 'src/client/data_commit.c', 'src/raft/raft_disk.c', 'src/raft/raft_mem.c']
    print(json.dumps({p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in watched}, indent=2), flush=True)
    with tempfile.TemporaryDirectory(prefix='efs-graph-bugs-') as tmp:
        d = Path(tmp)
        for fixed in (False, True):
            print('\nCONTROL=' + str(fixed), flush=True)
            # Full production disk/memory store, framing, CRC and real local files.
            disk = (ROOT/'src/raft/raft_disk.c').read_text()
            mem = (ROOT/'src/raft/raft_mem.c').read_text()
            if fixed:
                disk = replace_once(disk, '        off = (uint32_t)(index - first);\n        if (off < g->n)',
                                    '        if (index - first >= (uint64_t)g->n) { rc = EFS_ERR_NOT_FOUND; goto out; }\n        off = (uint32_t)(index - first);\n        if (off < g->n)')
                # Only the lookup is changed, not append/truncate (separate audit).
                start = mem.index('static int mem_get(')
                mem = mem[:start] + replace_once(mem[start:], '    off = (uint32_t)(index - first);',
                    '    if (index - first >= (uint64_t)m->n) return EFS_ERR_NOT_FOUND;\n    off = (uint32_t)(index - first);')
            (d/'raft_disk.c').write_text(disk); (d/'raft_mem.c').write_text(mem)
            cmd = CC + flags + ['-I'+str(ROOT/'src/raft'), '-I'+str(ROOT/'deps/blake3'),
                '-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41', '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512', '-DBLAKE3_USE_NEON=0',
                str(HERE/'index_alias.c'), str(d/'raft_disk.c'), str(d/'raft_mem.c'),
                str(ROOT/'src/raft/raft_log.c'), str(ROOT/'src/common/checksum.c'), str(ROOT/'src/common/batch_obs.c')]
            cmd += [str(ROOT/'deps/blake3'/p) for p in ('blake3.c','blake3_dispatch.c','blake3_portable.c')]
            binary = d/'index'; run(cmd + ['-o', binary]); store=d/('store-'+str(fixed))
            run([binary, store] + (['fixed'] if fixed else []))
            # Same table/algorithm as the host, production 4096-slot capacity.
            receipt = (ROOT/'src/common/request_receipt.c').read_text()
            if fixed:
                receipt = replace_once(receipt, '        pick = done ? done : any;',
                    '        if (!done) return NULL;\n        pick = done;')
                receipt = replace_once(receipt, '    if (!r)\n        r = receipt_insert(t, k);',
                    '    if (!r) {\n        r = receipt_insert(t, k);\n        if (!r) { pthread_mutex_unlock(&t->mu); return EFS_ERR_BUSY; }\n    }')
            (d/'request_receipt.c').write_text(receipt)
            probe = (HERE/'receipt_pending.c').read_text().replace('"../../src/common/request_receipt.c"', json.dumps(str(d/'request_receipt.c')))
            (d/'receipt.c').write_text(probe); binary=d/'receipt'
            run(CC+flags+[d/'receipt.c','-o',binary]); run([binary]+(['fixed'] if fixed else []))
            # Existing data-commit fixture: only transport endpoints are stubs.
            commit = (ROOT/'src/client/data_commit.c').read_text()
            if fixed:
                commit = replace_once(commit, '                retire = 1;\n            } else {\n                if (logged++ < 8)',
                                      '                failed++; /* not-found is not retirement proof */\n            } else {\n                if (logged++ < 8)')
            (d/'data_commit.c').write_text(commit)
            fixture = (ROOT/'tests/test_data_commit.c').read_text().replace('"../src/client/data_commit.c"', json.dumps(str(d/'data_commit.c')))
            (d/'fixture.c').write_text(fixture)
            probe = (HERE/'stripe_before_publish.c').read_text().replace('"../test_data_commit.c"', json.dumps(str(d/'fixture.c')))
            (d/'stripe.c').write_text(probe); binary=d/'stripe'
            run(CC+flags+['-I'+str(ROOT/'src/client'),d/'stripe.c','-o',binary]); run([binary]+(['fixed'] if fixed else []))
    print('\nCONFIRMED: 3 defect probes; all 3 temporary control variants reject the bad observations.', flush=True)
    print('Controls are discrimination experiments, not proposed complete product fixes or live qualification.', flush=True)


if __name__ == '__main__':
    main()
