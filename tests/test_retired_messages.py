#!/usr/bin/env python3
"""Retired message IDs have no handler and are refused before dispatch."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
proto = (root/'include/efs/protocol.h').read_text()
handler = Path(os.environ.get('EFS_HANDLER_SOURCE', root/'src/server/handler.c')).read_text()
macro = re.search(r'#define EFS_MSG_IS_RETIRED\(t\) \\\n((?:.*\\\n)*.*\n)', proto).group(1)
retired = sorted(map(int, re.findall(r'== (\d+)', macro)))
assert retired == [99, 100, 121, 122], retired
enum = proto[proto.index('enum efs_msg_type'):]
enum = enum[:enum.index('};')]
live = {int(v): n for n, v in re.findall(r'^\s*(EFS_MSG_\w+)\s*=\s*(\d+)', enum, re.M)}
for t in retired:
    assert t not in live, ('retired message ID reused', t, live[t])
# The refusal runs after the frame is read and before the switch; nothing
# below it may dispatch a retired type.
loop = handler[handler.index('void server_handle_conn('):]
check = loop.index('if (EFS_MSG_IS_RETIRED(type)) {')
assert check < loop.index('switch (type) {'), 'refusal must precede dispatch'
block = loop[check:loop.index('}', check)]
assert 'close_conn = retired_refuse(type);' in block and 'type = 0;' in block, block
a = handler.index('static int retired_refuse(uint8_t type)')
helper = handler[a:handler.index('\n}', a)+2]
source = '#include "efs/protocol.h"\n#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n' + helper + r'''
int main(void){
 for(int t=0;t<256;t++)if(EFS_MSG_IS_RETIRED(t))assert(retired_refuse((uint8_t)t)==1);
 assert(!EFS_MSG_IS_RETIRED(EFS_MSG_GC_INODE_BATCH)&&!EFS_MSG_IS_RETIRED(EFS_MSG_GC_FRAG_BATCH));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-retired-msg-') as d:
    path = Path(d)/'test.c'; path.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc'))+['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I'+str(root/'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, stderr=subprocess.DEVNULL)
print('retired messages: IDs %s unnamed, refused before dispatch with a closed connection PASS' % retired)
