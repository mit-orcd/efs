#!/usr/bin/env python3
"""Transient admission retains identity, deadline, and hard-error verdicts."""
import ast
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace

source=Path(__file__).resolve().parent/'live/gc_reclamation.py'
tree=ast.parse(source.read_text())
function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='session_admission')
clock=[0.0];seen=[];responses=[]
def execute(args,**kwargs):
 seen.append((args,kwargs['timeout']))
 return responses.pop(0)
def sleep(seconds):clock[0]+=seconds
scope={'time':SimpleNamespace(monotonic=lambda:clock[0],sleep=sleep),'subprocess':SimpleNamespace(run=execute,TimeoutExpired=subprocess.TimeoutExpired,CalledProcessError=subprocess.CalledProcessError),'env':{},'re':re}
exec(compile(ast.Module(body=[function],type_ignores=[]),str(source),'exec'),scope)
call=scope['session_admission'];args=['efs-mgmt','raft-session','host:123','establish','uuid','17','1']
def reply(rc,code):return SimpleNamespace(returncode=rc,stdout='raft-session status=3 rc='+str(code)+' primary=0\n',stderr='')
responses[:]=[reply(1,-13),reply(1,-13),reply(0,0)]
assert call(args,-13,1).returncode==0
assert all(a==args for a,t in seen) and [round(t,2) for a,t in seen]==[1,.95,.9],seen
for code in (-14,-6,-16):
 seen.clear();responses[:]=[reply(1,code)]
 try:call(args,-13,1)
 except subprocess.CalledProcessError as e:assert str(code) in e.stdout
 else:raise AssertionError('hard error accepted')
 assert len(seen)==1
clock[0]=0;seen.clear();responses[:]=[reply(1,-13)]*10
try:call(args,-13,.11)
except subprocess.TimeoutExpired:pass
else:raise AssertionError('BUSY exceeded total deadline')
assert len(seen)==3 and clock[0]==.11,seen
print('session admission: exact BUSY retry, unchanged identity, hard errors and total deadline PASS')
