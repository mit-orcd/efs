#!/usr/bin/env python3
"""Execute the retained owner through ambiguity, membership and budget faults."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "client_internal.h"
#include "efs/put_ticket.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static int allocation_fail,probe_rc=EFS_ERR_NOT_FOUND,send_rc,session_rc;
static uint32_t observed=EFS_PUT_TICKET_ADMITTED;
static unsigned allocations,frees,probes,sends,sessions;
static struct efs_put_ticket expected;
static int retirement_rc=EFS_ERR_NET;static unsigned retirements;
int efs_client_rpc_put_ticket_retire(const struct efs_put_ticket *t){assert(!memcmp(t,&expected,sizeof(*t)));retirements++;return retirement_rc;}
void *efs_buf_metadata_alloc(size_t n){allocations++;return allocation_fail?NULL:calloc(1,n);}
void efs_buf_metadata_free(void *p,size_t n){(void)n;frees++;free(p);}
int efs_client_publication_id(uint64_t seq,struct efs_opid *out){memset(out,0,sizeof(*out));out->client_uuid[0]=1;out->session_epoch=1;out->seq=seq;return EFS_OK;}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id){assert(shard==efs_kv_lane_shard(expected.ino,expected.ci%EFS_META_LANES));assert(!memcmp(id,&expected.id,sizeof(*id)));sessions++;return session_rc;}
int efs_client_rpc_put_ticket(const struct efs_put_ticket *t,int probe,uint32_t *state){assert(!memcmp(t,&expected,sizeof(*t)));*state=0;if(probe){probes++;if(probe_rc)return probe_rc;}else{sends++;if(send_rc)return send_rc;}*state=observed;return EFS_OK;}
#include "put_ticket_owner.c"
static struct efs_meta_pub intent(void){struct efs_meta_pub p={.ino=4097,.inode_gen=7,.fresh_object=1,.put_fragment_len=32,.put_member_mask=15,.coding_profile_id=EFS_META_PROFILE_K2F1};assert(!efs_client_publication_id(9,&p.put_id));p.publication_id=p.put_id;p.candidate_gen=efs_meta_candidate_gen(p.put_id.client_uuid,1,9,0,0);for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;memset(p.ch.checksums[i],i+1,EFS_HASH_SIZE);}return p;}
int main(void){
 g_client.node_count=4;for(unsigned i=0;i<4;i++)g_client.nodes[i].id=i+1;
 struct efs_meta_pub p=intent();assert(!efs_put_ticket_from_pub(&p,&expected));
 struct efs_client_put_request *r=NULL;uint32_t state=99;
 allocation_fail=1;assert(efs_client_put_begin(&r,&p)==EFS_ERR_NOMEM && !r && !probes && !sends);allocation_fail=0;
 struct efs_meta_pub bad=p;bad.put_member_mask=7;assert(efs_client_put_begin(&r,&bad)==EFS_ERR_INVAL && !r);
 bad=p;bad.put_id.client_uuid[0]=2;assert(efs_client_put_begin(&r,&bad)==EFS_ERR_STALE && !r);
 bad=p;bad.ch.nodes[2]=8;assert(efs_client_put_begin(&r,&bad)==EFS_ERR_INVAL && !r);
 g_client.nodes[3].id=1;assert(efs_client_put_begin(&r,&p)==EFS_ERR_INVAL && !r);g_client.nodes[3].id=4;
 assert(!efs_client_put_begin(&r,&p));assert(efs_client_put_begin(&r,&p)==EFS_ERR_INVAL);
 assert(efs_client_put_resume(r,1,&state)==EFS_ERR_NOT_FOUND && !state && !sessions && !sends);
 assert(!efs_client_put_free(r));r=NULL;
 assert(!efs_client_put_begin(&r,&p));
 session_rc=EFS_ERR_NET;assert(efs_client_put_resume(r,0,&state)==EFS_ERR_NET && !state && !sends);session_rc=0;
 send_rc=EFS_ERR_NET;assert(efs_client_put_resume(r,0,&state)==EFS_ERR_NET && !state && sends==1);
 assert(efs_client_put_free(r)==EFS_ERR_BUSY);assert(efs_client_put_retire(r)==EFS_ERR_BUSY && !retirements);
 p.ch.checksums[0][0]^=1;struct efs_put_ticket copy;
 assert(!efs_client_put_manifest(r,&copy) && !memcmp(&copy,&expected,sizeof(copy)));copy.body_digest[0]^=1;
 /* Warm establishment does not grant permission after server revocation. */
 assert(sessions==2);send_rc=EFS_ERR_STALE;
 assert(efs_client_put_resume(r,0,&state)==EFS_ERR_STALE && !state && sends==2 && sessions==2);
 assert(efs_client_put_free(r)==EFS_ERR_BUSY);
 send_rc=0;probe_rc=0;assert(!efs_client_put_resume(r,0,&state) && state==1 && sends==2);
 /* Never cache ADMITTED as continuing permission. */
 probe_rc=EFS_ERR_STALE;assert(efs_client_put_resume(r,0,&state)==EFS_ERR_STALE && !state && sends==2);
 probe_rc=0;unsigned before=probes;g_client.nodes[0].id=4;g_client.nodes[3].id=1;
 assert(efs_client_put_resume(r,0,&state)==EFS_ERR_STALE && !state && probes==before);
 g_client.nodes[0].id=1;g_client.nodes[3].id=4;g_client.node_count=3;
 assert(efs_client_put_resume(r,1,&state)==EFS_ERR_STALE && !state && probes==before);g_client.node_count=4;
 observed=3;assert(!efs_client_put_resume(r,1,&state) && state==3);assert(efs_client_put_free(r)==EFS_ERR_BUSY);
 observed=2;assert(efs_client_put_resume(r,0,&state)==EFS_ERR_STALE && state==2 && sends==2);
 assert(efs_client_put_free(r)==EFS_ERR_BUSY);
 assert(efs_client_put_retire(r)==EFS_ERR_NET && retirements==1);
 assert(efs_client_put_free(r)==EFS_ERR_BUSY);
 retirement_rc=0;assert(!efs_client_put_retire(r) && retirements==2);
 assert(!efs_client_put_retire(r) && retirements==2);
 assert(!efs_client_put_free(r));r=NULL;
 p=intent();assert(!efs_client_put_begin(&r,&p));observed=0;
 assert(efs_client_put_resume(r,1,&state)==EFS_ERR_PROTO && !state);
 /* A pure malformed probe did not submit this newly created handle. */
 assert(!efs_client_put_free(r));assert(frees==3);
 puts("retained PUT owner: budget-before-send, immutable manifest, lost admission replies, fresh probes, exact membership and revocation retention PASS");
}
'''
inode_source=(root/'src/client/inode_rpc.c').read_text()
a=inode_source.index('int efs_client_session_admit(');b=inode_source.index('\n}',a)+2
code=code.replace('#include "put_ticket_owner.c"',inode_source[a:b]+'\n#include "put_ticket_owner.c"')
with tempfile.TemporaryDirectory(prefix='efs-put-owner-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-pthread','-lm','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
