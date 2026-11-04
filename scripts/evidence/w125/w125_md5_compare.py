#!/usr/bin/env python3
"""w125_md5_compare.py BASE NEW: every file in NEW must hash as in BASE (if BASE has it) and read without error.

BASE and NEW are tiny_files_md5.py JSONL manifests. Prints counts; exits 1
on any mismatch or read error. Files only in BASE were deleted; files only
in NEW are new and become part of the next baseline (merge with --merge OUT).
With concurrent writers and deleters: a file whose byte count differs was
still being written when BASE read it ("grown", skipped); entries marked
changed, and files deleted while being read (No such file), are skipped.
"""
import json, sys
def load(p):
    m, err = {}, 0
    for line in open(p):
        r = json.loads(line)
        if r.get('type') != 'file' or r.get('status') == 'changed': continue
        if r.get('error') or r.get('status') not in (None, 'ok', 'stable'):
            if 'No such file' not in str(r.get('error', '')): err += 1
            continue
        if 'md5' in r: m[r['path']] = (r['md5'], r.get('bytes'))
    return m, err
base, _ = load(sys.argv[1]); new, err = load(sys.argv[2])
both = [p for p in new if p in base]
grown = [p for p in both if base[p][1] is not None and base[p][1] != new[p][1]]
bad = [p for p in both if p not in grown and base[p][0] != new[p][0]]
print("files=%d compared=%d mismatches=%d read_errors=%d new=%d grown=%d" % (len(new), len(both) - len(grown), len(bad), err, len(new) - len(both), len(grown)))
for p in bad[:10]: print("MISMATCH", p)
if len(sys.argv) > 4 and sys.argv[3] == '--merge':
    base.update(new)
    with open(sys.argv[4], 'w') as f:
        for p, (h, b) in base.items(): f.write(json.dumps({'type': 'file', 'path': p, 'md5': h, 'bytes': b}) + '\n')
sys.exit(1 if bad or err else 0)
