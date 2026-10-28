"""Sanitizer and memcheck plumbing for the Python-driven C harness tests.

The F1 gates run these tests three ways besides `make test`:
- `make leak-check` sets EFS_LEAK_WRAP to the valgrind memcheck command;
  the test runs its binary under it (and must not add ASan, which cannot
  share a process with memcheck);
- `make asan-check` sets EFS_TEST_SANITIZE=1: compile with
  -fsanitize=address,undefined;
- `make tsan-check` sets EFS_TEST_TSAN=1: compile with -fsanitize=thread.
A harness that links the tree's libefs.a must use these same flags, since
the gate's tree is built with them (tests/sanitizer_check.sh).

    from sanitize_env import cflags, wrap
    subprocess.run(cc + cflags() + [...]); subprocess.run(wrap() + [exe])
"""
import os, shlex


def cflags():
    if os.environ.get('EFS_TEST_TSAN') == '1':
        return ['-fsanitize=thread']
    if os.environ.get('EFS_TEST_SANITIZE') == '1' or os.environ.get('EFS_SANITIZE') == '1':
        if os.environ.get('EFS_LEAK_WRAP'):
            return []
        return ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    return []


def wrap():
    return shlex.split(os.environ.get('EFS_LEAK_WRAP', ''))


def timeout(seconds):
    """Sanitized or memcheck'd runs are several times slower."""
    return seconds * (10 if wrap() else 5 if cflags() else 1)


def asan_runtime_available():
    """Probe ASan/LSan independently; never classify a tested program's error.

    Some hosts forbid LSan's stop-the-world ptrace even for its own child.
    Keep that host limitation NOTRUN without dropping functional/negative
    controls. All other probe errors, and every error in a real harness,
    remain failures. Never disable leak detection to manufacture a pass.
    """
    import subprocess
    import tempfile
    from pathlib import Path
    source = r"""
#include <pthread.h>
#include <stdlib.h>
static void *worker(void *p) { return p; }
int main(void) {
    pthread_t thread;
    void *p = malloc(8);
    if (!p) return 2;
    if (pthread_create(&thread, 0, worker, p)) { free(p); return 3; }
    if (pthread_join(thread, 0)) return 4;
    free(p);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix='efs-asan-runtime-') as directory:
        path = Path(directory)
        c = path / 'probe.c'
        c.write_text(source)
        exe = path / 'probe'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-g', '-O1', '-pthread', '-fsanitize=address,undefined',
            str(c), '-o', str(exe)], check=True)
        env = dict(os.environ)
        # The probe must actually exercise leak checking, regardless of the
        # invoking shell. Keep unrelated diagnostic settings intact.
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=1'
        env['LSAN_OPTIONS'] = env.get('LSAN_OPTIONS', '') + ':verbosity=1:log_threads=1'
        result = subprocess.run([str(exe)], env=env, capture_output=True,
                                text=True, timeout=30)
        if result.returncode == 0:
            return True
        output = result.stdout + result.stderr
        if ('LeakSanitizer has encountered a fatal error.' in output and
                'Could not attach to thread ' in output and '(errno 1)' in output and
                'ERROR: AddressSanitizer:' not in output and
                'ERROR: LeakSanitizer:' not in output and
                'runtime error:' not in output):
            print(output, end='')
            print('NOTRUN: independent ASan/LSan runtime probe: host refuses '
                  'LeakSanitizer thread attachment; functional controls still run')
            return False
        print(output, end='')
        result.check_returncode()
        raise AssertionError('unreachable sanitizer probe result')
