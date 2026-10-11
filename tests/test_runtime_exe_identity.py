#!/usr/bin/env python3
"""Actual final verifier identifies mapped executable, not relative/spoofed argv."""
import ast, os, shutil, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'tests/measure/nuc_final_service_20261011.py').read_text()
node=next(n for n in ast.parse(source).body if isinstance(n,ast.FunctionDef) and n.name=='runtime_kind')
namespace=dict(os=os,Path=Path);exec(ast.get_source_segment(source,node),namespace)
assert Path('/proc/self/exe').exists(),'Linux /proc required for this actual process identity gate'
with tempfile.TemporaryDirectory(prefix='efs-runtime-identity-') as directory:
 installed=Path(directory).resolve()/'src';installed.mkdir();(installed/'scripts').mkdir()
 shutil.copy2(shutil.which('sleep'),installed/'efs-fuse')
 for argv,cwd,executable,expected in ((['./efs-fuse','30'],installed,None,'efs-fuse'),
                                     (['../efs-fuse','30'],installed/'scripts',None,'efs-fuse'),
                                     (['./efs-fuse','30'],installed,shutil.which('sleep'),None)):
  proc=subprocess.Popen(argv,cwd=cwd,executable=executable)
  try:assert namespace['runtime_kind'](Path('/proc')/str(proc.pid),installed)==expected,(argv,expected)
  finally:proc.terminate();proc.wait(timeout=5)
print('actual runtime identity: relative/parent argv accepted, spoofed installed argv rejected PASS')
