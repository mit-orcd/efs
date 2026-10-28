#!/usr/bin/env python3
"""Run Task P serially in an owned intel2 tree; preserve every verdict.

Usage: python3 scripts/task_p_rehearsal.py FULL_COMMIT ARTIFACT_ROOT
Launch under detached screen from an exact archived source tree.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

source = Path(__file__).resolve().parents[1]
work_root = source.parent
commit, output = sys.argv[1:]
art = Path(output).resolve()
assert socket.gethostname().split('.')[0] == 'intel2', 'Task P owns intel2 only'
assert source.name == 'src' and work_root.parent == Path.home(), source
assert work_root.name == 'efs-taskP' or work_root.name.startswith('efs-taskP-'), source
assert len(commit) == 40 and all(c in '0123456789abcdef' for c in commit)
assert art.parent == Path.home() / 'efs-artifacts', art
assert art.name == 'taskP-20261027' or art.name.startswith('taskP-20261027-'), art
art.mkdir(parents=True, exist_ok=True)
assert not (art / 'controller.exit').exists(), 'refuse to overwrite a completed run'
os.chdir(source)
os.environ.update(EFS_GIT_ID=commit, EFS_GIT_DESCRIBE=commit,
                  EFS_GIT_BRANCH='agent/devel-task-p', EFS_FUSE_PREFIX='/usr',
                  EFS_BUILD_JOBS='8', EFS_TEST_JOBS='8', EFS_TEST_TARGET='test-all',
                  TMPDIR=str(work_root / 'tmp'))
Path(os.environ['TMPDIR']).mkdir(parents=True, exist_ok=True)
rows = []

def owned_active():
    # Existing fixtures from any other task are never stopped by this script.
    processes = subprocess.check_output(['ps', '-u', str(os.getuid()), '-o', 'pid=,args='], text=True)
    bad = [l for l in processes.splitlines() if str(work_root) + '/' in l and
           ('/efsd ' in l or '/efs-fuse ' in l or 'scripts/soak.py ' in l)]
    mounts = subprocess.run(['findmnt', '-rn', '-o', 'TARGET'], text=True, capture_output=True)
    bad += [l for l in mounts.stdout.splitlines() if str(work_root) + '/' in l or str(art) in l]
    if bad:
        raise RuntimeError('owned work still active; no next stage: ' + '\n'.join(bad))

def run(name, command):
    (art / 'stage').write_text(name + '\n')
    start = time.time()
    with (art / (name + '.log')).open('x') as log:
        p = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    rows.append(dict(stage=name, argv=command, start=start, end=time.time(),
                     seconds=round(time.time()-start, 3), rc=p.returncode))
    (art / 'results.json').write_text(json.dumps(rows, indent=2)+'\n')
    return p.returncode

rc = 1
try:
    (art / 'source.txt').write_text(commit+'\n'+str(source)+'\n')
    owned_active()
    for name, cmd in [
        ('plan', ['python3', 'scripts/qualify.py', 'plan', '--check', '--matrix', 'scripts/qualify_matrix.json']),
        ('build', ['make', '-j8', 'test-bins']),
        ('driver-regression', ['python3', 'tests/test_qualify_driver.py']),
        ('standalone-regression', ['python3', 'tests/test_qualify_standalone.py']),
        ('soak-regression', ['python3', 'tests/test_soak_driver.py'])]:
        if run(name, cmd):
            raise RuntimeError(name + ' failed; no workload launched')
    with (art / 'version.txt').open('w') as f:
        subprocess.run(['./efsd', '--version'], stdout=f, stderr=subprocess.STDOUT, check=True)
    for n in (3, 5):
        owned_active()
        name = 'loopback-' + str(n)
        # Both matrices run to completion even when individual gates fail.
        code = run(name, ['python3', 'scripts/qualify.py', 'run', '--matrix',
             'scripts/qualify_matrix.json', '--topology', name, '--candidate', commit,
             '--allow', 'all', '--out', str(art/name), '--work', str(work_root/f'work-{n}')])
        if code not in (0, 1, 3):
            raise RuntimeError(name + ' driver/usage failure; inspect before next topology')
        owned_active()
        if run('verify-'+name, ['python3', 'scripts/qualify.py', 'verify-sums', str(art/name)]):
            raise RuntimeError(name + ' evidence manifest failed')
    code = run('code-gate', ['bash', 'scripts/make_test_summary.sh'])
    gate = art/'code-gate'
    gate.mkdir()
    import shutil
    for name in ('summary.txt', 'timings.tsv', 'progress.txt', 'build.log', 'enum.log', 'cmds.txt', 'cmds.raw'):
        if (source/name).exists(): shutil.copy2(source/name, gate/name)
    for p in (source/'tmp').glob('fail-*.log'): shutil.copy2(p, gate/p.name)
    owned_active()
    rc = 1 if any(r['rc'] not in (0, 3) for r in rows) else (3 if any(r['rc']==3 for r in rows) else 0)
    (art/'stage').write_text('FINISHED\n')
except Exception as exc:
    (art/'controller-error.txt').write_text(repr(exc)+'\n')
    (art/'stage').write_text('STOPPED\n')
finally:
    (art/'controller.exit').write_text(str(rc)+'\n')
sys.exit(rc)
