#!/usr/bin/env python3
"""Real Raft invariants with and without ACK-window protection, sanitizers and old-window negative."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/raft/raft.c').read_text()
host=(root/'src/server/raft_host.c').read_text()
assert 'cfg.preserve_ack_window = 1;' in host, 'the server host must keep the tested ACK-window protection'
guard="""if (!r->preserve_ack_window || !in->success ||
            in->match_index >= r->ae_inflight[in->from])"""
assert guard in source
with tempfile.TemporaryDirectory(prefix='efs-raft-ack-window-') as directory:
 p=Path(directory);binary=p/'positive'
 flags=shlex.split(os.environ.get('EFS_TEST_CFLAGS','-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'))
 command=[os.environ.get('CC','cc'),'-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-pthread','-I'+str(root/'include')]
 dependencies=[str(root/'tests/test_raft.c'),str(root/'src/raft/raft_mem.c'),str(root/'src/common/batch_obs.c')]
 subprocess.run([*command,*flags,*dependencies,str(root/'src/raft/raft.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
 old=p/'old-retirement.c';old.write_text(source.replace(guard,'if (1)',1))
 binary=p/'old-negative'
 subprocess.run([*command,'-O1','-g',*dependencies,str(old),'-o',str(binary)],check=True)
 result=subprocess.run([str(binary)],capture_output=True,text=True)
 assert result.returncode!=0 and 'duplicate preserves newer window when enabled' in result.stderr,'old-retirement negative did not expose the intended defect'
 print('raft ACK window: real dual-policy invariants/sanitizers PASS; original-retirement negative refused')
