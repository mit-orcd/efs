"""Strict verification of an on-host copy of the stopped Task I fixture."""
import hashlib
import os
from pathlib import Path
import re


def verify_fixture_copy(saved, original):
    saved, original = Path(saved), Path(original)
    files, links = [], []
    for path in sorted(saved.rglob('*')):
        relative = path.relative_to(saved)
        source = original/relative
        store = relative.parts[0] in ('n1', 'n2', 'n3')
        if path.is_symlink():
            assert (store and relative.parts[1:] == ('mdraft', 'query-cache', 'current')), 'unexpected synthetic evidence link'
            target = os.readlink(path)
            assert re.fullmatch(r'view-[A-Za-z0-9]{6}', target), 'unsafe query-cache target'
            assert source.is_symlink() and os.readlink(source) == target, 'copied link differs'
            # Exactly the immutable published view, within its own cache.
            # Neither a link chain nor an external/absolute target is allowed.
            for parent in (path.parent, source.parent):
                view = parent/target
                assert not view.is_symlink() and view.is_dir(), 'missing or indirect query view'
                assert view.resolve().parent == parent.resolve(), 'query view escapes cache'
            links.append(dict(path=str(relative),target=target,
                              sha256=hashlib.sha256(os.fsencode(target)).hexdigest()))
        elif path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if store:
                assert not source.is_symlink() and source.is_file(), 'original entry type differs'
                assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, 'store copy differs'
            files.append(digest+'  '+str(relative)+'\n')
    # A missing entry must fail too, not merely every copied file matching.
    for n in ('n1','n2','n3'):
        expected = {str(p.relative_to(original/n)) for p in (original/n).rglob('*')}
        actual = {str(p.relative_to(saved/n)) for p in (saved/n).rglob('*')}
        assert actual == expected, 'copied store entry set differs'
    return files, links
