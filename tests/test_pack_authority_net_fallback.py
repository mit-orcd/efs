from pathlib import Path
import subprocess, os, shlex, sys, tempfile
r=Path(__file__).resolve().parents[1];t=(r/'tests/test_packed_upload.c').read_text();u=(r/'src/server/packed_upload.c').read_text()
a=u.index('int server_packed_upload(');fn=u[a:u.index('\n}',a)+2]
a=t.index('static struct efsd_server server;');b=t.index('static int cleanup(');stubs=t[a:b].replace('out->rc=authority_rc;', 'out->rc=cohort_mode==3?EFS_ERR_NET:authority_rc;',1)
a=t.index('int main(void)');b=t.index('    for (int direct =',a);main=t[a:b]
code='#include "server_internal.h"\n#include "efs/packed_upload.h"\n#include "efs/checksum.h"\n#include <stdlib.h>\n#include <string.h>\n#include <assert.h>\n#include <stdio.h>\n'+stubs+'''
static int writes,guard_rc;
int server_put_session_begin(struct efsd_server *s,const uint8_t uuid[16],uint32_t ep,struct efs_put_storage_guard *g){(void)s;(void)uuid;(void)ep;g->locked=(void*)1;return guard_rc;}
void server_put_session_end(struct efs_put_storage_guard *g){assert(g->locked);}
int server_put_session_fence(struct efsd_server *s,const uint8_t u[16],uint32_t ep){(void)s;(void)u;(void)ep;return 0;}
int server_write_packed_fragment(struct efsd_server *s,efs_export_id_t e,uint64_t id,uint64_t gen,uint32_t fi,const uint8_t *p,uint32_t n,const uint8_t sum[32]){(void)s;(void)e;(void)id;(void)gen;(void)fi;(void)p;(void)n;(void)sum;writes++;return 0;}
'''+fn+'\n'+main+'''
 server.id=1;cohort_mode=3;
 int rc=server_packed_upload(&server,frame,bytes);
 assert(!rc&&legacy_calls==1&&view_calls==2&&writes==1);
 legacy_calls=view_calls=0;authority_rc=EFS_ERR_STALE;
 assert(server_packed_upload(&server,frame,bytes)==EFS_ERR_STALE&&legacy_calls==1&&!view_calls&&writes==1);
 legacy_calls=view_calls=0;authority_rc=0;epoch=1;
 assert(server_packed_upload(&server,frame,bytes)==EFS_ERR_STALE&&legacy_calls==1&&view_calls==1&&writes==1);
 legacy_calls=view_calls=0;epoch=0;guard_rc=EFS_ERR_STALE;
 assert(server_packed_upload(&server,frame,bytes)==EFS_ERR_STALE&&legacy_calls==1&&view_calls==2&&writes==1);
 legacy_calls=view_calls=0;guard_rc=0;cohort_mode=1;authority_rc=EFS_ERR_BUSY;
 assert(server_packed_upload(&server,frame,bytes)==EFS_ERR_BUSY&&!legacy_calls&&!view_calls&&writes==1);
 puts("FIX PASS: read-only NET uses fresh legacy admission and FileID checks; stale admission/epoch, local guard, and BUSY refuse writes");
 free(frame);free(fragments);
}
'''
tmp=tempfile.TemporaryDirectory(prefix='efs-authority-caller-');work=Path(tmp.name);source=work/'caller.c';exe=work/'caller';source.write_text(code)
files=['src/common/packed_segment.c','src/common/pack_ledger.c','src/common/checksum.c','src/common/common.c','src/common/publication.c','src/data/erasure.c','src/kv/kv_key.c','src/meta/pack_admission.c','src/meta/pack_mapping.c','src/sim/opid.c','deps/blake3/blake3.c','deps/blake3/blake3_dispatch.c','deps/blake3/blake3_portable.c']
cmd=['cc','-std=gnu11','-ffunction-sections','-fdata-sections',*(['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']),*shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS','')),'-I'+str(r/'include'),'-I'+str(r/'src/server'),'-I'+str(r/'deps/blake3'),'-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0',str(source)]+[str(r/f) for f in files]+['-o',str(exe)]
subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
