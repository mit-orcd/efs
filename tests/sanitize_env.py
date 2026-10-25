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
