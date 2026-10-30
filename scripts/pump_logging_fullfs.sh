#!/bin/bash
# Private 64 KiB tmpfs; fresh loopback cluster. No global mounts or storage.
set -eu
[ "$#" -eq 3 ] || { echo "usage: $0 SOURCE PRIVATE EVIDENCE"; exit 2; }
source_tree=$(realpath "$1")
private=$(realpath -m "$2")
evidence=$(realpath -m "$3")
mkdir "$private"
mkdir "$evidence"
exec unshare -Ur -m bash -s -- "$source_tree" "$private" "$evidence" <<'INNER'
set -eu
src=$1; private=$2; evidence=$3
mount --make-rprivate /
mkdir "$private/fullfs"
mount -t tmpfs -o size=64k,mode=700 tmpfs "$private/fullfs"
for i in 1 2 3; do : > "$private/fullfs/n$i.log"; done
dd if=/dev/zero of="$private/fullfs/fill" bs=4096 count=16 status=none
python3 - "$private/fullfs" <<'PY'
import os,sys
s=os.statvfs(sys.argv[1]); assert s.f_bavail == 0, s
print('private tmpfs bytes', s.f_blocks*s.f_frsize, 'available', s.f_bavail)
PY
cd "$src"
python3 scripts/pump_logging_profile.py --work "$private/work" \
 --output "$evidence/case" --sink full --obs 1 --full-path "$private/fullfs" \
 --full-release "$private/fullfs/fill" --require-progress
python3 - "$evidence/case" <<'PY'
import json,re,sys
from pathlib import Path
p=Path(sys.argv[1]); d=json.loads((p/'result.json').read_text()); assert d['verdict']=='PASS',d
for i in (1,2,3):
 text=(p/f'n{i}.log').read_text()
 values=re.findall(r'telemetry: overflow=\d+ sink_errors=(\d+)',text)
 assert values and max(map(int,values))>0, ('missing recovered sink-error count',i)
print('64 KiB full tmpfs: PASS, progress, exact payloads, recovered sink errors')
PY
umount "$private/fullfs"
INNER
