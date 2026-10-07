"""Evictable hints cannot authorize NEW on ambiguous PUT retries."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/client/write.c').read_text()
a = source.index('#define PATH_HINT_N')
b = source.index('static int put_recv_reply', a)
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
''' + source[a:b] + r'''
static void *worker(void *arg) {
    uintptr_t w = (uintptr_t)arg;
    efs_ino_t ino = 1 + w * 4096;
    for (unsigned i=0; i<100000; i++) {
        path_hint_put(1, ino, 0, 0, (uint8_t)w);
        uint32_t hint = path_hint_get(1, ino, 0, 0);
        assert(hint == 0 || hint == w+1);
    }
    return NULL;
}
int main(void) {
    assert(path_hint_index(1,1,0,0)==path_hint_index(1,4097,0,0));
    assert(path_hint_get(1,1,0,0)==0);
    path_hint_put(1,1,0,0,1);
    assert(path_hint_get(1,1,0,0)==2);
    path_hint_put(1,4097,0,0,0);
    assert(path_hint_get(1,1,0,0)==0); /* lost ACK + collision => probe */
    assert(path_hint_get(1,4097,0,0)==1);
    path_hint_put(1,1,0,0,0xff);
    assert(path_hint_get(1,1,0,0)==0); /* unknown reply never populates cache */
    pthread_t threads[4];
    for (uintptr_t w=0; w<4; w++) assert(!pthread_create(&threads[w],NULL,worker,(void *)w));
    for (unsigned w=0; w<4; w++) assert(!pthread_join(threads[w],NULL));
    return 0;
}
'''
# The first-send proof belongs to the lifetime of the newly generated object,
# outside the evictable hint table; all outer retry attempts reuse that object.
assert 'first_send ? EFS_PATH_HINT_NEW :' in source
assert 'failed, object_gen, attempt == 1)' in source
with tempfile.TemporaryDirectory() as d:
    p = Path(d) / 'hint.c'
    p.write_text(code)
    subprocess.run(['cc', '-std=c99', '-pthread', '-I'+str(root/'include'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
print('PUT hints: lost-reply eviction probes and concurrent placement cache PASS')
