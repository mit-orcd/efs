import importlib.util,subprocess
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
root='/tmp/efs-fault-fix-rdma-20261012'
print(m.shell('fcstor006',f'test ! -e {root}; cp -a /tmp/efs-orcd-tests-8b7391b3f3ed {root}',timeout=120),flush=True)
r=subprocess.run(m.ssh_command('fcstor006',f'cat > {root}/tests/faults/harness.py',m.OPTIONS,m.USER,m.BRIDGE),input=Path('work/fix-orcd-valgrind/tests/faults/harness.py').read_bytes(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
print(m.shell('fcstor006',f'cd {root}; export LD_LIBRARY_PATH={m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}; export EFS_FAULT_BIN={root}; set +e; bash tests/faults/run_faults.sh /tmp/efs-fault-fix-rdma-results-20261012 --integrate --mode direct --transport rdma --rdma-device mlx5_2; rc=$?; echo SUITE_EXIT=$rc; exit 0',timeout=3600),flush=True)
