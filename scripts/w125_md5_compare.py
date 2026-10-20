#!/usr/bin/env python3
"""w125_md5_compare.py BASE NEW: every file in NEW must hash as in BASE (if BASE has it) and read without error.

BASE and NEW are tiny_files_md5.py JSONL manifests. Prints counts; exits 1
on any mismatch or read error. Files only in BASE were deleted; files only
in NEW are new and become part of the next baseline (merge with --merge OUT).
"""
import json, sys
def load(p):
    m, err = {}, 0
    for line in open(p):
        r = json.loads(line)
        if r.get('type') != 'file': continue
        if r.get('error') or r.get('status') not in (None, 'ok', 'stable'):
            err += 1; continue
        if 'md5' in r: m[r['path']] = r['md5']
    return m, err
base, _ = load(sys.argv[1]); new, err = load(sys.argv[2])
bad = [p for p in new if p in base and base[p] != new[p]]
print("files=%d compared=%d mismatches=%d read_errors=%d new=%d" % (len(new), sum(1 for p in new if p in base), len(bad), err, sum(1 for p in new if p not in base)))
for p in bad[:10]: print("MISMATCH", p)
if len(sys.argv) > 4 and sys.argv[3] == '--merge':
    base.update(new)
    with open(sys.argv[4], 'w') as f:
        for p, h in base.items(): f.write(json.dumps({'type': 'file', 'path': p, 'md5': h}) + '\n')
sys.exit(1 if bad or err else 0)
