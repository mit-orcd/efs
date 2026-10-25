#!/usr/bin/env python3
"""Run actual recorder stop wrappers with stubbed process syscalls."""
from pathlib import Path
import argparse
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Test historical wrappers, e.g. HEAD before the fix')
args = parser.parse_args()
files = [('src/bench/bench_local.c', 'stop_perf_recorder'),
         ('src/client/efs_bench.c', 'stop_perf_recorder'),
         ('src/client/efs_fuse.c', 'stop_recorder'),
         ('src/server/efsd.c', 'stop_recorder')]
source = r'''
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
static int mode, polls, blocks, terms, kills, sleeps, reaped;
static pid_t g_perf_pid;
static pid_t fake_waitpid(pid_t pid, int *status, int options) {
    (void)status; assert(pid == 123);
    if (!options) {
        blocks++;
        if (blocks == 1) { errno = EINTR; return -1; }
        reaped = 1; return pid;
    }
    assert(options == WNOHANG); polls++;
    if (mode == 0 || (mode == 1 && polls == 2) || (mode == 3 && polls == 3)) {
        reaped = 1; return pid;
    }
    if (mode == 2 || (mode == 4 && polls == 2)) {
        reaped = 1; errno = ECHILD; return -1;
    }
    if ((mode == 3 && polls == 1) || mode == 6) { errno = EINTR; return -1; }
    if (mode == 7) { errno = EINVAL; return -1; }
    return 0;
}
static int fake_kill(pid_t pid, int sig) {
    assert(pid == 123 && !reaped);
    if (sig == SIGTERM) terms++;
    else { assert(sig == SIGKILL); kills++; }
    return 0;
}
static int fake_usleep(useconds_t delay) { assert(delay == 100000); sleeps++; return 0; }
#define waitpid fake_waitpid
#define kill fake_kill
#define usleep fake_usleep
#include "src/common/recorder_stop.h"
'''
for i, (filename, function) in enumerate(files):
    text = (subprocess.check_output(['git', 'show', args.revision + ':' + filename], cwd=root, text=True)
            if args.revision else (root / filename).read_text())
    match = re.search(r'^static void ' + function + r'\([^;]+?\)\n\{', text, re.M)
    if not match:
        raise RuntimeError(filename)
    body = text[match.start():text.index('\n}', match.end()) + 2]
    source += '\n' + body.replace(function, 'stop_' + str(i), 1)
    if function == 'stop_perf_recorder':
        source += f'\nstatic void invoke_{i}(pid_t *p) {{ g_perf_pid = *p; stop_{i}(); *p = g_perf_pid; }}\n'
    else:
        source += f'\nstatic void invoke_{i}(pid_t *p) {{ stop_{i}(p); }}\n'
    if not args.revision:
        assert '#include "../common/recorder_stop.h"' in text, filename
        assert 'efs_stop_recorder(' in body, filename
source += r'''
static void scenario(void (*stop)(pid_t *), int which) {
    mode = which; polls = blocks = terms = kills = sleeps = reaped = 0;
    pid_t pid = 123; stop(&pid);
    if (mode == 6 || mode == 7) {
        assert(pid == 123 && !terms && !kills && !blocks);
        assert(polls == (mode == 6 ? 50 : 1));
        return;
    }
    assert(pid == -1);
    assert(terms == ((mode == 0 || mode == 2) ? 0 : 1));
    assert(kills == (mode == 5 ? 1 : 0));
    assert(blocks == (mode == 5 ? 2 : 0));
    assert(sleeps == (mode == 5 ? 50 : ((mode == 0 || mode == 2) ? 0 : 1)));
    int old_polls = polls; stop(&pid); assert(polls == old_polls);
    pid = 0; stop(&pid); assert(polls == old_polls && pid == 0);
}
int main(void) {
    void (*stops[])(pid_t *) = {invoke_0, invoke_1, invoke_2, invoke_3, efs_stop_recorder};
    for (unsigned i = 0; i < sizeof(stops) / sizeof(stops[0]); i++)
        for (int which = 0; which < 8; which++) scenario(stops[i], which);
    puts("recorder stops: four actual wrappers + shared bench helper, reap/ECHILD/EINTR/timeout/error and repeat-stop PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-recorder-stop-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c99', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
