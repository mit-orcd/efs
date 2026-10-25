"""Copy only source inputs for the isolated decoder build.

Runtime stores, pipes, sockets, symlinks and build outputs are not inputs.
In particular a concurrent native test's TMPDIR must never enter the copy.
"""
from pathlib import Path
import shutil
import stat

SOURCE_ROOTS = {'Makefile', 'include', 'src', 'deps', 'tests', 'scripts', 'docs', '.gitignore'}
EXCLUDED = {'tmp', 'artifacts', 'results', '.git', '__pycache__', 'ACCOUNTS.txt'}

def copy_source(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if source == destination or source in destination.parents:
        raise ValueError('decoder build must be outside the source tree')
    def ignore(directory, names):
        directory = Path(directory)
        ignored = set()
        for name in names:
            path = directory/name
            if (directory == source and name not in SOURCE_ROOTS) or name in EXCLUDED:
                ignored.add(name)
                continue
            mode = path.lstat().st_mode
            if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)) or name.endswith(('.o', '.a', '.pyc')):
                ignored.add(name)
            if path.parent == source/'tests/decoders' and name in {'check', 'apply'}:
                ignored.add(name)
        return ignored
    shutil.copytree(source, destination, ignore=ignore)
