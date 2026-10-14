#!/usr/bin/env python3
"""Production read preferences retain parity and use only the existing deadline."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "read_peer_order.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
struct efs_read_peer_order hints;
static void *defer(void *unused) {
    (void)unused;
    for (unsigned i=0;i<1000;i++) efs_read_peer_defer(&hints,2,100,500);
    return NULL;
}
int main(void) {
    efs_node_id_t nodes[3]={2,1,3};
    int order[3]={0,1,2};
    efs_read_peer_prefer(&hints,nodes,order,100);
    assert(order[0]==0 && order[1]==1 && order[2]==2);
    efs_read_peer_defer(&hints,2,100,500);
    efs_read_peer_prefer(&hints,nodes,order,101);
    assert(order[0]==1 && order[1]==2 && order[2]==0);
    /* Preferred healthy data + parity, but stalled candidate still exists. */
    assert(nodes[order[0]]==1 && nodes[order[1]]==3 && nodes[order[2]]==2);
    efs_read_peer_defer(&hints,2,200,900);
    assert(hints.until[1]==500); /* no perpetual postponement */
    order[0]=0;order[1]=1;order[2]=2;
    efs_read_peer_prefer(&hints,nodes,order,500);
    assert(order[0]==0 && order[1]==1 && order[2]==2);
    efs_read_peer_defer(&hints,2,501,600);
    efs_read_peer_succeeded(&hints,2);
    assert(!hints.until[1]);
    efs_read_peer_defer(&hints,2,100,100);
    efs_read_peer_defer(&hints,0,100,900);
    efs_read_peer_defer(&hints,EFS_MAX_NODES+1,100,900);
    assert(!hints.until[1]); /* expired/cancelled work cannot add a hint */
    pthread_t workers[8];
    for(unsigned i=0;i<8;i++) assert(!pthread_create(&workers[i],NULL,defer,NULL));
    for(unsigned i=0;i<8;i++) assert(!pthread_join(workers[i],NULL));
    assert(hints.until[1]==500);
    efs_read_peer_defer(&hints,1,100,500);
    efs_read_peer_defer(&hints,3,100,500);
    order[0]=2;order[1]=0;order[2]=1;
    efs_read_peer_prefer(&hints,nodes,order,101);
    assert(order[0]==2 && order[1]==0 && order[2]==1); /* all remain candidates */
    puts("read peer preference: verified-alternative order, unchanged deadline expiry, no refresh, recovery, invalid IDs and concurrent hints PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-peer-order-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
