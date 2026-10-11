#!/usr/bin/env python3
"""Align this exclusive AMD deployment's restart defaults; retain overrides."""
from pathlib import Path
import subprocess
out=Path('/data1/efs/amd-gc-yield20-20261011')
p=Path('/data1/efs/devops/clusters/amd.env')
s=p.read_text()
b=out/'amd.env.before.txt'
if not b.exists(): b.write_text(s)
s=s.replace('# AMD acceptance: strict Soft-RoCE and direct storage I/O.', '# AMD non-RDMA acceptance: TCP and direct storage I/O.')
s=s.replace('EFS_TRANSPORT=${EFS_TRANSPORT:-rdma}', 'EFS_TRANSPORT=${EFS_TRANSPORT:-tcp}')
s=s.replace('# per node; 4x37.5G raw, 100G usable (2+1)', '# per-node logical quota; usable capacity depends on the active layout')
if 'export EFS_COMMANDS=' not in s:
 s+='\n# Keep command features disabled for this acceptance deployment.\nexport EFS_COMMANDS=${EFS_COMMANDS:-0}\nexport EFS_COMMANDS_SERVER=${EFS_COMMANDS_SERVER:-0}\n'
subprocess.run(['bash','-n'],input=s,text=True,check=True)
p.write_text(s)
(out/'amd.env.after.txt').write_text(s)
