#!/usr/bin/env python3
"""Client-side unlink attribution in a non-Git diagnostic archive.

No server logging is added under Raft ownership. Diagnostic results still are
not clean product timing evidence; retain an uninstrumented comparison.
"""
import argparse
import hashlib
import json
from pathlib import Path


def instrument(root):
    assert not (root/'.git').exists(),'diagnostic archives only'
    client=root/'src/client/inode_rpc.c';s=client.read_text()
    a=s.index('int efs_client_rpc_unlink(');b=s.index('\nint efs_client_rpc_rename_at(',a);body=s[a:b]
    assert 'ao-unlink-failure' not in body
    body=body.replace('''    if (slot < 0)
        return EFS_ERR_BUSY;''','''    if (slot < 0) {
        fprintf(stderr,"ao-unlink-failure us=%llu stage=admission parent=%llu name=%.255s slot=%d\\n",
                rpc_prof_now_us(),(unsigned long long)parent,name?name:"",slot);
        return EFS_ERR_BUSY;
    }''')
    body=body.replace('''    if (rc != EFS_OK)
        return rc;
    return rpc_status_to_efs(r.status);''','''    if (rc != EFS_OK) {
        fprintf(stderr,"ao-unlink-failure us=%llu stage=rpc parent=%llu name=%.255s seq=%llu rc=%d\\n",
                rpc_prof_now_us(),(unsigned long long)parent,name?name:"",(unsigned long long)q.id.seq,rc);
        return rc;
    }
    if (r.status != EFS_INODE_RPC_OK)
        fprintf(stderr,"ao-unlink-failure us=%llu stage=verdict parent=%llu name=%.255s seq=%llu status=%u\\n",
                rpc_prof_now_us(),(unsigned long long)parent,name?name:"",(unsigned long long)q.id.seq,r.status);
    return rpc_status_to_efs(r.status);''')
    assert body.count('ao-unlink-failure')==3
    client.write_text(s[:a]+body+s[b:])
    # Never add stdio to rc_to_inode_status: some callers hold the Raft
    # owner lock. Even error-only logging there perturbs apply/commit latency.
    # Client-side failure stages suffice to distinguish admission/RPC/verdict.
    host=root/'src/server/raft_host.c'
    assert 'ao_status_trace' not in host.read_text(), 'use a fresh archive, not the old intrusive probe'
    driver=root/'scripts/ao_busy_diagnostic.py'
    d=(root/'scripts/qualify_ao_readdir.py').read_text()
    d=d.replace("mount=k.Mount(c,1,'mnt')", "mount=k.Mount(c,1,'mnt')\n        (a.output/'fuse-maps.txt').write_bytes(Path(f'/proc/{mount.p.pid}/maps').read_bytes())")
    d=d.replace("extra_env={'EFS_FAULT_READDIR_LAYOUT_FILE':str(pause)})", "extra_env={'EFS_FAULT_READDIR_LAYOUT_FILE':str(pause)},quota='8G')")
    d=d.replace('c.start(4,join=2);','c.start(4,join=2,quota="8G");')
    a=d.index('        if a.posix:');b=d.index("        result['verdict']='PASS'",a)
    d=d[:a]+'''        if a.posix:
            result['diagnostic_profile']='20 bounded POSIX rounds, 8GiB/member; stop on first failure; not a replacement qualification'
            for round_index in range(20):
                argv=[sys.executable,str(ROOT/'tests/posix/posix_suite.py'),str(mount.path),
                      '--suite','core','--keep','--results',str(a.output/f'posix-{round_index:02d}.tsv'),
                      '--tag',f'ao-diag-{round_index:02d}']
                with (a.output/f'posix-{round_index:02d}.log').open('w') as log:
                    done=subprocess.run(argv,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
                result['diagnostic_rounds']=round_index+1
                print('DIAGNOSTIC round',round_index,'rc',done.returncode,flush=True)
                assert done.returncode==0,('diagnostic POSIX failure',round_index,done.returncode)
    '''.rstrip()+'\n'+d[b:]
    driver.write_text(d)
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (client,host,driver)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path)
    print(json.dumps(instrument(p.parse_args().root.resolve()),indent=2))
