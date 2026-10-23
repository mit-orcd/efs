#!/usr/bin/env python3
"""Every live harness and script that hand-builds an EFS_MSG_PUT_CHUNK frame
sends the K13 ticket header (tests/live/ticket_put.py), and builds an
unticketed PUT only as a refusal control.

Oct 23 (W158): K13 made the ticketed PUT the only data path, but
gc_reclamation.py still sent the pre-K13 native PUT header for its "valid
late-PUT control"; storage refused it (status 1) and the intel expiry
fixture stopped there. fragment_integrity.py, fragment_tail_smoke.py and
io_stats_smoke.py had the same header. Fast `make test` group.
Usage: test_live_ticketed_put.py [TREE] (default: this checkout).
"""
from pathlib import Path
import re, sys
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
OWNER = 'tests/live/ticket_put.py'  # owns the refusal-control header
SITE = re.compile(r"rpc\((?:\w+,\s*)?6\s*,|\+1\s*,\s*(?:6|ticket_put\.PUT_CHUNK)\s*\)")
RETIRED = re.compile(r"\+1\s*,\s*1(?:29|30)\s*\)")  # EFS_MSG_PUT_TICKETED
OLD_HEADER = re.compile(r"class Put\b|\('gen',[^)]*\)\s*,\s*\('hint',")
TICKETED = re.compile(r"\.body\(|unticketed_body\(")
REFUSED = re.compile(r"bytes\(\[7,\s*1\]\)|ticket_put\.ERROR")
bad, sites = [], 0
files = [p for d in ('tests/live', 'tests/measure', 'tests/faults') for p in (root/d).glob('*.py')]
files += [p for p in (root/'scripts').glob('*') if p.is_file()]
for p in sorted(files):
    rel = str(p.relative_to(root)); t = p.read_text(errors='replace'); lines = t.splitlines()
    if rel != OWNER and OLD_HEADER.search(t): bad.append(f'{rel}: pre-K13 PUT header')
    if RETIRED.search(t): bad.append(f'{rel}: retired PUT_TICKETED opcode')
    for n, line in enumerate(lines):
        if 'unticketed_body(' in line and rel != OWNER and 'def ' not in line:
            if not REFUSED.search(' '.join(lines[n:n+3])):
                bad.append(f'{rel}:{n+1}: unticketed PUT without a refusal (status 1) check')
        if not SITE.search(line): continue
        sites += 1
        if TICKETED.search(line): continue
        # The frame's body is a variable: assigned from a ticket body, or a
        # parameter of a sender whose every call passes one.
        var = re.search(r"\+\s*(\w+)\)", line)
        if var and re.search(rf"\b{var.group(1)}\s*=\s*[\w.]*\.body\(", t): continue
        fn = next((m.group(1) for l in reversed(lines[:n]) for m in [re.match(r"\s*def (\w+)\(", l)] if m), None)
        calls = [l for l in lines if fn and re.search(rf"\b{fn}\(", l) and not re.match(rf"\s*def {fn}\(", l)]
        if fn and calls and all(TICKETED.search(l) for l in calls): continue
        bad.append(f'{rel}:{n+1}: PUT_CHUNK frame without a ticket header')
for p in sorted((root/'tests/live').glob('*.c')):
    t = p.read_text(errors='replace'); rel = str(p.relative_to(root))
    if re.search(r"efs_send_msg\([^;]*EFS_MSG_PUT_CHUNK\s*,", t): bad.append(f'{rel}: PUT_CHUNK sent without the ticket header part')
    if re.search(r"efs_send_msg_parts\([^;]*EFS_MSG_PUT_CHUNK\s*,", t):
        sites += 1
        if 'efs_put_upload_header(' not in t: bad.append(f'{rel}: PUT_CHUNK header not built by efs_put_upload_header')
assert sites, 'no hand-built PUT_CHUNK frames found; update the patterns'
assert not bad, 'unticketed live PUTs: ' + '; '.join(bad)
print(f'live harness PUT_CHUNK frames: {sites} sites carry the K13 ticket header PASS')
