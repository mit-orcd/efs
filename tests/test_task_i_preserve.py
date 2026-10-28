#!/usr/bin/env python3
"""Preservation accepts only exact internal published-view links and bytes."""
from pathlib import Path
import shutil
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from task_i_preserve import verify_fixture_copy
with tempfile.TemporaryDirectory(prefix='efs-preserve-test-') as directory:
    root=Path(directory);original=root/'original';saved=root/'saved'
    for n in ('n1','n2','n3'):
        cache=original/n/'mdraft/query-cache';(cache/'view-Ab123z').mkdir(parents=True)
        (cache/'view-Ab123z/data').write_bytes(b'exact synthetic bytes')
        (cache/'current').symlink_to('view-Ab123z')
    def copy():
        if saved.exists():shutil.rmtree(saved)
        shutil.copytree(original,saved,symlinks=True)
    def refused():
        try:verify_fixture_copy(saved,original)
        except AssertionError:return
        raise AssertionError('unsafe or changed copy accepted')
    copy();files,links=verify_fixture_copy(saved,original)
    assert len(files)==3 and len(links)==3
    link=saved/'n1/mdraft/query-cache/current'
    for target in ('/etc','../../outside','view-Missing','view-A12345/../outside'):
        copy();link.unlink();link.symlink_to(target);refused()
    copy();(saved/'n1/mdraft/query-cache/view-Ab123z/data').write_bytes(b'changed');refused()
    copy();(saved/'n2/mdraft/query-cache/view-Ab123z/data').unlink();refused()
    copy();(saved/'n1/extra').symlink_to('mdraft/query-cache/view-Ab123z');refused()
    copy();view=saved/'n1/mdraft/query-cache/view-Ab123z';shutil.rmtree(view)
    view.symlink_to(original/'n1/mdraft/query-cache/view-Ab123z');refused()
print('Task I preservation: exact files/internal view links PASS; changed, missing, external and indirect entries refused')
