#!/usr/bin/env python3
"""gc_reclamation files(): an export directory removed by physical reclaim during the scan reads as empty."""
from pathlib import Path
import ast,os,shutil,tempfile,types
root=Path(__file__).resolve().parents[1]
tree=ast.parse(Path(os.environ.get('EFS_GC_FIXTURE_SOURCE',root/'tests/live/gc_reclamation.py')).read_text())
functions=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name in ('export_entries','files')]
assert len(functions)==2,'gc_reclamation.py: export_entries()/files() not found'
state={'vanish':None,'refuse':None}
def scandir(path):
    if Path(path)==state['refuse']:raise PermissionError(13,'scan refused',str(path))
    if Path(path)==state['vanish']:shutil.rmtree(path)  # reclaim wins the race after the parent listing
    return os.scandir(path)
with tempfile.TemporaryDirectory(prefix='efs-gc-files-') as tmp:
    data=Path(tmp)
    namespace={'os':types.SimpleNamespace(scandir=scandir),'Path':Path,'data_roots':[data]}
    exec(compile(ast.Module(body=functions,type_ignores=[]),'actual_gc_files.py','exec'),namespace)
    files=namespace['files']
    ino=12345
    folder=data/'data/exports/1/0000/0000/0000/0001/2345'
    for sub in ('a','b'):(folder/sub).mkdir(parents=True);(folder/sub/'frag').write_text(sub);(folder/sub/'frag.sum').write_text('')
    assert sorted(x.parent.name for x in files(ino))==['a','b'],'resident fragments or .sum filtering wrong'
    state['vanish']=folder/'b'
    assert [x.parent.name for x in files(ino)]==['a'],'vanished subdirectory hid or broke its sibling'
    state['vanish']=folder
    assert files(ino)==[],'vanished inode directory did not read as reclaimed'
    assert files(99999)==[],'absent inode directory did not read as reclaimed'
    (folder/'c').mkdir(parents=True);state['vanish']=None;state['refuse']=folder/'c'
    try:files(ino)
    except PermissionError:pass
    else:raise AssertionError('a scan error other than a vanished directory was hidden')
print('GC fixture files(): vanished export directory empty, siblings kept, other scan errors raised PASS')
