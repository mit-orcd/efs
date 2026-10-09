#!/usr/bin/env python3
"""Compile the real host admission policy against controlled observations."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/server/raft_host.c').read_text()
start=source.index('static uint64_t host_pressure_add(')
end=source.index('/* Propose one command.',start)
policy=source[start:end]
limits='\n'.join(re.findall(r'^#define HOST_PRESSURE_.*$',source,re.M))
code=r'''
#include "efs/raft.h"
#include "efs/kv_lsm.h"
#include "efs/meta_cmd.h"
#include "efs/meta_apply.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#define HOST_NGROUPS 2
struct efs_raft_host;
struct host_group { struct efs_raft_host *host; struct efs_raft *r; };
struct efs_raft_host { int pressure_enabled; struct efs_kv *kv;
 struct host_group g[2]; uint64_t pressure_rejected,pressure_recv_rejected; };
static struct efs_kv_lsm_pressure observation;
static uint64_t pending_n,pending_bytes,log_bytes,forced_work;
int efs_kv_lsm_pressure(struct efs_kv *kv,struct efs_kv_lsm_pressure *p)
{(void)kv;*p=observation;return EFS_OK;}
int efs_raft_backlog(struct efs_raft *r,uint64_t *n,uint64_t *b,uint64_t *l)
{(void)r;*n=pending_n;*b=pending_bytes;*l=log_bytes;return EFS_OK;}
'''+limits+'\n'+policy+r'''
int efs_raft_backlog_charge(struct efs_raft *r,efs_raft_charge_fn fn,uint64_t limit,uint64_t *out)
{(void)r;(void)fn;*out=forced_work ? forced_work : host_pressure_charge(pending_bytes,pending_n);
 if(*out>limit)*out=limit;
 return EFS_OK;}

int main(void) {
 struct efs_raft_host h={.pressure_enabled=1};
 h.g[0].host=&h;h.g[0].r=(struct efs_raft*)&h;
 uint8_t ordinary=0xfe,recovery=EFS_MD_CMD_RESOLVE;
 struct efs_raft_entry e={.cmd=&ordinary,.clen=1};
 observation.l0_soft_bytes=768ull<<20;observation.l0_hard_bytes=1ull<<30;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 observation.shape.l0_bytes=768ull<<20;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 assert(host_pressure_admit(&h.g[0],NULL,0,EFS_RAFT_ADMIT_LEAD)==EFS_ERR_BUSY);
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_RECV)==EFS_OK);
 observation.shape.l0_bytes=1ull<<30;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_RECV)==EFS_ERR_BUSY);
 e.cmd=&recovery;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 observation.shape.mt_bytes=64ull<<20;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 observation.shape.mt_bytes=0;observation.shape.l0_bytes=0;
 pending_n=HOST_PRESSURE_COUNT-HOST_PRESSURE_RECOVERY_COUNT;
 e.cmd=&ordinary;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 e.cmd=&recovery;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 pending_n=HOST_PRESSURE_COUNT;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 assert(pending_n==HOST_PRESSURE_COUNT); /* refusal cannot free owned work */
 pending_n=0;log_bytes=HOST_PRESSURE_LOG_BYTES-HOST_PRESSURE_LOG_RESERVE;
 e.cmd=&ordinary;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 e.cmd=&recovery;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 log_bytes=HOST_PRESSURE_LOG_BYTES;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 log_bytes=0;pending_bytes=HOST_PRESSURE_PENDING_BYTES/16;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 /* Values that wrapped the former multiplication to zero must refuse. */
 pending_bytes=UINT64_MAX/16+1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 pending_bytes=0;pending_n=UINT64_MAX/4096+1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 /* Aggregate two group observations without wrapping count/bytes/logs. */
 h.g[1].host=&h;h.g[1].r=(struct efs_raft*)&h;
 pending_n=0;pending_bytes=UINT64_MAX/2+1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_RECV)==EFS_ERR_BUSY);
 pending_bytes=0;log_bytes=UINT64_MAX/2+1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_RECV)==EFS_ERR_BUSY);
 log_bytes=0;pending_n=UINT64_MAX/2+1;
 assert(host_pressure_admit(&h.g[0],NULL,0,EFS_RAFT_ADMIT_LEAD)==EFS_ERR_BUSY);
 pending_n=0;
 uint8_t sweep=EFS_MD_CMD_LANE_SWEEP_BATCH,legacy=EFS_MD_CMD_LANE_SWEEP;
 uint8_t tags[]={EFS_MD_CMD_TRUNCATE,EFS_MD_CMD_LANE_FENCE,EFS_MD_CMD_RESOLVE,EFS_MD_CMD_DROP};
 uint32_t limits[]={EFS_META_TRUNCATE_ITEMS,EFS_META_LANE_FENCE_ITEMS,EFS_TXN_RESOLVE_ITEMS,EFS_TXN_DROP_ITEMS};
 uint32_t keys[]={2,2,1,1},values[]={EFS_META_GC_VAL,EFS_META_GC_VAL,EFS_TXN_RECORD_MAX,0};
 h.g[1].r=NULL;
 for (unsigned i=0;i<4;i++) {
   uint64_t work=host_pressure_rows_charge(limits[i],keys[i],values[i]);
   assert(host_pressure_command_charge(&tags[i],22)==work);
   e.cmd=&tags[i];e.clen=22;
   uint64_t cap=HOST_PRESSURE_PENDING_BYTES-(host_pressure_recovery(e.cmd,e.clen)?0:HOST_PRESSURE_PENDING_RESERVE);
   forced_work=cap-work;
   assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
   forced_work++;
   assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 }
 forced_work=0;e.clen=1;
 assert(host_pressure_command_charge(&sweep,51)==(64u<<10));
 h.g[1].r=NULL;
 forced_work=HOST_PRESSURE_PENDING_BYTES-(64u<<10);
 e.cmd=&sweep;e.clen=1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 forced_work++;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 forced_work=UINT64_MAX;
 e.cmd=&recovery;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 forced_work=0;
 assert(host_pressure_command_charge(&legacy,18)==UINT64_MAX);
 legacy=EFS_MD_CMD_REAP_DONE;
 assert(host_pressure_command_charge(&legacy,17)==UINT64_MAX);
 e.cmd=&legacy;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_BUSY);
 e.cmd=&recovery;
 assert(host_pressure_add(UINT64_MAX,1)==UINT64_MAX);
 assert(host_pressure_charge(UINT64_MAX,UINT64_MAX)==UINT64_MAX);
 assert(host_pressure_charge(2,3)==32+12288);
 assert(host_pressure_charge(UINT64_MAX/16,0)==UINT64_MAX-15);
 assert(host_pressure_charge(UINT64_MAX/16,1)==UINT64_MAX);
 assert(host_pressure_charge(0,UINT64_MAX/4096)==UINT64_MAX-4095);
 assert(host_pressure_charge(256,UINT64_MAX/4096)==UINT64_MAX);
 pending_bytes=0;observation.io_failed=1;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_ERR_IO);
 assert(host_pressure_admit(&h.g[0],NULL,0,EFS_RAFT_ADMIT_LEAD)==EFS_ERR_IO);
 assert(!host_pressure_recovery(&recovery,(256u<<10)+1));
 h.pressure_enabled=0;
 assert(host_pressure_admit(&h.g[0],&e,1,EFS_RAFT_ADMIT_LOCAL)==EFS_OK);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pressure-policy-') as directory:
    c=Path(directory)/'test.c';binary=Path(directory)/'test';c.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('host pressure: soft/hard, receive pacing, leader eligibility, recovery reserves, retained ownership and persistence poison PASS')
