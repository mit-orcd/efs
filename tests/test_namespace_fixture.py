#!/usr/bin/env python3
"""Salted production placement must build the exact live namespace envelope."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
helper=Path(os.environ.get('EFS_NAMESPACE_FIXTURE_SOURCE',root/'tests/live/namespace_attrs.c'))
with tempfile.TemporaryDirectory(prefix='efs-namespace-fixture-') as tmp:
    binary=Path(tmp)/'names'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(helper),str(root/'libefs.a'),'-pthread','-lm','-ldl','-libverbs','-o',str(binary)],check=True)
    for salt in (0,1,0xffffffffffffffff,0x123456789abcdef0,0xa55aa55a):
        used={1};parent=1
        for index in range(145):
            fresh=index<143
            result=subprocess.run([str(binary),'choose',str(parent),str(salt),f'max-{index}','fresh' if fresh else 'member'],input=' '.join(map(str,sorted(used))),capture_output=True,text=True,check=True,timeout=5)
            fields=result.stdout.split();assert len(fields)==2,result.stdout
            shard,name=int(fields[0]),fields[1]
            assert (shard not in used)==fresh,(salt,index,shard)
            used.add(shard);parent=((index+2)<<12)|shard
            if index>=142:assert len(used)==144,(salt,index,used)
        assert len(used)==144
    for mode,used in (('fresh',range(4096)),('member',())):
        result=subprocess.run([str(binary),'choose','1','0','none',mode],input=' '.join(map(str,used)),capture_output=True,text=True,timeout=5)
        assert result.returncode==1 and not result.stdout
print('namespace fixture: five salts, exact144 distinct participants, shared-shard depth and bounded impossible placement PASS')
