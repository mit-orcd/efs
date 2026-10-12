#!/usr/bin/env python3
"""Exercise real fork/exec launches with fake perf/strace executables."""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
driver = r'''
#include <assert.h>
#include <sys/wait.h>
#include "src/common/recorder_start.h"
int main(int argc, char **argv) {
    assert(argc == 4);
    pid_t pid = !strcmp(argv[1], "perf") ?
        efs_start_perf_recorder(4242, argv[2]) :
        efs_start_strace_recorder(4242, argv[2]);
    if (!strcmp(argv[3], "parent-error")) { assert(pid == -1); return 0; }
    assert(pid > 0);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    assert(WEXITSTATUS(status) == atoi(argv[3]));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-recorder-launch-') as directory:
    base = Path(directory)
    source = base / 'driver.c'
    binary = base / 'driver'
    source.write_text(driver)
    flags = ['-fsanitize=address,undefined'] if os.getenv('EFS_TEST_SANITIZE') == '1' else []
    subprocess.run([*shlex.split(os.getenv('CC', 'cc')), *flags, '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-I'+str(root), str(source), '-o', str(binary)], check=True)
    tools = base / 'bin'
    tools.mkdir()
    for tool in ('perf', 'strace'):
        executable = tools / tool
        executable.write_text('#!'+sys.executable+'\nimport json,os,sys\n'
                              'open(os.environ["FAKE_RECORD_FILE"],"w").write(json.dumps(sys.argv[1:]))\n')
        executable.chmod(0o755)
    output = base / 'argv.json'
    env = dict(os.environ, PATH=str(tools), FAKE_RECORD_FILE=str(output))
    env.pop('EFS_STRACE_EXPR', None)
    path = str(base / 'new parent' / 'nested' / 'capture with spaces')
    subprocess.run([str(binary), 'perf', path, '0'], env=env, check=True)
    assert Path(path).parent.is_dir()
    assert json.loads(output.read_text()) == ['record', '-g', '-F', '999', '-p', '4242', '-o', path]
    for expr in (None, '', 'trace=fsync,writev - literal space'):
        e = dict(env)
        if expr is not None:
            e['EFS_STRACE_EXPR'] = expr
        subprocess.run([str(binary), 'strace', path, '0'], env=e, check=True)
        expected = ['-f', '-tt', '-T', '-qq', '-o', path, '-p', '4242']
        if expr:
            expected += ['-e', expr]
        assert json.loads(output.read_text()) == expected
    # Failed exec is a child exit, not a successful recording.
    empty = base / 'empty-bin'
    empty.mkdir()
    for tool in ('perf', 'strace'):
        subprocess.run([str(binary), tool, path, '1'], env=dict(env, PATH=str(empty)), check=True)
    # Invalid parent must refuse before creating a child.
    blocked = base / 'file'
    blocked.touch()
    for bad in ('', str(blocked / 'capture'), 'x'*8192):
        subprocess.run([str(binary), 'perf', bad, 'parent-error'], env=env, check=True)
print('recorder launches: real fork/exec, argv, filters, parent creation, exec failure and invalid paths PASS')
