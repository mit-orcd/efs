import importlib.util,subprocess
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
root='/tmp/efs-fault-repair-worker-20261012'
r=subprocess.run(m.ssh_command('fcstor006',f'cat > {root}/src/server/repair_worker.c',m.OPTIONS,m.USER,m.BRIDGE),input=Path('work/fix-orcd-valgrind/src/server/repair_worker.c').read_bytes(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
r=subprocess.run(m.ssh_command('fcstor006',f'cat > {root}/tests/test_repair_worker.c',m.OPTIONS,m.USER,m.BRIDGE),input=Path('work/fix-orcd-valgrind/tests/test_repair_worker.c').read_bytes(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
print(m.shell('fcstor006',f'''cd {root}
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
make -j8 EFS_GIT_ID=orcd-repair-final-wip EFS_FAULTS=1 efsd tests/test_repair_worker > /tmp/efs-repair-diag-build.log 2>&1
./tests/test_repair_worker > /tmp/efs-repair-diag-unit.log 2>&1
export EFS_FAULT_BIN={root}
set +e
for i in 1 2 3; do
    bash tests/faults/run_faults.sh /tmp/efs-repair-fixed-tcp-$i-20261012 --integrate --mode direct --transport tcp --repair-experiment > /tmp/efs-repair-fixed-tcp-$i.log 2>&1
    rc=$?; echo TCP_DIAG_$i=$rc; tail -2 /tmp/efs-repair-fixed-tcp-$i.log
    if [ "$rc" -ne 0 ]; then grep -h 'Repair ' /tmp/efs-repair-fixed-tcp-$i-20261012/cluster/s*/efsd.log; break; fi
done
''',1800),flush=True)
