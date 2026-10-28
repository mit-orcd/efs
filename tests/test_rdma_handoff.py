#!/usr/bin/env python3
"""Production receive repost and provider CQ copy share one handoff lock."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / 'src/common/rdma.c').read_text()
start = text.index('static int post_recv(')
end = text.index('/* Push one harvested recv completion', start)
code = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#define EFS_RDMA_RECV_ALIGN 64
struct ibv_sge {uint64_t addr;unsigned length,lkey;};
struct ibv_recv_wr {uint64_t wr_id;unsigned num_sge;struct ibv_sge *sg_list;};
struct ibv_wc {int unused;};struct mr {unsigned lkey;};
struct efs_rdma_dev {pthread_mutex_t recv_handoff_mu;void *recv_cq;};
struct efs_rdma_conn {unsigned bufsz,reg_gen;int reg_idx;void *qp;
    unsigned char **recv_bufs;struct mr *mr;struct efs_rdma_dev *dev;};
static struct efs_rdma_dev dev={.recv_handoff_mu=PTHREAD_MUTEX_INITIALIZER};
static int posts,polls,result;
static void locked(void) {
    assert(pthread_mutex_trylock(&dev.recv_handoff_mu)==EBUSY);
}
static void rdma_memcheck_post_recv(void *buf,size_t len) {locked();assert(buf && len==128);}
static int ibv_post_recv(void *qp,struct ibv_recv_wr *wr,struct ibv_recv_wr **bad) {
    (void)qp;(void)bad;locked();assert(wr->num_sge==1 && wr->sg_list->length==128);
    posts++;return result;
}
static int ibv_poll_cq(void *cq,int count,struct ibv_wc *wc) {
    (void)cq;(void)wc;locked();assert(count==32);polls++;return result;
}
''' + text[start:end] + r'''
int main(void) {
    unsigned char bytes[192],*bufs[]={bytes};struct mr mr={.lkey=7};
    struct efs_rdma_conn rc={.bufsz=128,.recv_bufs=bufs,.mr=&mr,.dev=&dev};
    struct ibv_wc wc[32];
    for(int i=0;i<2;i++) {
        result=i ? -1 : 0;
        assert(post_recv(&rc,0)==result);assert(recv_poll(&dev,32,wc)==result);
        assert(!pthread_mutex_trylock(&dev.recv_handoff_mu));pthread_mutex_unlock(&dev.recv_handoff_mu);
    }
    assert(posts==2 && polls==2);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-handoff-') as directory:
    source = Path(directory) / 'handoff.c'
    binary = source.with_suffix('')
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('RDMA handoff: provider CQ copies ordered after buffer repost; error paths unlock PASS')
