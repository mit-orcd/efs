#!/usr/bin/env python3
"""Export-only K48 page trace under existing EFS_RPC_PROF; never qualification.

Records cookies, replica IDs and synthetic directory names/inodes, no file
contents. It adds no retry, authority, deadline or metadata mutation.
"""
import argparse
import hashlib
import json
from pathlib import Path


def apply(root):
    root=root.resolve();assert not (root/'.git').exists()
    marker=root/'an-readdir-probe.json';assert not marker.exists()
    names=('src/client/inode_rpc.c','src/server/raft_host.c')
    before={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names}
    for name,signature,anchor,insert in (
        (names[0],'int efs_client_rpc_readdir_cur(', '        n = r->count;',r'''
        if (prof) {
            fprintf(stderr,"an-readdir-client t_us=%llu nid=%u parent=%llu in=%u/%s n=%u next=%u/%s done=%u\n",
                rpc_prof_now_us(),(unsigned)nid,(unsigned long long)parent,
                req.after_src,req.after_name,r->count,r->next_src,r->next_name,r->next_done);
            for(uint32_t an_i=0;an_i<r->count;an_i++)
                fprintf(stderr,"an-readdir-client-name parent=%llu in=%u/%s i=%u name=%s ino=%llu\n",
                    (unsigned long long)parent,req.after_src,req.after_name,an_i,
                    r->ents[an_i].name,(unsigned long long)r->ents[an_i].ino);
        }
'''),
        (names[1],'void server_raft_host_readdir(',None,r'''
    if (getenv("EFS_RPC_PROF")) {
        fprintf(stderr,"an-readdir-server t_us=%llu nid=%u parent=%llu in=%u/%s rc=%d status=%u n=%u next=%u/%s done=%u\n",
            (unsigned long long)efs_batch_obs_now_us(),(unsigned)h->raft_id,
            (unsigned long long)parent,after_src,after_name?after_name:"",rc,out->status,
            out->count,out->next_src,out->next_name,out->next_done);
        for(uint32_t an_i=0;an_i<out->count;an_i++)
            fprintf(stderr,"an-readdir-server-name parent=%llu in=%u/%s i=%u name=%s ino=%llu\n",
                (unsigned long long)parent,after_src,after_name?after_name:"",an_i,
                out->ents[an_i].name,(unsigned long long)out->ents[an_i].ino);
    }
''')):
        path=root/name;text=path.read_text();start=text.index(signature)
        end=text.index('\n}',start)+2;body=text[start:end]
        if anchor:
            assert body.count(anchor)==1
            body=body.replace(anchor,insert+anchor,1)
        else:body=body[:-1]+insert+'}'
        path.write_text(text[:start]+body+text[end:])
    after={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in names}
    marker.write_text(json.dumps(dict(before=before,after=after,
        purpose='DIAGNOSTIC page trace; never native/performance qualification'),indent=2)+'\n')
    print(marker)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('exported_tree',type=Path)
    apply(p.parse_args().exported_tree)
