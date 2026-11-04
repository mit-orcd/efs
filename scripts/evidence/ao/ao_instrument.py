#!/usr/bin/env python3
"""AO measurement probes, applied only to an archived, non-Git source copy.

Counts actual Raft read-round starts globally (including D4/index traffic),
not a claim that each DIR admission causes a new quorum round. Page logs are
emitted only after the reply is received / the server call returns. Both
comparison builds get the identical probes. No product setting is added.
"""
from pathlib import Path
import argparse
import hashlib
import json


def instrument(root, experimental_page_size=None):
    assert not (root/'.git').exists(), 'archive experiment copies only'
    # Counterfactual only: this is NOT a qualified protocol implementation.
    # A shipping change still needs a scan cap, strict new framing and gates.
    assert experimental_page_size in (None,256)
    protocol=root/'include/efs/protocol.h'
    if experimental_page_size is not None:
        source=protocol.read_text()
        assert source.count('#define EFS_READDIR_MAX 64\n')==1
        protocol.write_text(source.replace('#define EFS_READDIR_MAX 64\n',
                                           '#define EFS_READDIR_MAX 256\n'))
    raft = root/'src/raft/raft.c'
    s = raft.read_text()
    assert 'ao_round_counts' not in s
    anchor = '#include "efs/raft.h"'
    s = s.replace(anchor, anchor + '''
static uint64_t ao_round_counts[256];
uint64_t efs_ao_read_rounds(unsigned group) {
    return group < 256 ? __atomic_load_n(&ao_round_counts[group], __ATOMIC_RELAXED) : 0;
}
''', 1)
    assert s.count('    r->read_round++;') == 1
    s = s.replace('    r->read_round++;', '''    r->read_round++;
    __atomic_add_fetch(&ao_round_counts[r->group], 1, __ATOMIC_RELAXED);''')
    raft.write_text(s)
    host = root/'src/server/raft_host.c'
    s = host.read_text()
    assert 'ao_dir_calls' not in s
    anchor = '#define HOST_TICK_US'
    assert s.count(anchor)==1
    s = s.replace(anchor, '''extern uint64_t efs_ao_read_rounds(unsigned group);
static uint64_t ao_dir_calls;

'''+anchor, 1)
    anchor = 'void server_raft_host_status(struct efs_msg_raft_status_reply *out)\n{'
    assert s.count(anchor)==1
    s = s.replace(anchor, anchor+'''
    fprintf(stderr, "ao-counters g0=%llu g2=%llu dir_calls=%llu\\n",
            (unsigned long long)efs_ao_read_rounds(0),
            (unsigned long long)efs_ao_read_rounds(2),
            (unsigned long long)__atomic_load_n(&ao_dir_calls, __ATOMIC_RELAXED));
''')
    anchor = 'void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,'
    assert s.count(anchor)==1
    s = s.replace(anchor, 'static void ao_readdir_impl(efs_ino_t parent, uint32_t max_ents,')
    anchor = '/* LOOKUP_PATH: hop-by-hop ReadIndex + lookup'
    assert s.count(anchor)==1
    s = s.replace(anchor, '''void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,
                              uint32_t after_src, const char *after_name,
                              struct efs_msg_inode_readdir_reply *out)
{
    uint64_t started=now_us_();
    __atomic_add_fetch(&ao_dir_calls, 1, __ATOMIC_RELAXED);
    ao_readdir_impl(parent,max_ents,after_src,after_name,out);
    fprintf(stderr,"ao-server parent=%llu count=%u status=%u us=%llu\\n",
            (unsigned long long)parent,out->count,out->status,
            (unsigned long long)(now_us_()-started));
}

'''+anchor)
    host.write_text(s)
    client = root/'src/client/inode_rpc.c'
    s = client.read_text(); a=s.index('int efs_client_rpc_readdir_cur('); b=s.index('\nint efs_client_rpc_writer_view(',a)
    body=s[a:b]
    body=body.replace('    int rc;','    int rc;\n    uint64_t ao_started=rpc_prof_now_us();',1)
    anchor='    if (efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR, &req, sizeof(req)) != 0) {'
    assert body.count(anchor)==1
    body=body.replace(anchor,'''    fprintf(stderr,"ao-send parent=%llu src=%u max=%u\\n",
            (unsigned long long)parent,req.after_src,req.max_ents);
'''+anchor)
    anchor='        struct efs_msg_inode_readdir_reply *r = payload;'
    assert body.count(anchor)==1
    body=body.replace(anchor,anchor+'''
        fprintf(stderr,"ao-page parent=%llu count=%u status=%u bytes=%u us=%llu\\n",
                (unsigned long long)parent,r->count,r->status,plen,
                (unsigned long long)(rpc_prof_now_us()-ao_started));''')
    client.write_text(s[:a]+body+s[b:])
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (raft,host,client,protocol)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path)
    p.add_argument('--experimental-page-size',type=int,choices=(256,),default=None,
                   help='archived performance counterfactual, never a product setting')
    args=p.parse_args()
    print(json.dumps(instrument(args.root.resolve(),args.experimental_page_size),indent=2))
