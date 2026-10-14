#!/usr/bin/env python3
"""Mounted repeated token close; no raw RPC transport or cluster changes.

Usage: python3 tests/live/query_page_close.py /tmp/efs-mount
Uses the existing root catalogue and production helper encoder. No files created.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mount', type=Path)
args = parser.parse_args()
assert os.path.ismount(args.mount)
source = Path(__file__).resolve().parents[2] / 'src/client/commands/command.py'
spec = importlib.util.spec_from_file_location('efs_command', source)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
directory = args.mount / '.efs'
nonce = int.from_bytes(os.urandom(4), 'big')

def name(limit, token=0, cursor=0):
    return helper.page_name('find', b'', 'contains', None, limit, token, cursor, nonce)

def first():
    result = helper.read_result(str(directory), name(1))
    return int(result['page']['token']), int(result['page']['cursor'])

def close(token, cursor):
    start = time.monotonic()
    # LOOKUP/GETATTR/OPEN can each issue the same close before read.
    with (directory / '.query' / name(0, token, cursor)).open('rb') as stream:
        assert stream.read() == b''
    return time.monotonic() - start

token, cursor = first()
try:
    seconds = [close(token, cursor) for _ in range(3)]
    try:
        helper.read_result(str(directory), name(1, token, cursor))
    except OSError as error:
        stale_errno = error.errno
    else:
        raise AssertionError('closed-token continuation unexpectedly succeeded')
    new_token, new_cursor = first()
    assert new_token != token, (token, new_token)
    close(new_token, new_cursor)
    print(json.dumps(dict(result='PASS', token=str(token), fresh_token=str(new_token),
                          close_seconds=seconds, stale_errno=stale_errno)), flush=True)
finally:
    close(token, cursor)
