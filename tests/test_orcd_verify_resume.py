#!/usr/bin/env python3
"""Resume must skip recorded paths while carrying failed verification forward."""
import hashlib,json,os,pathlib,runpy,socket,sys,tempfile
from unittest.mock import patch
script=pathlib.Path(__file__).parents[1]/'scripts/orcd_verify_existing.py'
real_path=type(pathlib.Path())
with tempfile.TemporaryDirectory() as tmp:
 root=real_path(tmp);run=root/'campaign';old=root/'old';out=root/'new';mount=root/'mount'
 run.mkdir();old.mkdir();host='fcstor003'
 (run/(host+'-scale.json')).write_text(json.dumps(dict(per_worker=[2],errors=[])))
 # Already checked index 0 is intentionally absent. Rereading it would add a failure.
 original=dict(host=host,expected=2,checked=1,bad=1,per_worker=[1],errors=[dict(path='original-failure',error='EBUSY')])
 (old/(host+'.json')).write_text(json.dumps(original))
 p=mount/('scale-'+host)/'w00/d00000/f0001';p.parent.mkdir(parents=True)
 p.write_bytes(hashlib.sha256((host+':0:1').encode()).digest()*8)
 def mapped(value):return mount if str(value)=='/tmp/efs-mount' else real_path(value)
 with patch.object(pathlib,'Path',mapped),patch.object(socket,'gethostname',lambda:host),patch.object(os.path,'ismount',lambda p:True),patch.object(sys,'argv',[str(script),str(run),str(out),'--resume',str(old) ]):
  try:runpy.run_path(str(script),run_name='__main__')
  except SystemExit as e:assert e.code==1,'prior failure must not become PASS'
 result=json.loads((out/(host+'.json')).read_text())
 assert result['complete'] and result['checked']==2 and result['bad']==1
 assert result['prior_checked']==1 and result['errors']==original['errors']
 assert json.loads((old/(host+'.json')).read_text())==original
print('verification resume: unchecked payloads verified; prior failure and evidence preserved PASS')
