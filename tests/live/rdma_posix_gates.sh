#!/usr/bin/env bash
# Isolated same-host gates. Does not touch the installed cluster or stores.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
mode=${EFS_TRANSPORT:-rdma}
case "$mode" in tcp|rdma) ;; *) echo 'Use explicit tcp or rdma' >&2; exit 2;; esac
export EFS_TRANSPORT=$mode EFS_MD_RAFT_N=4
export EFS_RDMA_DEV=${EFS_RDMA_DEV:-rxe0}
if [ "$mode" = rdma ]; then
    test -d "/sys/class/infiniband/$EFS_RDMA_DEV" || { echo 'RDMA device missing; gates NOT RUN' >&2; exit 77; }
    "$src/tests/test_rdma_xprt" > /tmp/efs-rdma-preflight-$$.log 2>&1
    cat /tmp/efs-rdma-preflight-$$.log
    grep -q SKIP /tmp/efs-rdma-preflight-$$.log && { echo 'RDMA preflight skipped; gates NOT RUN' >&2; exit 77; }
fi
run=$(mktemp -d "${EFS_GATE_ROOT:-/home/efs/efs-rdma}/gate-$mode-XXXXXX")
mkdir -p "$run/mnt" "$run/mnt2"; pids=()
cleanup() {
    for m in "$run/mnt" "$run/mnt2"; do
        if mountpoint -q "$m"; then
            "$src/scripts/client.sh" stop "$m" || {
                if mountpoint -q "$m"; then echo "Retained cluster: clean stop failed at $m" >&2; return 1; fi
            }
        fi
    done
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
for id in 1 2 3 4; do
    port=$((27431+id)); join=(); [ "$id" = 1 ] || join=(--join 127.0.0.1:27432)
    "$src/efsd" --node-id "$id" --addr 127.0.0.1 --port "$port" --storage "$run/n$id" --quota 2G --direct-io "${join[@]}" > "$run/n$id.log" 2>&1 &
    pids+=("$!")
    ready=0
    for attempt in $(seq 1 60); do
        if grep -q listening "$run/n$id.log"; then ready=1; break; fi
        kill -0 "${pids[-1]}" || break; sleep .5
    done
    [ "$ready" = 1 ] || { cat "$run/n$id.log"; exit 1; }
done
"$src/efs-mgmt" raft-mkfs 127.0.0.1:27432
for m in "$run/mnt" "$run/mnt2"; do "$src/scripts/client.sh" 127.0.0.1:27432 "$m"; done
rc=0
python3 "$src/tests/posix/posix_suite.py" "$run/mnt" --jobs 4 --timeout-s 30 --results "$run/posix.tsv" > "$run/posix.log" 2>&1 || rc=1
python3 "$src/tests/posix/posix_2client.py" --local "$run/mnt" "$run/mnt2" --parent rdma-peer-gate --results "$run/posix2.tsv" > "$run/posix2.log" 2>&1 || rc=1
if [ "$mode" = rdma ]; then
    for id in 1 2 3 4; do grep -q 'RDMA transport up' "$run/n$id.log" || rc=1; done
    for label in mnt mnt2; do grep -q 'RDMA transport up' "$run/efs-fuse-$label.log" || rc=1; done
fi
"$src/efs-mgmt" status 127.0.0.1:27432 > "$run/status.txt"
tail -5 "$run/posix.log"; tail -5 "$run/posix2.log"; echo "Evidence: $run"
exit "$rc"
