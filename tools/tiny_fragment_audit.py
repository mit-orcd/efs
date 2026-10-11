#!/usr/bin/env python3
"""Read-only exact-path fragment allocation audit; never enumerate an EFS tree.

Run on each mapped storage node with its configured roots. Input inode,
chunk/fragment index and generation must come from the authoritative mapping.
A missing/duplicate object is an inconclusive audit, not zero physical cost.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import time


def unsigned(value, bits):
    n = int(value)
    if not 0 <= n < 1 << bits:
        raise ValueError("identifier outside unsigned %d-bit range" % bits)
    return n


def fragment_path(root, export, inode, chunk, fragment, generation):
    export, inode, chunk, generation = [unsigned(v, b) for v, b in
        ((export, 32), (inode, 64), (chunk, 32), (generation, 64))]
    fragment = unsigned(fragment, 32)
    if fragment > 2:
        raise ValueError("fragment index must be 0, 1 or 2")
    segments = []
    for _ in range(5):
        segments.append("%04d" % (inode % 10000))
        inode //= 10000
    name = "%d.%d" % (chunk, fragment)
    if generation:
        name += ".%d" % generation
    return Path(root).joinpath("data", "exports", str(export),
                               *reversed(segments), str(chunk >> 10), name)


def audit(roots, export, inode, chunk, fragment, generation, expected_bytes=None):
    if not roots or len(set(map(str, roots))) != len(roots):
        raise ValueError("provide distinct configured storage roots")
    rows, errors = [], []
    for root in roots:
        path = fragment_path(root, export, inode, chunk, fragment, generation)
        try:
            st = path.lstat()
            if not stat.S_ISREG(st.st_mode):
                raise ValueError("fragment is not a regular file")
            rows.append(dict(path=str(path), size=st.st_size,
                             allocated_bytes=st.st_blocks * 512,
                             device=st.st_dev, backing_inode=st.st_ino))
        except FileNotFoundError:
            continue
        except (OSError, ValueError) as exc:
            errors.append(dict(path=str(path), error=str(exc)))
    complete = len(rows) == 1 and not errors
    if expected_bytes is not None:
        complete = complete and rows[0]["size"] == expected_bytes
    return dict(host=os.uname().nodename, timestamp_ns=time.time_ns(),
                mapping=dict(export=export, inode=inode, chunk=chunk,
                             fragment=fragment, generation=generation),
                matches=rows, errors=errors, complete=complete,
                expected_bytes=expected_bytes)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", action="append", required=True)
    for name, bits in (("export", 32), ("inode", 64), ("chunk", 32),
                       ("fragment", 32), ("generation", 64)):
        p.add_argument("--" + name, type=lambda v, bits=bits: unsigned(v, bits), required=True)
    p.add_argument("--expected-bytes", type=int)
    a = p.parse_args()
    if a.expected_bytes is not None and a.expected_bytes < 0:
        p.error("expected bytes must be nonnegative")
    try:
        row = audit(a.root, a.export, a.inode, a.chunk, a.fragment,
                    a.generation, a.expected_bytes)
    except ValueError as exc:
        p.error(str(exc))
    print(json.dumps(row, sort_keys=True))
    return 0 if row["complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
