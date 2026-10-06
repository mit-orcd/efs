#!/usr/bin/env python3
"""Exercise actual cluster stop and remount scripts with isolated transports."""
from pathlib import Path
import os
import subprocess
import tempfile
cluster = Path(os.environ.get('EFS_CLUSTER_SCRIPTS', str(Path.home()/'git/cluster')))
with tempfile.TemporaryDirectory(prefix='efs-cluster-stop-') as directory:
    task=Path(directory);(task/'bin').mkdir()
    (task/'source/scripts').mkdir(parents=True)
    source=Path(__file__).resolve().parents[1]/'scripts'
    for name in ('client.sh','client_processes.py'):
        (task/'source/scripts'/name).write_text((source/name).read_text())
    for name in ('cluster.sh','ct-up.sh','gw-up.sh'):
        (task/name).write_text((cluster/name).read_text())
    (task/'hosts.sh').write_text('#!/bin/bash\nexit 0\n');(task/'hosts.sh').chmod(0o755)
    (task/'bin/sshpass').write_text('''#!/bin/bash
case "$*" in
 *"rsync -az"*)
   echo "BOOTSTRAP:$*" >> "$LOG"
   case "$*" in *"admin@${SYNC_FAIL:-never}:"*) exit 2;; esac
   exit 0;;
 *"client_processes.py has"*) exit "${PROCESS_RC:-0}";; esac
exit 0
''');(task/'bin/sshpass').chmod(0o755)
    (task/'env.sh').write_text('''STATE_DIR="$TEST_DIR"
NODES=(n1 n2 n3)
GW=gw
CLIENTS=(c1 c2)
CLUSTER_NAME=test
EFS_PORT=17432
EFS_SRC="$TEST_DIR/source"
SSH_USER=admin
SSH_PASS=unused
SSH_OPTS=
ip_of() { echo "$1"; }
vm_is_running() { return 0; }
wait_ssh() { return 0; }
ensure_guest_tz() { return 0; }
rsync_to() { return 0; }
ssh_node() {
  local host=$1; shift
  echo "$host:$*" >> "$LOG"
  case "$*" in
    *"client_processes.py list"*)
      [ "$host" = "${DISCOVERY_FAIL:-none}" ] && return 2
      [ "$host" = gw ] && echo /mnt/efs
      return 0;;
    *"client.sh stop"*)
      [ "$host" = "${REFUSE:-none}" ] && return 1
      echo 0 > "$STATE"; return 0;;
    *"mountpoint -q"*) [ "$(cat "$STATE")" = 1 ]; return $?;;
    *"client.sh "*) echo 1 > "$STATE"; return 0;;
  esac
  return 0
}
''')
    def run(script,*args,**settings):
        log=task/'log';state=task/'state';log.write_text('');state.write_text('0')
        env=dict(os.environ,PATH=str(task/'bin')+':'+os.environ['PATH'],LOG=str(log),STATE=str(state),TEST_DIR=str(task),**settings)
        result=subprocess.run(['bash',str(task/script),*args],env=env,capture_output=True,text=True)
        return result,log.read_text()
    r,log=run('cluster.sh','stop')
    assert r.returncode==0 and 'gw:cd /tmp/efs && sudo ./scripts/client.sh stop /mnt/efs' in log
    assert log.index('gw:cd /tmp/efs && sudo ./scripts/client.sh stop') < log.index('systemctl stop')
    assert 'fusermount' not in log
    assert log.index('BOOTSTRAP:') < log.index('client_processes.py list')
    assert 'client_processes.py admin@n1:/tmp/efs/scripts/' in log
    for settings in ({'REFUSE':'gw'},{'DISCOVERY_FAIL':'c1'},{'SYNC_FAIL':'n1'}):
        r,log=run('cluster.sh','stop',**settings)
        assert r.returncode!=0 and 'systemctl stop' not in log and 'pkill' not in log
    helper=task/'source/scripts/client_processes.py'
    saved=helper.read_text();helper.unlink()
    r,log=run('cluster.sh','stop')
    assert r.returncode!=0 and not log and 'missing local stop tool' in r.stderr
    helper.write_text(saved)
    for script in ('ct-up.sh','gw-up.sh'):
        r,log=run(script)
        assert r.returncode!=0 and 'detached client' in r.stderr
        assert 'client.sh stop' not in log
        r,log=run(script,'--remount')
        assert r.returncode==0 and 'client.sh stop /mnt/efs' in log
        r,log=run(script,'--remount',REFUSE='c1' if script=='ct-up.sh' else 'gw')
        assert r.returncode!=0
    print('cluster scripts: detached discovery, drain refusal retains servers, remount preflight PASS')
