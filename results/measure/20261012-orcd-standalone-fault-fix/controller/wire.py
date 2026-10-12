import importlib.util,subprocess
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
root='/tmp/efs-fault-repair-worker-20261012';r=subprocess.run(m.ssh_command('fcstor006',f'cat > {root}/tests/live/repair_wire.py',m.OPTIONS,m.USER,m.BRIDGE),input=Path('work/fix-orcd-valgrind/tests/live/repair_wire.py').read_bytes(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
print(m.shell('fcstor006',f'cd {root}; python3 tests/live/repair_wire.py --binary {root}/efsd --build-id orcd-repair-final-wip --root /tmp/efs-repair-final-wire-20261012',600),flush=True)
