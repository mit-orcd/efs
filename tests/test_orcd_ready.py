#!/usr/bin/env python3
"""Readiness must prove usable recovered leaders, not merely listening sockets."""
import copy
import importlib.util
from unittest.mock import patch
from types import SimpleNamespace
from pathlib import Path
spec=importlib.util.spec_from_file_location('ready',Path(__file__).parents[1]/'scripts/orcd_test_ready.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
states=[]
for node in range(1,5):
 groups={}
 for group,voters,leader in [(0,7,1),(2,14,2)]:
  hosted=bool(voters & (1 << (node-1)))
  groups[group]=dict(hosted=hosted,role='LEADER' if node-1==leader else 'FOLLOWER',
   leader=leader,term=5,commit=10,applied=10,voters=voters,joint=0)
 states.append(dict(node=node,root=int(node!=4),salt=123,groups=groups))
assert set(m.ready(states))=={0,2}
def refuses(change):
 case=copy.deepcopy(states);change(case)
 try:m.ready(case)
 except AssertionError:return
 raise AssertionError('premature readiness accepted')
refuses(lambda s:s[1]['groups'][0].update(role='FOLLOWER'))
refuses(lambda s:s[1]['groups'][0].update(applied=9))
refuses(lambda s:s[0]['groups'][0].update(term=4))
refuses(lambda s:s[1].update(root=0))
refuses(lambda s:s[2].update(salt=124))
refuses(lambda s:s[2]['groups'][2].update(joint=1))
refuses(lambda s:s[0]['groups'][0].update(hosted=0))
# A stale retired copy can remain hosted after the 3-to-4 placement change.
retained=copy.deepcopy(states)
retained[0]['groups'][2].update(hosted=1,retired=True,leader=-1,term=1,voters=7)
assert set(m.ready(retained))=={0,2}
# Retired cannot excuse a missing current voter or count as its leader.
refuses(lambda s:s[1]['groups'][0].update(retired=True))
text='node 1 root=1 salt=123 groups=2\n'+\
 'group 0 hosted=1 role=FOLLOWER leader=1 term=5 commit=10 applied=10 voters=0x7 joint=0 retired=0\n'+\
 'group 2 hosted=1 role=FOLLOWER leader=-1 term=1 commit=2 applied=2 voters=0x7 joint=0 retired=1\n'
with patch.object(m.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=text)):
 parsed=m.inspect('unused','unused')
assert not parsed['groups'][0]['retired'] and parsed['groups'][2]['retired']
print('ORCD readiness rejects unavailable/recovering leaders, divergent terms/exports and missing voters PASS')
