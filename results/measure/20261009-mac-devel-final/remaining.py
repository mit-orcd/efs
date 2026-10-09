#!/usr/bin/python3
"""Run exactly the cases marked NOTRUN in a prior suite, using its normal runner."""
import csv, sys
sys.path.insert(0, '/tmp/efs/tests/posix')
import posix_suite
prior = sys.argv.pop(1)
names = {row[0] for row in csv.reader(open(prior), delimiter='\t') if len(row)>1 and row[1]=='NOTRUN'}
posix_suite.TESTS[:] = [(name, fn) for name, fn in posix_suite.TESTS if name in names]
print('Resuming %d previously unrun cases' % len(posix_suite.TESTS), flush=True)
sys.exit(posix_suite.main(sys.argv[1:]))
