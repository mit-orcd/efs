#!/usr/bin/env python3
"""Portable, source-bound checks for the graph-led repairs. Never deploys a cluster.
Each invocation requires fresh --out; original failure logs cannot be overwritten.
"""
from pathlib import Path
import argparse, hashlib, json, os, platform, shlex, subprocess
ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    cc=shlex.split(os.environ.get('CC','cc'))
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    for key in ['EFS_LEAK_WRAP','EFS_TEST_TSAN','EFS_TEST_SANITIZE','EFS_SANITIZE']:env.pop(key,None)
    if args.sanitize:env['EFS_TEST_SANITIZE']='1'
    flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-Iinclude','-pthread']
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    source=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    watched=['src/common/request_receipt.c','include/efs/request_receipt.h','src/client/data_commit.c','src/client/client_internal.h','src/client/inode_rpc.c','src/server/raft_host.c','src/raft/raft_disk.c','src/raft/raft_mem.c','tests/test_data_commit.c','tests/test_raft_store.c','tests/test_receipt_pending_pressure.py']
    manifest={'source_commit':source,'platform':platform.platform(),'compiler':subprocess.check_output(cc+['--version'],text=True),'mode':'ASan/UBSan (no leak claim)' if args.sanitize else 'native',
              'sha256':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in watched}}
    (out/'source.json').write_text(json.dumps(manifest,indent=2)+'\n')
    results=[]
    def step(name,cmd,deadline=180):
        print(name,flush=True)
        with (out/(name+'.log')).open('w') as log:
            print(shlex.join(map(str,cmd)),file=log,flush=True)
            try:rc=subprocess.run(list(map(str,cmd)),cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=deadline).returncode
            except subprocess.TimeoutExpired:rc=124
            print('exit',rc,file=log,flush=True)
        results.append({'name':name,'rc':rc,'sha256':hashlib.sha256((out/(name+'.log')).read_bytes()).hexdigest()})
        return rc
    py=['tests/test_w155_receipt_table.py','tests/test_receipt_pending_pressure.py','tests/test_w155_report_resolve.py','tests/test_w155_identified_resolve.py']
    if not args.sanitize:py+=['tests/test_report_frame_boundary.py','tests/test_txn_apply_retry.py','tests/test_make_test_recipe.py']
    for test in py:step(Path(test).stem,['python3',test])
    core=['src/raft/raft.c','src/raft/raft_mem.c','src/common/batch_obs.c']
    suites=[('data_commit',['tests/test_data_commit.c']),('raft',['tests/test_raft.c']+core),
            ('raft_store',['tests/test_raft_store.c','src/raft/raft.c','src/raft/raft_disk.c','src/raft/raft_log.c','src/raft/raft_mem.c','src/common/checksum.c','src/common/batch_obs.c','deps/blake3/blake3.c','deps/blake3/blake3_dispatch.c','deps/blake3/blake3_portable.c'])]
    for name,files in suites:
        binary=out/(name+'.bin');extra=[]
        if name=='raft_store':extra=['-Ideps/blake3','-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0']
        if step(name+'-build',cc+flags+extra+files+['-o',binary])==0:step(name,[binary])
    if not args.sanitize:step('defect-regressions',['python3','tests/review_graph_20261028/regression.py'])
    bad=[r['name'] for r in results if r['rc']]
    (out/'results.json').write_text(json.dumps({'source_commit':source,'results':results,'verdict':'FAIL' if bad else 'PASS','failures':bad},indent=2)+'\n')
    print('FAIL '+str(bad) if bad else 'PASS all scoped checks',flush=True)
    return bool(bad)


if __name__=='__main__':raise SystemExit(main())
