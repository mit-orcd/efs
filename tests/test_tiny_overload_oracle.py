#!/usr/bin/env python3
"""A refused-I/O verdict cannot waive the acknowledged-byte cold oracle."""
import importlib.util
from pathlib import Path
import tempfile
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('tiny_sustained',root/'tests/live/tiny_sustained.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory(prefix='efs-overload-oracle-') as d:
 mount=Path(d);folder=mount/'tiny-sustained';folder.mkdir()
 tiny=folder/'tiny-0-0';expected=m.payload(0,3);tiny.write_bytes(b'wrong')
 import hashlib
 big=folder/'large-0';body=hashlib.sha256(b'large-0').digest()*32768
 big.write_bytes(body)
 unknown=folder/'unacknowledged';unknown.write_bytes(b'no receipt')
 record={'acknowledged_tiny':{'tiny-0-0':[0,3]},'acknowledged_large_sync_writes':[1,0,0,0],
         'overload_errors':[{'errno':28}]}
 try:m.verify_overload_and_delete(mount,record)
 except AssertionError:pass
 else:raise AssertionError('corrupt acknowledged tiny bytes accepted')
 assert tiny.exists() and unknown.exists()
 tiny.write_bytes(expected);big.write_bytes(b'wrong')
 try:m.verify_overload_and_delete(mount,record)
 except AssertionError:pass
 else:raise AssertionError('corrupt acknowledged large prefix accepted')
 assert tiny.exists() and unknown.exists()
 big.write_bytes(body);m.verify_overload_and_delete(mount,record)
 assert not folder.exists() and record['cold_acknowledged_tiny']==1
 assert record['cold_acknowledged_large_bytes']==1<<20
print('overload oracle: refusals preserve tiny/large byte checks; unknown files retire only after verification PASS')
