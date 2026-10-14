#!/usr/bin/env python3
"""Lease ownership stays on the correct host across distributed query seeds."""
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

path=Path(__file__).resolve().parent/'live/query_rpc.py'
spec=importlib.util.spec_from_file_location('rpc',path)
rpc=importlib.util.module_from_spec(spec);spec.loader.exec_module(rpc)
nodes={'1':['node1',17432],'2':['node2',17432],'3':['node3',17432],'4':['node4',17432]}
base={'EFS_QUERY_TEST_HOST':'node2','EFS_QUERY_TEST_PORT':'17432','EFS_QUERY_TEST_NODES':json.dumps(nodes)}

def exercise(env, req, replies, expected):
    calls=[]
    def raw(port,q,*,host,timeout=7):
        calls.append((host,port));assert q is req and timeout==7
        status, owner, body=replies[len(calls)-1]
        return SimpleNamespace(status=status,primary_id=owner),body
    with patch.dict(os.environ,env,clear=True),patch.object(rpc,'query',raw):
        result=rpc.routed_query(17432,req)
    assert calls==expected,(calls,expected)
    return result

# Fresh request follows a seed redirect; retained stream and CLOSE go straight
# to token owner, even when another node is now the seed/current leader.
assert exercise(base,rpc.page_request(),[(-15,4,b''),(0,4,b'page')],
                [('node2',17432),('node4',17432)])[1]==b'page'
for limit in (0,10):
    assert exercise(base,rpc.page_request(token=(3<<56)|123,limit=limit),[(0,3,b'pinned')],
                    [('node3',17432)])[1]==b'pinned'
assert exercise({'EFS_QUERY_TEST_HOST':'localhost','EFS_QUERY_TEST_PORT':'17432'},
                rpc.page_request(token=(4<<56)|123),[(0,4,b'local')],
                [('localhost',17435)])[1]==b'local'
# An actual server error is retained, not rerouted into a silent success.
assert exercise(base,rpc.request(),[(-28,2,b'')],[('node2',17432)])[0].status==-28
for replies in ([(-15,4,b''),(-15,2,b'')],[(-15,9,b'')]):
    try:exercise(base,rpc.request(),replies,[])
    except AssertionError as error:
        assert any(reason in str(error) for reason in ("unknown query owner","query redirect loop")),error
    else:raise AssertionError('invalid owner/redirect loop accepted')
with patch.dict(os.environ,base,clear=True),patch.object(rpc,'query',side_effect=AssertionError('network used')):
    try:rpc.routed_query(17432,rpc.page_request(token=(9<<56)|1))
    except AssertionError as error:assert 'unknown query owner' in str(error),error
    else:raise AssertionError('unmapped token accepted')
print('distributed seed redirects, token owner/CLOSE, localhost compatibility, errors and invalid owners PASS')
