#!/usr/bin/env python3
from resync_setup import request_before
now=[0.0]
def clock():return now[0]
def sleep(dt):now[0]+=dt
answers=iter([(1,'raft-resync x group=0 rc=-13'),(1,'raft-resync x group=0 rc=-13'),(0,'ok')])
assert request_before(lambda:next(answers),1,clock=clock,sleep=sleep)==3
for rc,out in [(1,'rc=-6'),(1,'rc=-15'),(2,'rc=-13'),(1,'rc=-130')]:
 try:request_before(lambda:(rc,out),1,clock=clock,sleep=sleep)
 except AssertionError:pass
 else:raise AssertionError('hard failure retried')
now[0]=0
try:request_before(lambda:(1,'rc=-13'),0.25,clock=clock,sleep=sleep)
except AssertionError as e:assert 'through deadline' in str(e) and now[0]==0.25
else:raise AssertionError('permanent BUSY passed')
print('resync setup: receive-persistence BUSY only, hard errors fail, shared original deadline PASS')
