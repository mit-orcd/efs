#!/usr/bin/env python3
"""One place that says which C source files a splice-style test reads.

Many tests cut functions and structs out of a module's source with
`s.index('static int name(')` and compile a program around them. They read
the module through `module(name)` so that splitting a source file into a
directory later changes only the MODULES table below.

Tests use first-match `str.index`, so the concatenation order is part of the
contract: a later split must keep each function's first occurrence unique
and keep a definition before the text that is searched for after it.

An environment variable per module replaces the file list with one path
(an unchanged-source negative control, or a copy of the source). A test with
its own historical variable name passes it as `env`.
"""
import os
from pathlib import Path

root = Path(__file__).resolve().parents[1]

# module name -> (override environment variable, source files in order)
MODULES = {
    'raft_host': ('EFS_RAFT_HOST_SOURCE', ['src/server/raft_host.c']),
    'efs_fuse': ('EFS_FUSE_SOURCE', [
        'src/client/efs_fuse/support.c',
        'src/client/efs_fuse/getattr.c',
        'src/client/efs_fuse/open.c',
        'src/client/efs_fuse/writeback.c',
        'src/client/efs_fuse/append.c',
        'src/client/efs_fuse/write.c',
        'src/client/efs_fuse/namespace.c',
        'src/client/efs_fuse/release.c',
        'src/client/efs_fuse/plock_init.c',
        'src/client/efs_fuse/lookup.c',
        'src/client/efs_fuse/ll_run.c',
        'src/client/efs_fuse/main.c',
        'src/client/efs_fuse/fuse_internal.h',
        'src/client/efs_fuse/support.h',
        'src/client/efs_fuse/getattr.h',
        'src/client/efs_fuse/open.h',
    ]),
    'write': ('EFS_WRITE_SOURCE', ['src/client/write.c']),
}


def module(name, env=None):
    var, files = MODULES[name]
    override = os.environ.get(env or var)
    paths = [Path(override)] if override else [root / f for f in files]
    return ''.join(p.read_text() for p in paths)


def hblock(name, start, end='\n', env=None):
    """Text from `start` through `end` in the module's headers (the `.h` entries,
    searched in order): a type a splice needs when its slice used to contain
    that type's definition. Empty when an override file replaces the module (it
    still holds the type inline)."""
    var, files = MODULES[name]
    if os.environ.get(env or var):
        return ''
    h = ''.join((root / f).read_text() for f in files if f.endswith('.h'))
    a = h.index(start)
    return h[a:h.index(end, a) + len(end)]


def upto(text, marker):
    """Index where the declaration containing `marker` starts, so a slice that
    ends before a function also stops before its `static ` when it has one."""
    i = text.index(marker)
    return i - 7 if text[i - 7:i] == 'static ' else i


if __name__ == '__main__':
    import tempfile
    for key in {v for v, _ in MODULES.values()} | {'EFS_CSRC_TEST'}:
        os.environ.pop(key, None)
    for name, (var, files) in MODULES.items():
        assert module(name) == ''.join((root / f).read_text() for f in files), name
    with tempfile.TemporaryDirectory() as d:
        alt = Path(d) / 'alt.c'
        alt.write_text('int alt;\n')
        os.environ['EFS_RAFT_HOST_SOURCE'] = str(alt)
        assert module('raft_host') == 'int alt;\n'
        assert module('efs_fuse') != 'int alt;\n'
        os.environ['EFS_CSRC_TEST'] = str(alt)
        assert module('write', 'EFS_CSRC_TEST') == 'int alt;\n'
    assert upto('a static int f(', 'int f(') == 2 and upto('a int f(', 'int f(') == 2
    print('csrc self-check ok')
