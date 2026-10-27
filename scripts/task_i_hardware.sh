#!/usr/bin/env bash
# User-selected node9900: isolated RDMA then TCP D4 confirmations.
# COMMIT NEW_EXTERNAL_EVIDENCE_ROOT EXISTING_ORCD_MINI_CLIENT_PID
set -u
commit=${1:?source commit required}
art=${2:?new evidence root required}
client=${3:?existing node9900 client PID required}
[[ "$(hostname -s)" == node9900 && "$(pwd -P)" == /tmp/efs-taskI-*/src ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_RDMA_DEV=mlx5_0
export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib64/pkgconfig" LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib64"
snapshot() {
    python3 - "$1" <<'PY'
import json,sys,time
from pathlib import Path
base=Path('/sys/class/infiniband/mlx5_0/ports/1')
values={}
for d in ('counters','hw_counters'):
    for f in (base/d).glob('*'):
        try:values[d+'/'+f.name]=f.read_text().strip()
        except OSError:pass
Path(sys.argv[1]).write_text(json.dumps(dict(t=time.time(),state=(base/'state').read_text().strip(),counters=values),indent=2)+'\n')
PY
}
status=0
for transport in rdma tcp; do
    echo "$transport" > "$art/stage"
    snapshot "$art/$transport-before.json"
    bash scripts/task_i_run.sh "$commit" "$art/$transport" "$transport" 172.16.99.0 measure-only "$client"
    rc=$?;echo "$rc" > "$art/$transport-exit"
    snapshot "$art/$transport-after.json"
    # No new work after an unsafe cleanup or failed prerequisite/build.
    if test "$rc" -eq 125 || test "$rc" -eq 77 || test ! -f "$art/$transport/measurement/exit"; then exit "$rc";fi
    [ "$rc" -eq 0 ] || status=1
    python3 - "$art/$transport" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);lines=[]
for f in (p/'logs').rglob('*.log'):
    for line in f.read_text(errors='replace').splitlines():
        if 'RDMA transport up' in line or 'RDMA counters ' in line or 'transport ' in line:
            lines.append(str(f.relative_to(p))+': '+line)
(p/'transport-evidence.txt').write_text('\n'.join(lines)+'\n')
PY
    # Both independent fixtures stay until reviewed and their logs hashed.
done
echo FINISHED > "$art/stage"
exit "$status"
