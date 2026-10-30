#!/usr/bin/env python3
"""Owned current-format listing gate; retain the fixture for checked cleanup."""
import importlib.util
import os
import shutil
import sys
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('qualify_ao',root/'scripts/qualify_ao_readdir.py')
ao=importlib.util.module_from_spec(spec);spec.loader.exec_module(ao)
if not ao.k.EFSD.exists() or not ao.k.FUSE.exists() or not os.access('/dev/fuse',os.R_OK|os.W_OK) or not shutil.which('fusermount3'):
    print('NOTRUN: AO live gate needs fault daemon, FUSE binary/device and fusermount3');sys.exit(0)
fixture=Path(tempfile.mkdtemp(prefix='efs-ao-live-',dir=os.environ.get('TMPDIR')))
started=time.monotonic()
try:
    ao.run(SimpleNamespace(work=fixture/'work',output=fixture/'evidence',
                           transport='tcp',bind='127.0.0.1',base_port=None,posix=False))
finally:
    print('AO retained fixture',fixture,'elapsed_s',round(time.monotonic()-started,3),flush=True)
