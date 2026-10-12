import importlib.util
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
root='/tmp/efs-repair-final-normal-20261012';runtime=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
print(m.shell('fcstor006',f'''test ! -e {root}
cp -a /tmp/efs-fault-repair-worker-20261012 {root}
cd {root}
export PATH=/tmp/efs-repair-private-compiler-20261012:$PATH
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={runtime}:{m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
make clean > /tmp/efs-repair-final-normal-clean.log 2>&1
make -k -j12 EFS_GIT_ID=orcd-repair-normal-wip EFS_FAULTS=0 all test > /tmp/efs-repair-final-normal.log 2>&1
printf '0\\n' > /tmp/efs-repair-final-normal.exit
tail -5 /tmp/efs-repair-final-normal.log
''',7200),flush=True)
