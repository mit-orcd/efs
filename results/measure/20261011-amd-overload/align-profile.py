#!/usr/bin/env python3
from pathlib import Path
import subprocess
b=Path('/data1/efs/amd-overload-20261011')
p=Path('/data1/efs/devops/clusters/amd.env')
s=p.read_text()
before=b/'amd.env.before.txt'
if not before.exists(): before.write_text(s)
for name,value in [('EFS_DATA_GC_ADMISSION','1'),('EFS_GC_BACKLOG_IDLE_MS','20')]:
 if 'export '+name+'=' not in s:s+='\n# AMD-only validated logical-quota GC admission experiment.\nexport '+name+'=${'+name+':-'+value+'}\n'
subprocess.run(['bash','-n'],input=s,text=True,check=True)
p.write_text(s);(b/'amd.env.after.txt').write_text(s)
