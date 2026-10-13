#!/usr/bin/env python3
"""Real deferred-flush regression and old-inline negative, plus pump ownership."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/kv/kv_compact.c').read_text()
needle='if(l->defer_held_flush && l->flush_holds)return EFS_OK;'
assert source.count(needle)==1
with tempfile.TemporaryDirectory(prefix='efs-flush-tail-') as work:
 p=Path(work)/'compact.c';p.write_text(source.replace(needle,'if(0)return EFS_OK;'))
 # The replacement object owns all compactor symbols. The archive supplies
 # the remaining actual KV/WAL/segment code without its original compactor.
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/kv'),'-c',str(p),'-o',str(p.with_suffix('.o'))],check=True)
 binary=Path(work)/'negative'
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(root/'tests/test_kv_lsm.c'),str(p.with_suffix('.o')),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(binary)],check=True)
 env=dict(os.environ,EFS_TEST_KV_DEFER_ONLY='1')
 result=subprocess.run([str(binary)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
 assert result.returncode!=0 and 'no inline automatic flush during hold' in result.stdout,result.stdout
# Instrument every actual KV module and the real regression harness.
with tempfile.TemporaryDirectory(prefix='efs-flush-tail-asan-') as work:
 flags=['-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-D_GNU_SOURCE',
        '-I'+str(root/'include'),'-I'+str(root/'src/common'),'-I'+str(root/'deps/blake3')]
 objects=[]
 for source in sorted((root/'src/kv').glob('*.c')):
  obj=Path(work)/(source.stem+'.o');objects.append(str(obj))
  subprocess.run(['cc',*flags,'-c',str(source),'-o',str(obj)],check=True)
 binary=Path(work)/'sanitized'
 subprocess.run(['cc',*flags,str(root/'tests/test_kv_lsm.c'),*objects,str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(binary)],check=True)
 subprocess.run([str(binary)],env=dict(os.environ,EFS_TEST_KV_DEFER_ONLY='1'),check=True,timeout=60)
host=(root/'src/server/raft_host.c').read_text()
a=host.index('static void *host_pump(');b=host.index('int server_raft_host_',a)
pump=host[a:b]
assert pump.index('pthread_mutex_unlock(&h->mu);')<pump.index('efs_kv_lsm_sync_release(h->kv)')<pump.index('host_checkpoint_dispatch(')
assert 'if(s->storage_engine)\n            (void)efs_kv_lsm_defer_held_flush(h->kv,1);' in host
print('container flush tail: actual KV ASan/UBSan and old-inline negative, container startup scope and off-mutex WAL-before-checkpoint ordering PASS')
