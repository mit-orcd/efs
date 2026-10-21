#!/usr/bin/env python3
"""A successful helper/node must not hide another serving node's stale catalog."""
import os,pathlib,sys
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(pathlib.Path(__file__).parent/'live'))
import query_timing,query_rpc
calls=[]
def send(port,q,**kw):
    calls.append(port)
    round_no=(len(calls)-1)//3
    status=-15 if port==1 else -21 if port==3 and round_no==0 else 0
    return SimpleNamespace(status=status,primary_id=2,boot_id=port,generation=round_no+1), b'{}' if status==0 else b''
with patch.dict(os.environ,{'EFS_QUERY_TEST_NODES':'{"1":["a",1],"2":["b",2],"3":["c",3]}'}),patch.object(query_rpc,'query',send),patch.object(query_timing.time,'sleep',lambda _:None):
    ready=query_timing.wait_query_nodes_indexed(123,5)
    assert calls==[1,2,3,1,2,3] and set(ready)=={'2','3'}
    with patch.object(query_rpc,'query',lambda *a,**k:(SimpleNamespace(status=-21),b'')),patch.object(query_timing.time,'monotonic',side_effect=[0,0,0,0,6]):
        try:query_timing.wait_query_nodes_indexed(123,5)
        except AssertionError:pass
        else:raise AssertionError('permanent NODATA accepted')
print('query warmup: every serving catalog required; permanent NODATA fails PASS')
