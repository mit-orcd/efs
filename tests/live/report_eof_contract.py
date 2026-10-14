#!/usr/bin/env python3
"""Cross-host EOF contract oracle using an EFS_FAULTS private client.

Run on Intel1 (the fixture mount must be local); peer reads run on the peer
host over ssh. Baseline failure is retained. In fixed mode rejected
publication must retain bytes for retry after disabling the fault.

Orchestration (fixture start/stop, builds) is driven from the Mac, whose
intel1_efs/intel2_efs aliases are authoritative. The peer read needs host->
peer ssh: the alias given by --peer must resolve ON the mount host
(provisioned 2026-10-13: intel1 efs key authorized on intel2, both
directions). Example, from the Mac:

  ssh intel1_efs python3 /data1/efs/w100-private/devel-kimi/tests/live/report_eof_contract.py \
      /data1/efs/w100-private/runtime-devel-kimi/mnt \
      /data1/efs/w100-private/runtime-devel-kimi/omit-size --expect refused
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('mount', type=Path)
ap.add_argument('fault', type=Path)
ap.add_argument('--expect', choices=['rounded', 'refused'], required=True)
ap.add_argument('--peer', default='intel2_efs',
                help='ssh alias for the peer host, resolved on the mount host')
args=ap.parse_args()
if not os.path.ismount(args.mount):ap.error('private mounted fixture required')
root=Path(tempfile.mkdtemp(prefix='w100-eof-',dir=args.mount))


def peer(path, size):
    code='''import os,sys,json
p=sys.argv[1];n=int(sys.argv[2]);s=os.stat(p).st_size
fd=os.open(p,os.O_RDONLY)
try:
    high=os.pread(fd,1,8<<20).hex();low=os.pread(fd,1,1<<20).hex()
    past=os.pread(fd,1,n).hex()
finally:os.close(fd)
print(json.dumps(dict(size=s,high=high,low=low,past=past)))
'''
    return json.loads(subprocess.check_output(['ssh',args.peer,shlex.join(['python3','-c',code,str(path),str(size)])],text=True))


rows=[]
try:
    for name,expected in [('small',512),('sparse',(8<<20)+1)]:
        path=root/name
        fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
        try:
            ino=os.fstat(fd).st_ino
            args.fault.write_text(f'OMIT_SIZE {ino}\n')
            if name=='small':assert os.pwrite(fd,b'S'*512,0)==512
            else:
                assert os.pwrite(fd,b'H',8<<20)==1
                assert os.pwrite(fd,b'L',1<<20)==1
            error=None
            try:os.fsync(fd)
            except OSError as e:error=dict(errno=e.errno,error=str(e))
            observed=peer(path,expected)
            row=dict(name=name,inode=ino,expected=expected,error=error,before_retry=observed)
            if args.expect=='rounded':
                assert error is None,row
                assert observed['size']==((expected+131071)//131072)*131072,row
                assert observed['size']!=expected,row
            else:
                assert error is not None,row
                assert observed['size']==0,row
                # A second fsync under the same omission must not acknowledge
                # the stale/absent EOF or release its dirty payload ownership.
                second_error=None
                try:os.fsync(fd)
                except OSError as e:second_error=dict(errno=e.errno,error=str(e))
                second_peer=peer(path,expected)
                row.update(second_error=second_error,second_peer=second_peer)
                assert second_error is not None and second_peer['size']==0,row
            args.fault.write_text('OFF\n')
            if args.expect=='refused':
                os.fsync(fd)
                retry=peer(path,expected);row['after_retry']=retry
                assert retry['size']==expected and retry['past']=='',row
                if name=='sparse':assert retry['high']=='48' and retry['low']=='4c',row
                else:assert subprocess.check_output(['ssh',args.peer,shlex.join(['python3','-c',
                    'import pathlib,sys;assert pathlib.Path(sys.argv[1]).read_bytes()==b"S"*512',str(path)])])==b''
            rows.append(row)
        finally:os.close(fd)
finally:
    args.fault.write_text('OFF\n')
    print(json.dumps(dict(fixture=str(root),expect=args.expect,cases=rows)),flush=True)
print('PASS',args.expect,flush=True)
