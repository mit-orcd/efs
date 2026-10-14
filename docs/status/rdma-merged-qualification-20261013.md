# AMD merged W100 / maintenance / RDMA qualification

W100 / W106. Branch `devel-rdma`; frozen candidate
**fc2460b191c30cdccd2346b25433d786b52bbc1a**. Status: PARTIAL; integrated/tested, two loaded mutation refusals remain.
Normal native build timestamp2026-10-14T03:26:40Z; fault client03:28:55Z,
samefc source/build identity, branchdevel-rdma. Installed SHA256:

- efsd: `edc3e83721c3420e2ef84fa2f5d4a4aa51692f492e73ca513ec8c839f1b27ba4`
- efs-fuse (normal): `f276bb5f0588dc74423fe6e0c6584c8ef22c5e6ef14f89ab9bae24936ed932ea`
- efs-mgmt: `266275482a111593c106dd2d216a7a157b0afe28dfa6b0c13c9e20ba62a4f29a`
- private fault efs-fuse: `50f2f34b0140a74da3b6a72bd2a3b7ac0647cd5107ad01596b8333115fd75045`

No push; no TCP comparison. `.efs` client/server, packing, containers, expiry
and repair experiment OFF. Files/direct, strict segmented RDMA, Soft-RoCE
rxe0/GID1 on same-host AMD-395. Explicit metadata balancing1 retained.

## Integration

Merged exactly the reviewed endpoints:

- `devel` dcb114b9d:1302ec58b,c08bbe67a,c3d8ee560,384b3eb2f,
  842a4c3d9,b872bdc29,dcb114b9d (merge5bdcdc96f).
- `devel-orcd-package1`5e27a497c:1a2234a13,e8bab2101,881e7ee77,
  5e27a497c (merge85fee1b7a).

Production changes merged without textual conflicts. Status/index entries from
both parents were retained; conflicting generated architecture documents were
regenerated and all five doc gates passed. Earlier RDMA readiness, fragment
admission, fresh-read/control, files ancestry and opt-in placement fixes remain.
No defaults were enabled for containers, query, expiry or repair.

The W100 series must remain together: missing-size live records are rejected,
explicit EOF0 remains valid, missing staged EOF uses bounded read-only repair,
and a stale remote EOF must never be adopted into the next retry snapshot.
The native two-attempt oracle PASSes; oldc3d8ee560 fails the exact intended
second-attempt assertion. Missing original accepted EOF is still an open W100
problem, not repaired by refusing unsafe publication.

Maintenance scans count merged keys including hidden tombstones, stop after
4096 work keys and carry their inclusive continuation. Callback refusals retain
their own verdict; partial pages do not imply exhaustion or deletion permission.
Finite ORCD gates passed but sustained close EIO recurred; both findings remain
in the imported [ORCD ledger](orcd-close-eio-20261013.md). Imported Intel evidence
is historical TCP/direct qualification, not a new comparison or an AMD RDMA pass;
its30s mounted cases do not qualify AMD's stock15s cases.

## Added qualification support

`2eee93527`: existing EOF oracle accepts `--peer-mount`, using an independent
local FUSE client instead of SSH. The private RDMA driver may receive
`EFS_GATE_EOF_SOURCE` pointing to a separately built fault-enabled client from
the same source/build ID. Only that owned mount enables the omission hook;
installed clients stay fault-OFF. Size omission after valid snapshot construction
must refuse two fsyncs, peer sees EOF0, clearing the hook allows exact recovery.
512-byte payload and sparse low/high anchors+EOF are checked; sparse anchors do
not constitute a full-hole hash or spontaneous staged-row eviction.

Initial full native2eee run stopped at the stale embedded read-verifier harness,
which omitted `read_frag_prof` used by the extracted production initializer.
`ddf43f2f7` supplies that state and keeps diagnostics OFF in the verifier. The
next export omitted rootREADME.md; that run stopped at a docs test. Export was
completed from the same frozen commit; neither failed attempt counts as PASS.
The documentation link check additionally needed473 exact committed historical
link targets (6.6MiB), exported without placeholders or large raw bundles.
A remaining-recipes audit ran174 native commands; its only failure was the
packed-storage fault harness, which had stopped intercepting the now-shared
ancestry directory barrier. `fc2460b19` includes both implementation units under
the fault hooks, keeping all barrier/error/retry assertions. It also replaces
an obsolete physical-repost stub in the standalone frame watcher with logical
message release. These are harness repairs, not changes to production behavior.
The packed fault harness now PASSes; full native is rerun on the final frozen
source. Raw failed2eee/ddf attempts remain outside Git, never counted as PASS.

## Gates and remaining work

| Exact merged candidate gate | Verdict |
| --- | --- |
| Full AMD native make test | PASS on finalfc; complete exit0, all recipes |
| W100 two-attempt native / old repair negative | PASS locally |
| Merged KV tombstone work / close continuation | PASS in full native |
| Three quiet seven-client core cohorts |1510/1512 in33.297s;1512/1512 in34.343s;1512/1512 in43.504s |
| Deep / unusual-name cases within those full cohorts | PASS42/42 deep and21/21 names; no timeout |
| Peer67 and cold prepare/verify27 per cohort | PASS all three:201 peer,81 prepare,81 verify checks |
| W100 fault refusal/recovery via separate RDMA clients | PASS all three:6 cases,12 refused fsyncs and6 exact retries |
| Paused-follower32MiB, original180s outer | PASS: follower2; every256×128KiB block verified,87.847s total read |
| Targeted ASan/UBSan KV/close/retry/durability | PASS all five; leak checking, no sanitizer diagnostics |
| Preserved10GiB digest | PASS:10,737,418,240bytes, exact baseline SHA256 |

The first full core fixture retains two failures, both on client3:
`dir_times_unlink` and `dir_times_link` returned EBUSY. Other two cohorts pass;
that does not erase the first failure or establish sustained-load closure.
No CPU/trace collection ran alongside acceptance. W100 target refusal/retry
passes; aggregate lost-original-EOF/staged-row eviction acceptance remains open.
W106 full loaded acceptance remains FAIL/PARTIAL pending these mutation refusals.
Next reproduce the same mixed load at unchanged budgets, correlate the exact
namespace/authority/transaction refusal and rerun affected quiet gates.

The earlier paused-node2 read stalled at16MiB/180s on14d. This merged candidate
passed that same follower identity and unchanged byte/budget oracle. The old
failure remains traceable; this does not attribute the improvement solely to
one of the two integrated fixes. Fresh-read latency is still20.09–22.59s per8MiB
buffer. SHA256 of all33,554,432bytes is
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.
Retained10GiB digest is
`7aa0c22312679872fd8771e222398ee44be40c60ece4ded8e76896c9ef02625b`.
All fault files cleared, follower SIGCONT completed and every owned fixture,
reader, fault mount and daemon stopped. Only installed `mnt`/`mnt2` remain;
all four native installed daemons are alive/not paused with the required flags.
No store data or raw capture is committed. No ORCD/Intel/NUC actions.

Prior [14d AMD ledger](rdma-load-fallback-fixes-20261013.md) remains PARTIAL:
deep42/42 and names21/21 passed with balancing1, but one unlinkEBUSY and a
paused-node2 read stalled at16MiB/180s. Balancing-OFF failures, sustained ORCD
close failure, hardware RDMA and broader W100/W106 obligations remain open.
Do not borrow those old passes for this merged candidate.

## Sanitizer reproduction

After quiet fixtures stop, export the same source to a fresh separate tree
(`/data1/efs/rdma-san-fc2460b19`), then:

```sh
make -j8 CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -fsanitize=address,undefined' LDFLAGS='-lpthread -lm -ldl -libverbs -fsanitize=address,undefined' tests/test_kv_lsm tests/test_store_durability
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
./tests/test_kv_lsm
./tests/test_store_durability
export CC='gcc -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined'
for t in close_retire_fastpath report_retry_eof report_zero_eof; do python3 tests/test_$t.py || exit; done
```

KV/full common library and included storage implementations are instrumented;
close/EOF tests execute extracted production blocks with adapters. Test-only
sanitizer marker isunknown; exact exportedfc source governs. These are not
sanitized live daemon/FUSE or hardware-power-loss qualifications.

## Reproduction and identity

Export exact frozen tracked source including Makefile/src/include/deps/tests/
scripts/tools/docs and rootREADME/LICENSE/.gitignore/efs-bench.sh. Include exact
committed targets for historical doc links if running the full docs gate;
there are no generated placeholder targets. A complete detached checkout or
full `git archive fc2460b191c30cdccd2346b25433d786b52bbc1a` also supplies all
required assets; reproduction needs no external private run capture. The documentation-only historical
assets are unchanged between ddf and finalfc. Native tree
`/data1/efs/rdma-merged-fc2460b19`, separate fault-client tree
`/data1/efs/rdma-fault-fc2460b19`, each SOURCE_COMMIT fullSHA above.

```sh
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
make -j8 EFS_GIT_ID=fc2460b19 EFS_GIT_DESCRIBE=fc2460b19 EFS_GIT_BRANCH=devel-rdma efsd efs-fuse efs-mgmt test
# In separate fresh exported fault tree, same identity:
make -j8 EFS_FAULTS=1 EFS_GIT_ID=fc2460b19 EFS_GIT_DESCRIBE=fc2460b19 EFS_GIT_BRANCH=devel-rdma efs-fuse
```

After native gates pass, drain installed clients, stop the four installed
daemons, install normal binaries/source, retain fullSOURCE_COMMIT, restart via
`EFS_CLUSTER=amd /data1/efs/devops/start.sh`. Verify every actual process and
files/direct args with all requested flags OFF and balancing1. Installed stores
17G/node and original user namespace retained. Private fixtures are fresh4G/node,
four nodes at27432–27435 and independent mounts under `/home/efs`.

```sh
cd /data1/efs/src
mkdir -p /data1/efs/rdma-fixes-fc2460b19
for r in 1 2 3; do
 degraded=0; [ "$r" != 1 ] || degraded=1
 EFS_GATE_DEGRADED=$degraded EFS_GATE_EOF_SOURCE=/data1/efs/rdma-fault-fc2460b19 bash tests/live/rdma_four_node_qualification.sh > /data1/efs/rdma-fixes-fc2460b19/fresh-q$r.log 2>&1
 echo $? > /data1/efs/rdma-fixes-fc2460b19/fresh-q$r.exit
done
```

Unchanged core216 cases/jobs16 on each of seven distinct clients: stock15s,
decorated75s, outer180s. No profiler, tracing, controller or build alongside
quiet measurement. Peer/persistence budgets remain stock. Fault oracle uses
its unchanged client syncREPORT8s/FUSE budgets, bounded by180s overall; paused
read keeps existing30s read/5s pair and180s outer. No timeouts/window widened.
Driver clears faults, restores paused followers, drains mounts/stops owned
nodes before discarding only synthetic stores; preserve failed/log markers.
Cleanup failure retains owned fixtures and must be resolved before layering.
All raw outputs are outside Git at `/data1/efs/rdma-fixes-fc2460b19`; summarize
verdicts and precise reproductions, never commit captures/binaries/stores.
