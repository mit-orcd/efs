#!/usr/bin/env python3
"""Behavioral regressions: default current source must PASS; --against is an explicit
negative-control experiment, not format/backward compatibility testing.
Original defect observations and logs are retained separately in run.py's report.
"""
from pathlib import Path
import argparse, hashlib, json, os, shlex, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--against',help='read production source at this Git commit (negative control only)')
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    def source(path):
        return subprocess.check_output(['git','show',args.against+':'+path],cwd=ROOT,text=True) if args.against else (ROOT/path).read_text()
    cc=shlex.split(os.environ.get('CC','cc'))
    flags=['-std=gnu11','-O1','-g','-I'+str(ROOT/'include'),'-pthread']
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    failures=[]
    with tempfile.TemporaryDirectory(prefix='efs-graph-regressions-') as temp:
        d=Path(temp)
        paths=['src/common/request_receipt.c','src/client/data_commit.c','src/raft/raft_disk.c','src/raft/raft_mem.c','src/raft/raft_log.c','src/common/checksum.c','src/common/batch_obs.c']
        for path in paths:(d/Path(path).name).write_text(source(path))
        print('source='+str(args.against or 'working tree')+'\n'+json.dumps({p:hashlib.sha256(source(p).encode()).hexdigest() for p in paths},indent=2),flush=True)
        receipt=(HERE/'receipt_pending.c').read_text().replace('"../../src/common/request_receipt.c"',json.dumps(str(d/'request_receipt.c')))
        (d/'receipt.c').write_text(receipt)
        fixture=(HERE/'transport_fixture.h').read_text().replace('"../../src/client/data_commit.c"',json.dumps(str(d/'data_commit.c')))
        (d/'fixture.h').write_text(fixture)
        stripe=(HERE/'stripe_before_publish.c').read_text().replace('#define main upstream_data_commit_main\n#include "../test_data_commit.c"\n#undef main','#include "'+str(d/'fixture.h')+'"')
        (d/'stripe.c').write_text(stripe)
        cases=[('receipt',cc+flags+[str(d/'receipt.c')],[str(d/'receipt'),'fixed']),
               ('stripe',cc+flags+['-I'+str(ROOT/'src/client'),str(d/'stripe.c')],[str(d/'stripe'),'fixed'])]
        store_flags=['-I'+str(ROOT/'src/raft'),'-I'+str(ROOT/'deps/blake3'),'-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0']
        store_src=[str(HERE/'index_alias.c')]+[str(d/f) for f in ['raft_disk.c','raft_mem.c','raft_log.c','checksum.c','batch_obs.c']]+[str(ROOT/'deps/blake3'/p) for p in ['blake3.c','blake3_dispatch.c','blake3_portable.c']]
        cases.append(('index',cc+flags+store_flags+store_src,[str(d/'index'),str(d/'store'),'fixed']))
        for name,build,run in cases:
            command=build+['-o',str(d/name)]
            print('+ '+shlex.join(command),flush=True)
            compiled=subprocess.run(command,cwd=ROOT,timeout=90).returncode
            rc=compiled if compiled else subprocess.run(run,cwd=ROOT,timeout=30).returncode
            print(name+': '+('FAIL' if rc else 'PASS')+' rc='+str(rc),flush=True)
            if rc:failures.append(name)
    print('regression failures='+str(failures),flush=True)
    return bool(failures)


if __name__=='__main__':raise SystemExit(main())
