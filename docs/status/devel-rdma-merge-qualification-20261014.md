# devel / RDMA merge qualification — 2026-10-14

User-requested `devel-rdma` -> `devel` integration. Status: merged candidate;
AMD qualification NOTRUN. No push. Scope files/direct/strict RDMA on exclusive
AMD Soft-RoCE rxe0/GID1; .efs client/server, packing, containers, expiry and
repair experiment disabled. Metadata balancing1; original deadlines retained.

## Integrated source and conflicts

RDMA endpoint `e6b6b83f4681b99d6a584c3448dc4806c18e1ee6` merged into devel
`7c5a59f5167ef448445808d19ef1c2f55e440991` as `26c3442395c8054cae936c34e4fe05eb1f7e166e`.
Intel active devel edits were checkpointed at
`1ac603128414c4e3d822b468bd9e4e93f3c51422`, checkout clean/detached by its
owner under explicit human coordination authorization. This checkpoint merged
as `11af39ca9` (Git records full identity). No uncommitted edits were overwritten.
Production changes merged cleanly. Status index conflict resolved by retaining
both Intel failure evidence and AMD source-specific passes; generated full
architecture Markdown/HTML regenerated, never resolved by dropping evidence.

Includes RDMA namespace early routing `a7e2b6b7d`, bounded failed-read peer
preference `ade336daf`, inactive query name-ordinal changes, Intel cached-body
EOF ownership `61e5c939d`, exact offline REAP and quota-shrink test corrections,
and default flow-off Raft pending ordered-frame admission `1ac603128`.
The Raft guard permits independent peer/group work and control escape; pending
queued data is released by dequeue, no durability or receipt credit changes.
EOF ownership now observes dirty/publication marks plus pinned cached bodies
atomically before adopting remote size. No optional feature was enabled.

Prior [RDMA read acceptance](rdma-paused-follower-read-20261014.md) on ade source
and [Intel devel evidence](intel-devel-merged-qualification-20261013.md) are
historical separate-source gates. Intel's depth timeouts stay OPEN; no TCP
comparison is run on AMD and no previous pass qualifies this merge automatically.

Local route/preference regressions and all five architecture checks PASS.
AMD clean native, repeated paused-follower32MiB, three quiet7-client/jobs16
4G/node core cohorts, peer/cold/EOF fault oracles and the new O_EXCL cached-EOF
visibility regression remain NOTRUN on this candidate.

## Reproduce affected gates

Freeze the merged full source SHA and export a complete tracked tree or the
same source/build/test/doc assets plus exact committed link targets. Record
SOURCE_COMMIT and EFS_GIT_ID/DESCRIBE/BRANCH=devel when building; clean native
build and `make test` first. Separately build fault-enabled FUSE from this exact
source for owned EOF refusal tests only. Drain/stop AMD installed cluster,
install native code and restart with strict RDMA/direct and required disabled
flags. Verify actual process environments, files/direct args and binary hashes.

On AMD, after all builds finish (OUTPUT and FAULT_SOURCE name fresh owned paths):

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
for r in 1 2 3; do
 if python3 -u tests/live/rdma_degraded_read.py /data1/efs/src /data1/efs/mnt \
     "$OUTPUT/paused-r$r" > "$OUTPUT/paused-r$r.log" 2>&1; then code=0; else code=$?; fi
 echo "$code" > "$OUTPUT/paused-r$r.exit"
done
for r in 1 2 3; do
 degraded=0; [ "$r" != 1 ] || degraded=1
 if EFS_GATE_DEGRADED=$degraded EFS_GATE_EOF_SOURCE="$FAULT_SOURCE" \
   bash tests/live/rdma_four_node_qualification.sh > "$OUTPUT/q$r.log" 2>&1; then code=0; else code=$?; fi
 echo "$code" > "$OUTPUT/q$r.exit"
done
python3 tests/live/exclusive_create_visibility.py /data1/efs/mnt --cases 1000 --workers 16
```

Budgets: read30s/pair5s, paused read outer180/k5; core ordinary15s/decorated75s,
seven distinct clients/jobs16/outer180s; O_EXCL stock30s per case, no fsync/retry
added. Private full fixture uses4 nodes,4G/node, ports27432..27435. Installed
stores17G/node. Drivers always restore paused follower first, drain reader/fault
mounts, stop owned daemons and delete only synthetic stores after clean teardown.
Keep failed markers and raw output outside Git; summary and commands in Git.
Avoid build/profiler/trace alongside quiet measurement. W106 aggregate and Intel
W100/deep-directory obligations remain distinct; no hardware closure implied.
