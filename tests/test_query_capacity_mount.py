#!/usr/bin/env python3
"""The live direct-I/O oracle validates its actual mount without forking readers."""
import ast
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
source=Path(__file__).with_name('live')/'efs_query_capacity.py'
tree=ast.parse(source.read_text())
function=next(node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='mounted')
assert not any(isinstance(node,ast.Name) and node.id=='subprocess' for node in ast.walk(function))
module=ast.Module(body=[function],type_ignores=[])
for filesystem in ['fuse.efs-fuse','nfs4','ext4',None]:
    # The older covered mount at the same path must never qualify the new one.
    rows=['11 1 0:5 / /tmp/efs-mount rw - fuse.efs-fuse old rw']
    if filesystem:rows.append('12 1 0:6 / /tmp/efs-mount rw - '+filesystem+' active rw')
    handles={'/proc/self/fdinfo/9':'pos: 0\nmnt_id: 12\n','/proc/self/mountinfo':'\n'.join(rows)}
    def path(value):return SimpleNamespace(read_text=lambda:handles[value])
    os=SimpleNamespace(open=Mock(return_value=9),close=Mock(),O_PATH=0x200000,O_DIRECTORY=0x10000)
    namespace={'Path':path,'os':os,'a':SimpleNamespace(mount='/tmp/efs-mount')}
    exec(compile(module,str(source),'exec'),namespace)
    try:namespace['mounted']()
    except AssertionError:assert filesystem!='fuse.efs-fuse'
    else:assert filesystem=='fuse.efs-fuse'
    os.close.assert_called_once_with(9)
    os.open.assert_called_once_with('/tmp/efs-mount',os.O_PATH|os.O_DIRECTORY)
print('capacity mount: exact active mount accepted; stacked/local/NFS/missing refused; no reader fork PASS')
