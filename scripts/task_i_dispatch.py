#!/usr/bin/env python3
"""Queue task I on nuc after H's completion AND removal of H's private tree.

Run on the Mac in a detached process. This performs only small read-only SSH
checks every five minutes while H owns the host. It never stops H, touches
/data1, retries a test, pushes, or overwrites an existing private source tree.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--commit', required=True)
p.add_argument('--state', type=Path, required=True)
a = p.parse_args()
assert re.fullmatch('[0-9a-f]{9,40}', a.commit)
root = Path(__file__).resolve().parents[1]
ssh = ['ssh', '-o', 'ConnectTimeout=20', 'nuc']

def state(label, **fields):
    a.state.write_text(json.dumps(dict(state=label, commit=a.commit, **fields), indent=2)+'\n')
    print(label, json.dumps(fields), flush=True)

probe = '''from pathlib import Path
import json
h=Path.home();p=h/'efs-artifacts/taskH-20261026'
print(json.dumps(dict(tree_exists=(h/'efs-taskH').exists(),
 stage=(p/'stage').read_text().strip() if (p/'stage').exists() else None,
 controller_exit=(p/'controller-exit').read_text().strip() if (p/'controller-exit').exists() else None,
 gate_summary=(p/'gate/summary.txt').read_text().splitlines()[0] if (p/'gate/summary.txt').exists() else None)))
'''
end = time.monotonic() + 8*3600
while time.monotonic() < end:
    try:
        r = subprocess.run(ssh + ['python3 -c '+shlex.quote(probe)], text=True,
                           capture_output=True, timeout=45)
        if r.returncode:
            state('WAIT_CONNECTION', error=r.stderr[-500:])
        else:
            h = json.loads(r.stdout)
            if (not h['tree_exists'] and h['stage']=='FINISHED' and
                    h['controller_exit'] is not None and h['gate_summary']):
                state('HANDOFF_CONFIRMED', task_h=h)
                break
            state('WAIT_TASK_H', task_h=h)
    except (subprocess.TimeoutExpired, ValueError) as error:
        state('WAIT_CONNECTION', error=str(error))
    time.sleep(300)
else:
    state('BLOCKED', reason='H did not release nuc within eight hours; no work started')
    raise SystemExit(77)

# mkdir is an exclusive claim; a partial export is preserved and never retried.
remote = ('test ! -e "$HOME/efs-taskH" && mkdir "$HOME/efs-taskI" && '
          'mkdir "$HOME/efs-taskI/src" && tar -xf - -C "$HOME/efs-taskI/src"')
state('EXPORTING')
archive = subprocess.Popen(['git','archive',a.commit], cwd=root, stdout=subprocess.PIPE)
try:
    result = subprocess.run(ssh+[remote], stdin=archive.stdout, timeout=180)
except (OSError, subprocess.TimeoutExpired) as error:
    state('BLOCKED', reason='source export outcome unknown; preserve partial tree; no retry', error=str(error))
    raise SystemExit(1)
finally:
    archive.stdout.close()
    archive_rc = archive.wait()
if result.returncode or archive_rc:
    state('BLOCKED', reason='source export failed; partial tree preserved, no retry')
    raise SystemExit(1)
launch = ('SCREENDIR="$HOME/.screen" screen -dmS taskI-20261026 bash -c '+
          shlex.quote('cd "$HOME/efs-taskI/src" && bash scripts/task_i_run.sh '+a.commit+
                      ' "$HOME/efs-artifacts/taskI-20261026" tcp'))
r = subprocess.run(ssh+[launch], timeout=45)
if r.returncode:
    state('BLOCKED', reason='detached launcher failed; source tree preserved')
    raise SystemExit(1)
state('LAUNCHED', host='nuc', screen='taskI-20261026', artifacts='~/efs-artifacts/taskI-20261026')
