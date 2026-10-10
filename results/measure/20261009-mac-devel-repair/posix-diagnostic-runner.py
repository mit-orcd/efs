#!/usr/bin/python3
"""Diagnostic only: preserve all cases/assertions, allow slow durable VM I/O."""
import sys
sys.path.insert(0,'/tmp/efs/tests/posix')
import posix_suite
for name,fn in posix_suite.TESTS:
 fn._posix_timeout=max(300,getattr(fn,'_posix_timeout',0))
print('DIAGNOSTIC: case budget 300s; original 30/75/180s gate results remain failures',flush=True)
sys.exit(posix_suite.main(sys.argv[1:]))
