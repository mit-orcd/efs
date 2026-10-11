#!/usr/bin/env python3
"""Partition refusal retains exact epoch, FENCING, and failed completion."""
import ast,re
from pathlib import Path
p=Path(__file__).resolve().parent/'live/raft_read_freshness.py'
tree=ast.parse(p.read_text());fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='incomplete_storage_fence')
scope={'re':re};exec(compile(ast.Module(body=[fn],type_ignores=[]),str(p),'exec'),scope);check=scope['incomplete_storage_fence']
for rc in (-1,-13):
 assert check(f'raft-session status=3 rc={rc} primary=3 op=fence epoch=2 state=2 touched=0 index=0\n')
for status,rc,epoch,state in ((0,0,2,1),(3,-14,2,2),(3,-1,1,2),(3,-13,2,1),(3,-13,2,3),(0,-13,2,2),(3,-1,0,0)):
 assert not check(f'raft-session status={status} rc={rc} primary=3 op=fence epoch={epoch} state={state} touched=0 index=0\n')
assert not check('connect failed: rc=-1')
fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='active_session')
exec(compile(ast.Module(body=[fn],type_ignores=[]),str(p),'exec'),scope)
for status,epoch,state in ((0,1,1),(3,1,1),(0,2,1),(0,1,2)):
 assert scope['active_session'](f'raft-session status={status} rc=0 epoch={epoch} state={state} touched=0') == (status==0 and epoch==1 and state==1)
print('partition fence: BUSY or hard IO refusal, exact epoch/FENCING, successful/stale/partial verdict rejection PASS')
