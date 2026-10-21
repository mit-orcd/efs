#!/usr/bin/env python3
"""Execute the actual loaded-query convergence loop against staggered snapshots."""
import json,os
from pathlib import Path
from types import SimpleNamespace
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_LOADED_SOURCE',str(root/'tests/live/efs_commands_load.py'))).read_text()
a=s.index('for _ in range(40):',s.index('converge_start='));b=s.index("print(f'loaded publication",a);loop=compile(s[a:b],'<actual loaded query convergence>','exec')
def check(snapshots,success):
 calls=[];sleeps=[]
 def run(*args,**kwargs):
  i=min(len(calls),len(snapshots)-1);calls.append(i)
  return SimpleNamespace(stdout=json.dumps({'stats':snapshots[i]}).encode())
 context=dict(settle=180,expected=3,subprocess=SimpleNamespace(run=run,PIPE=-1),sys=SimpleNamespace(executable='python'),peer=Path('/unused'),fixture='fixture',time=SimpleNamespace(sleep=sleeps.append),json=json)
 try:exec(loop,context)
 except AssertionError:
  assert not success,'oracle rejected a valid later snapshot';assert len(calls)==40 and len(sleeps)==40
 else:
  assert success,'oracle accepted incomplete totals';assert context['parsed']['stats']==dict(tree_files=3,tree_bytes=768)
 return len(calls)
assert check([dict(tree_files=2,tree_bytes=512),dict(tree_files=3,tree_bytes=512),dict(tree_files=3,tree_bytes=768)],True)==3
assert check([dict(tree_files=3,tree_bytes=768)],True)==1
check([dict(tree_files=3,tree_bytes=512)],False)
check([dict(tree_files=2,tree_bytes=768)],False)
print('loaded query oracle: count/bytes converge jointly, incomplete snapshots never pass, original 40-attempt bound retained PASS')
