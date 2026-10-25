#!/usr/bin/env python3
"""15-minute accelerated Task D lifecycle, exclusive in the summary runner.

Not a four-hour endurance claim. Evidence is under this command's TMPDIR.
Uses only its own five-to-seven-member loopback cluster and two mounts.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tests'))
from k33_daemons import fuse_ok
if not fuse_ok():
 print('NOTRUN: soak requires /dev/fuse, fusermount3 and efs-fuse');sys.exit(0)
edelete=Path.home()/'bin/edelete'
if not edelete.exists():
 print('NOTRUN: soak cleanup requires ~/bin/edelete');sys.exit(0)
parent=Path(tempfile.mkdtemp(prefix='efs-soak-',dir=os.environ.get('TMPDIR')))
source=os.environ.get('EFS_GIT_ID') or subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
p=subprocess.run([sys.executable,str(root/'scripts/soak.py'),'--work',str(parent/'work'),'--out',str(parent/'out'),
                  '--source',source,'--seconds','900','--schedule-scale','.0625','--verify-seconds','180'])
print('soak evidence:',parent/'out',flush=True)
if p.returncode==0 and (parent/'work').exists():
 subprocess.run([str(edelete),'--force','--delete',str(parent/'work')],check=True)
sys.exit(0 if p.returncode==77 else p.returncode)
