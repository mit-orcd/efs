#!/usr/bin/env python3
"""Compare private frozen namespaces; arguments: remote working root, KV clone."""
import json,os,subprocess,sys,time
from pathlib import Path
root=Path(sys.argv[1]);kv=sys.argv[2]
for trial,variant in enumerate((0,1,2,3,3,2,1,0,0,1,2,3)):
    output=root/f'w92-incremental-repeat-{trial}-r{variant}.jsonl'
    with output.open('w') as stream:
        start=time.monotonic()
        proc=subprocess.Popen([str(root/f'w92-incremental-frozen-r{variant}'),kv,str(root/f'w92-incremental-repeat-{trial}-cache')],stdout=stream)
        _,status,usage=os.wait4(proc.pid,0)
        proc.returncode=os.waitstatus_to_exitcode(status)
    assert proc.returncode==0,(trial,proc.returncode)
    rows=[json.loads(line) for line in output.read_text().splitlines()]
    assert len(rows)==4 and all(row['rc']==0 and (row['files'],row['dirs'],row['logical_bytes'])==(25425,827,6234928) for row in rows),rows
    print(json.dumps({'trial':trial,'variant':variant,'wall_seconds':time.monotonic()-start,'cpu_user_seconds':usage.ru_utime,'cpu_system_seconds':usage.ru_stime,'process_peak_rss_kib':usage.ru_maxrss,'builds':rows}),flush=True)
