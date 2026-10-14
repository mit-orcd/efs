# devel / RDMA merge qualification — 2026-10-14

User-requested `devel-rdma` -> `devel` integration. Status: merged candidate;
AMD affected qualification PASS on frozen source a40428d9d. No push. Scope files/direct/strict RDMA on exclusive
AMD Soft-RoCE rxe0/GID1; .efs client/server, packing, containers, expiry and
repair experiment disabled. Metadata balancing1; original deadlines retained.

## Frozen merged-source results

Qualified/deployed source **`a40428d9d5d97e4df23f8fc527c697669d9a071c`**, native
build2026-10-14T05:10:38Z (Oct14 01:10 EDT), branch `devel`. Clean full native
`make test` PASS, exit0. Normal installed and private fault-enabled client
builds share this frozen source; only owned EOF fault mount enables omission.
Installed binary SHA256:

- efsd `67cdd5ecbacb5b3e0dc3bf872ff0f11ecac5ccb61f7a7a0b1cfc09d349452666`
- efs-fuse `3191ff1f27d99cde88697b1884c0f2235aa6da6eb0df8afb4c6d825f3418451a`
- efs-mgmt `6430419454d14e0672c20c89a29576b66579edbdc7178bcac1fdea7a10218fa1`
- private fault-client `e5b0d9e5616898487629b80925fdfdd02cd9b58b9f6900dfb22a53236708bfab`

| Affected AMD gate | Verdict |
| --- | --- |
| Clean full native |PASS, all recipes |
| Three standalone paused follower2 reads, installed17G/node |32MiB exact bytes/digest each PASS; sum read calls2.617/2.614/2.608s |
| Fresh4G/node paused follower2 read after cold resume |32MiB exact bytes/digest PASS; sum read calls2.616s |
| Three quiet seven-client/jobs16 cores, fresh4G/node |1512/1512 each;4536/4536 PASS |
| Quiet deep/unusual-directory/unlink/link |42/42 deep,21/21 crazy dirs,21/21 named unlink and21/21 link PASS |
| Quiet peer |201/201 PASS |
| Cold prepare/verify |81/81 each PASS |
| W100 EOF fault oracle |6 cases,12 refused fsyncs,6 exact retry recoveries PASS |
| Quiet O_EXCL/close visibility,1000 cases/workers16 |1000/1000 PASS in5.225s, max case0.092s |
| Loaded O_EXCL/close visibility plus seven-client core |1000/1000 PASS in27.861s, max case1.437s; concurrent core1512/1512 in78.447s |
| Focused ASan/UBSan with leak checking |read preference, queued-frame admission and pending EOF all PASS |

Quiet fixtures: `/data1/efs/rdma-qualification-WNuuQH`34.775s,
`rdma-qualification-Y47OAa`30.784s, `rdma-qualification-qcLQnZ`40.199s;
all exit0 and no logged opid-admission refusals in the quiet clients. The loaded
visibility/core stress is additional evidence on installed17G/node, not folded
into the three original4G quiet comparisons. Its seven distinct clients retain
jobs16/core216/stock15s/decorated75s/outer180s. O_EXCL retains original two-child
case, no fsync/retry added,30s case bound plus180/k5 overall safety cap.
Its complete/cleanup markers PASS with no unresolved mounts or errors.

Every paused read selects follower2 only after checking FOLLOWER in both groups;
all256 deterministic128KiB blocks/four8MiB buffers/EOF are verified. Each digest
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.
Read phase excludes open/setup, caller verification and cleanup. Original
read30s/pair5s/outer180/k5 unchanged. Sparse EOF oracle checks low/high anchors
and exact EOF, not full-hole hash; full small payload checked. These do not
close original lost-EOF or hardware obligations.

4624 observed upgraded connection counter rows in reader/main/load logs have
post-setup TCP bytes0/0 and violations0; no payload fallback or TCP comparison.
All actual installed processes audited RDMA/rxe0/GID1, command client/server0,
packing0, expiry0, balancing1, daemon args files/17G/direct. `.efs` absent on
installed mount. No diagnostic/profiler/build alongside quiet measurement.
Only installed mnt/mnt2 and four nonpaused daemons remain. All owned followers
restored, faults cleared, reader/fault mounts drained; synthetic private stores
removed after clean shutdown. Installed namespace retained, no backwards
compatibility effort or unrelated services touched.

Raw summaries/logs/build/markers outside Git:
`/data1/efs/rdma-fixes-a40428d9d`; native tree `/data1/efs/rdma-devel-a40428d9d`,
fault tree `/data1/efs/rdma-fault-a40428d9d`, installed source `/data1/efs/src`
with full SOURCE_COMMIT. All native, controller, standalone, fixture,
visibility/stress and sanitizer exit markers0. No raw files/binaries committed.

Sanitizer reproduction (after quiet fixtures finish):

```sh
cd /data1/efs/src
export CC='gcc -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined'
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
for t in read_peer_order host_pending_entries inode_pending_eof; do
 python3 tests/test_$t.py || exit
done
```

These instrument production headers/extracted blocks and adapters, not full
live daemon/FUSE instrumentation. No sanitizer diagnostics; all three exit0.
Software RXE on one AMD host remains distinct from native hardware IB/RDMA.
No W106 aggregate/hardware or Intel W100 timeout closure and no push.

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
After qualification, Intel checkpoint `f258cf5d9f27be42d10ecd35795e4957951c27ad`
adds docs and Intel-only sampling/pause controllers. It changes no src/include/
deps/Makefile bytes relative to a404; those new Intel tools are not AMD-tested
gates. Intel peer66/67 and six/five directory timeout findings are preserved in
its [queue guard ledger](intel-queue-guard-20261014.md), not relabelled by AMD
passes. Both owners checkpointed/detached before devel advancement.

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
4G/node core cohorts, peer/cold/EOF fault oracles and quiet/loaded O_EXCL
cached-EOF visibility PASS on frozen a404 source as recorded above.

## Reproduce affected gates

Freeze the merged full source SHA and export a complete tracked tree or the
same source/build/test/doc assets plus exact committed link targets. Record
SOURCE_COMMIT and EFS_GIT_ID/DESCRIBE/BRANCH=devel when building; clean native
build and `make test` first. Separately build fault-enabled FUSE from this exact
source for owned EOF refusal tests only. Drain/stop AMD installed cluster,
install native code and restart with strict RDMA/direct and required disabled
flags. Verify actual process environments, files/direct args and binary hashes.

On AMD, after all builds finish (OUTPUT is a fresh owned path and FAULT_SOURCE names the exact-source fault tree):

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
OUTPUT=$(mktemp -d /data1/efs/rdma-merged-rerun-XXXXXX)
FAULT_SOURCE=/data1/efs/rdma-fault-a40428d9d
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
timeout -k 5 180s python3 tests/live/exclusive_create_visibility.py /data1/efs/mnt --cases 1000 --workers 16
# Separate additional loaded gate, after the quiet fixtures are fully drained:
python3 -u tests/live/rdma_installed_load.py /data1/efs/src "$OUTPUT/exclusive-load" > "$OUTPUT/exclusive-load.log" 2>&1 &
loadpid=$!
if timeout -k 5 180s python3 tests/live/exclusive_create_visibility.py /data1/efs/mnt --cases 1000 --workers 16 > "$OUTPUT/exclusive-loaded.log" 2>&1; then code=0; else code=$?; fi
echo "$code" > "$OUTPUT/exclusive-loaded.exit"
if wait "$loadpid"; then code=0; else code=$?; fi
echo "$code" > "$OUTPUT/exclusive-load.exit"
```

Budgets: read30s/pair5s, paused read outer180/k5; core ordinary15s/decorated75s,
seven distinct clients/jobs16/outer180s; O_EXCL stock30s per case, no fsync/retry
added. Private full fixture uses4 nodes,4G/node, ports27432..27435. Installed
stores17G/node. Drivers always restore paused follower first, drain reader/fault
mounts, stop owned daemons and delete only synthetic stores after clean teardown.
Keep failed markers and raw output outside Git; summary and commands in Git.
Avoid build/profiler/trace alongside quiet measurement. W106 aggregate and Intel
W100/deep-directory obligations remain distinct; no hardware closure implied.
