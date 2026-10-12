import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
original_ssh=m.ssh_command
def forward(host,remote,options,user,bridge):
 argv=original_ssh(host,remote,options,user,bridge)
 if bridge:argv[-1]=argv[-1].replace('ssh -o ', 'ssh -A -o ',1)
 return argv
m.ssh_command=forward
p=Path('operations/orcd-packing-fix');rev=json.loads((p/'profile-stage.json').read_text())['revision'];tag=rev[:12]
body=f'''cd /tmp/efs
cat > /tmp/efs-profile-peer-ssh-{tag} <<'SH'
#!/bin/sh
exec timeout 15 ssh -o BatchMode=yes -o ConnectTimeout=8 erbmi1@"$1" "$2"
SH
chmod +x /tmp/efs-profile-peer-ssh-{tag}
set +e
python3 tests/posix/posix_2client.py --prepare /tmp/efs-mount --parent profile-peer-{tag} || exit 1
python3 -u tests/posix/posix_2client.py --remote fcstor007 fcstor008 --mnt /tmp/efs-mount --ssh /tmp/efs-profile-peer-ssh-{tag} --parent profile-peer-{tag} --results {m.BULK}/fcstor007/profile-peer-{tag}.tsv > {m.BULK}/fcstor007/profile-peer-{tag}.log 2>&1
rc=$?; printf '%s\\n' "$rc" > {m.BULK}/fcstor007/profile-peer-{tag}.exit
tail -10 {m.BULK}/fcstor007/profile-peer-{tag}.log
'''
print(m.shell('fcstor007',body,1200),flush=True)
