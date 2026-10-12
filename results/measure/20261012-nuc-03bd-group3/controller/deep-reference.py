import importlib.util,os,sys,tempfile,shutil
from pathlib import Path
source=Path('/home/efs/nuc-03bd-group3/source-normal/tests/posix/posix_suite.py')
spec=importlib.util.spec_from_file_location('posix_reference',source);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
d=tempfile.mkdtemp(prefix='nuc-deep-reference-',dir='/tmp')
try:
 print('DEEP_REFERENCE_BEGIN',file=sys.stderr,flush=True)
 module.dir_deep_nesting(d)
 print('DEEP_REFERENCE_END',file=sys.stderr,flush=True)
finally:shutil.rmtree(d)
