#!/usr/bin/env python3
"""W96 Linux memory-safety gates after timing/native tests, normal build untouched."""
import json,os,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010');out=base/'sanitizers';out.mkdir(exist_ok=False)
until=time.monotonic()+2400
while not (base/'closure/native-exit.txt').exists():
    assert time.monotonic()<until,'native completion timed out'
    time.sleep(1)
assert (base/'closure/native-exit.txt').read_text().strip()=='0'
def run(name,command,env=None):
    with (out/(name+'.log')).open('w') as log:
        result=subprocess.run(command,cwd=base/'src',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=180)
    (out/(name+'-exit.txt')).write_text(str(result.returncode)+'\n')
    assert result.returncode==0,name
    print(name,'PASS',flush=True)
run('asan-ubsan-wal',['python3','tests/test_kv_wal_faults.py'],dict(os.environ,WAL_TEST_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer',ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
linked=subprocess.check_output(['ldd',str(base/'wal-reserve')],text=True)
assert 'libasan' not in linked and 'libubsan' not in linked
(out/'valgrind-binary-libraries.txt').write_text(linked)
run('valgrind-wal',['valgrind','--leak-check=full','--show-leak-kinds=all','--errors-for-leak-kinds=definite,indirect','--error-exitcode=99','--track-origins=yes',str(base/'wal-reserve'),str(out/'valgrind-owned.wal'),'2','16'])
(out/'exit.txt').write_text('0\n')
