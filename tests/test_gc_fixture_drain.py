#!/usr/bin/env python3
"""A stale zero GC sample cannot certify physical fixture reclamation."""
from pathlib import Path
import ast,os
root=Path(__file__).resolve().parents[1]
tree=ast.parse(Path(os.environ.get('EFS_GC_FIXTURE_SOURCE',root/'tests/live/gc_reclamation.py')).read_text())
functions=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='physically_drained']
wait=next(node for node in ast.walk(tree) if isinstance(node,ast.Call) and isinstance(node.func,ast.Name) and node.func.id=='wait' and len(node.args)>1 and isinstance(node.args[1],ast.Constant) and node.args[1].value=='bulk compactor-pressure deletions drained before fault fixture')
state={'sample':True,'files':{11:['live-fragment'],22:[]},'queried':[]}
def files(ino):
    state['queried'].append(ino);return state['files'][ino]
namespace={'drained':lambda:state['sample'],'files':files,'pressure_inodes':[11,22]}
if functions:exec(compile(ast.Module(body=functions,type_ignores=[]),'actual_gc_fixture.py','exec'),namespace)
predicate=eval(compile(ast.Expression(body=wait.args[0]),'actual_compactor_wait.py','eval'),namespace)
def check(inodes):
    namespace['pressure_inodes']=inodes;return predicate()
assert not check([11,22]),'cached zero certified an existing owned fragment'
state['files'][11]=[];state['sample']=False;state['queried']=[]
assert not check([11,22]) and not state['queried'],'pending GC became a physical-drain success'
state['sample']=True
assert check([11,22]) and state['queried']==[11,22]
state['files'][22]=['late-member-fragment'];assert not check([11,22])
def failed_scan(ino):raise OSError('physical inventory unavailable')
namespace['files']=failed_scan
try:check([11,22])
except OSError:pass
else:raise AssertionError('failed physical inventory became drained')
print('GC fixture: stale-zero refusal, all owned inodes, pending-work refusal and scan errors PASS')
