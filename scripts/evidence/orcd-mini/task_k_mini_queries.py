#!/usr/bin/env python3
"""Task K real orcd-mini: stable .efs after advertised catalog readiness.

Separate from the cold-directory POSIX extras measurement: those original
cases and budgets stay unchanged, and their SKIPs remain INCOMPLETE. Uses the
existing live-test warmup on every serving catalog, then checks each command
once and requires exact quiescent payloads/metadata. No product setting changes.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--mount',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
root=a.source
sys.path.insert(0,str(root/'tests/live'))
from query_timing import refresh_seconds,settle_seconds,wait_query_nodes_indexed
assert os.path.ismount(a.mount),a.mount
version=subprocess.check_output([str(root/'efsd'),'--version'],text=True)
assert '00e85a299531' in version,version
a.out.mkdir(parents=True,exist_ok=False)
fixture=a.mount/'taskK-mini-catalog'
fixture.mkdir()
expected={'report.txt':bytes(range(256))*16,
          'second.bin':bytes(range(256))*4}
try:
    for name,body in expected.items():
        with (fixture/name).open('wb') as stream:
            stream.write(body);stream.flush();os.fsync(stream.fileno())
    interval=refresh_seconds(a.mount)
    os.environ['EFS_QUERY_TEST_NODES']=json.dumps({str(i):['10.1.99.1',17431+i] for i in (1,2,3,4)})
    begin=time.monotonic()
    serving=wait_query_nodes_indexed(fixture.stat().st_ino,settle_seconds(interval))
    (a.out/'readiness.json').write_text(json.dumps(dict(interval_s=interval,
        deadline_s=settle_seconds(interval),elapsed_s=time.monotonic()-begin,serving=serving),indent=2)+'\n')
    for command in ('stats','ls','find'):
        args=[sys.executable,str(fixture/'.efs'/command)]
        if command=='find':args+=['report']
        args+=['--json']
        result=subprocess.run(args,env=os.environ,capture_output=True,text=True,timeout=30)
        (a.out/(command+'.stdout')).write_text(result.stdout)
        (a.out/(command+'.stderr')).write_text(result.stderr)
        (a.out/(command+'-exit')).write_text(str(result.returncode)+'\n')
        assert result.returncode==0,(command,result.returncode,result.stderr)
        value=json.loads(result.stdout);assert value['complete'] is True,value
        if command=='stats':
            assert value['stats']['tree_files']==2 and value['stats']['tree_bytes']==5120,value
        else:
            names={bytes.fromhex(e['path_hex']).split(b'/')[-1]:e['size'] for e in value['entries']}
            want={name.encode():len(body) for name,body in expected.items() if command=='ls' or name=='report.txt'}
            assert names==want,(command,names,want)
    for name,body in expected.items():assert (fixture/name).read_bytes()==body,name
    (a.out/'result.json').write_text(json.dumps(dict(verdict='PASS',commands=3,
        exact_payload_files=2,exact_payload_bytes=5120))+'\n')

except BaseException as exc:
    (a.out/'failure.txt').write_text(repr(exc)+'\n')
    raise
finally:
    (a.out/'identity.txt').write_text(version)
