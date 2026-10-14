# AMD RDMA load deadlines and degraded fragment reads

Historical14d/earlier qualification. Current merged deployment and reruns are
in the [merged AMD ledger](rdma-merged-qualification-20261013.md); prior failures
and passes below remain source-specific evidence.

W106 / [installed deployment baseline](rdma-installed-amd-20261013.md).
Branch `devel-rdma`; current candidate **14d6211822907d9f49f2004e279034f3a135d0ab**.
Status: affected deep/name gates PASS with explicit balancing; full qualification remains PARTIAL.
Earlier18be below remains PARTIAL (two loaded deep/name timeouts), not erased.
No TCP control/comparison, no push.

## Explicit files metadata placement follow-up

07bd without balancing passed1512/1512 in42.507s, then failed1507/1512 in54.894s
and1507/1512 in55.843s (deep/name/mtime15s deadlines). All peer/cold checks still
passed. The disk-barrier improvement alone does not close loaded acceptance.
Both installed and private deployments previously left metadata balancing OFF;
the earlier routing optimization cannot remove remote proof/apply waits when a
group leader hosts only that group. Random leader placement remains a source of
variation, not a passing liveness argument.

**14d621182** makes the existing `EFS_METADATA_BALANCE=1` placement option
independent of the data engine. It prefers one committed voter common to both
metadata groups, so authoritative component reads can execute at that leader.
Unknown membership defers handoff; disjoint groups retain actual-voter placement.
The unchanged2s settling grace, durable catch-up refusal,30s re-election backoff,
quorum and own-apply predicates remain. Default balancing stays **OFF**; this
qualification explicitly enables it on installed AMD and fresh fixtures. Do not
claim that unbalanced/default-OFF load failures disappeared. Containers, packing,
commands/commands-server and expiry remain OFF. No batch-noop/overlap activation.

The files placement oracle fails on07bd and passes on14d, including unknown/
disjoint membership, actual voter selection, lag refusal and backoff. Native read authority/routing and core Raft gates PASS before deployment.

| Final14d, explicit balancing1 | Verdict |
| --- | --- |
| Deep-directory cases under full mixed load | PASS42/42 across three independent fixtures |
| Unusual-name directories under full mixed load | PASS21/21 across three independent fixtures |
| Complete core cohorts (1512 cases each) | 1511/1512 in32.973s;1512/1512 in33.625s;1512/1512 in42.828s |
| Peer / cold persistence | PASS67/67, prepare27/27, verify27/27 in each fixture |
| Files durability / overwrite / GC; core Raft / placement / read authority | PASS |
| Full-common-library ASan/UBSan production durability | PASS; leak checking enabled, no sanitizer diagnostics |
| Preserved10GiB file | PASS, exact baseline digest and length |
| Paused shared follower2,32MiB fresh reader | FAIL180s exit124;16MiB returned before stall; follower restored and reader drained |

One core failure remains: first fixture's `dir_times_unlink` returned EBUSY.
Its cause is not established; neither other passes nor placement settling
justify dropping it. Next reproduce that ordinary unlink under the same mixed
load and inspect its mutation/authority refusal. The paused-node2 read requires
bounded fragment/admission diagnostics: do not borrow the earlier paused-node3
PASS as proof for this different placement/fault. Retain the unchanged180s gate.
These remain W106 blockers. Default balancing-OFF loaded failures also remain
open; the configured target passes do not close unqualified modes or broaderW106.

Installed source/build: full14d SHA above, branchdevel-rdma,
`efsd 14d621182`, built2026-10-14T02:46:31Z (Oct13 evening local).
SHA256: `efsd`9f2d5cdebfb1b217bdeb23659d27937646ef4c5c185b19aaa62756aa5cba1a88;
`efs-fuse`2ca46403aec9d6742b3a798d4125f32f7dd0103bb24f81431755bb2884bb379a;
`efs-mgmt`e60a2a8177f78c391bf64422e0669667b8cd621d26b47eafb3158a0c8d6af511.
Raw14d verdicts stay at `/data1/efs/rdma-fixes-14d621182`, intermediate07bd at
`/data1/efs/rdma-fixes-07bd66e19`. Native build and source export are at the same
paths described below; installed stores stay17G/node, private fixtures4G/node.

Sanitizer reproduction: fresh exact14d export, then the sanitizer flags and
options below, substituting target `tests/test_store_durability` and running
that binary. `/data1/efs/rdma-san-14d621182` instrumented the full common library
and both included production storage units. Runtime daemons remain native.
Native build/gates, from exact exported14d source:

```sh
make -B -j8 EFS_GIT_ID=14d621182 EFS_GIT_DESCRIBE=14d621182 EFS_GIT_BRANCH=devel-rdma efsd efs-fuse efs-mgmt tests/test_store_durability tests/test_store_overwrite tests/test_store_gc tests/test_raft
for t in store_durability store_overwrite store_gc raft; do tests/test_$t || exit; done
for t in metadata_balance namespace_leader_route readindex_pair readindex_result; do python3 tests/test_$t.py || exit; done
```

No TCP control workload.


## Files ancestry barrier follow-up

A30s/99Hz CPU profile of all four private AMD daemons under the same seven-client
core/jobs16 load found31.17% self samples in `native_queued_spin_lock_slowpath`.
The dominant stack was ext4 journal commit waiting from `sync_fragment_parents`
(19.40% of all samples in that stack), with another6.70% through the file barrier.
Metadata diagnostics still show fresh quorum/remote-proof/apply waits; the
profile establishes substantial shared-disk journal interference rather than
proving every timeout is caused by that stack. Diagnostic cohort passed; it is
not quiet acceptance. Raw profile/logs are external at
`/data1/efs/rdma-timeouts-18be5d060` (55K samples, no lost samples).

**07bd66e19** lets initialized files targets reuse the existing bounded pinned
inode ancestry proof (128 directory descriptors across all roots). Each PUT
still fences data/checksum/length and freshly syncs the changed containing
directory. Only previously proven unchanged ancestors may be skipped. A failed
barrier admits no hint; pinned descriptors prevent deleted/recreated inode
reuse; startup/attach seals roots and restart clears runtime hints. Unknown/
uninitialized roots retain full ancestry barriers. Nested files targets match
the longest canonical root. This does not enable containers or cache metadata
read authority; Raft quorum/apply checks and all disabled features stay intact.

`test_store_durability` now uses a real files target and both implementation
units, so fault injection covers the entire barrier path. The new known-ancestry
assertion fails on18be; it passes on07bd with file/directory/close failures,
EINTR/retry, failed unknown ancestry, restart, deletion/recreation, nested root
selection, direct/bounce writes and32 retired worker threads. Real overwrite and
two-root GC/failure/accounting gates also PASS. Test fixtures are local native
files tests, not TCP comparison runs.

| 07bd same-source affected gate | Verdict |
| --- | --- |
| Production durability / overwrite / GC regressions | PASS |
| Quiet07bd without balancing, three independent cohorts | FAIL:1512/1507/1507;10 stock15s deep/name/mtime timeouts |
| 07bd peer67 / prepare27 / cold verify27 per fixture | PASS all three fixtures |
| Sanitized production durability regression | PASS on final14d |

Current acceptance rebuilds/exports exact14d tracked source with the same native flags as below,
changing only build identity to14d621182. Deploy after draining installed clients
and stopping all four daemons. Require `SOURCE_COMMIT` full14d, files/direct,
strict RDMA, commands/commands-server/packing/expiry/repair OFF. The committed
private driver explicitly exports `EFS_METADATA_BALANCE=1`; persist the same
setting in `/data1/efs/devops/clusters/amd.env` for installed AMD. Run the first fixture with
`EFS_GATE_DEGRADED=1` to recheck paused-follower reads under the new placement. Run three
independent fresh fixtures sequentially, retaining each failed verdict:

```sh
for r in 1 2 3; do
 degraded=0; [ "$r" != 1 ] || degraded=1
 EFS_GATE_DEGRADED=$degraded bash tests/live/rdma_four_node_qualification.sh > /data1/efs/rdma-fixes-14d621182/fresh-q$r.log 2>&1
 echo $? > /data1/efs/rdma-fixes-14d621182/fresh-q$r.exit
done
```

Each4G/node fixture keeps the original15s default cases, decorated75s cases and
180s/client outer budget; seven distinct mounts,16 jobs/client. No profiling,
other controller or build during quiet acceptance. Driver drains/stops owned
mounts/daemons before discarding only synthetic stores; logs remain outside Git.

Reproduce the diagnostic separately from acceptance, on an exclusive deployed
fixture. Copy the driver outside Git, keep its exact workload/budgets and set
only observation flags (the committed quiet driver deliberately unsets them):

```sh
python3 - <<'PYDIAG'
from pathlib import Path
s=Path('/data1/efs/src/tests/live/rdma_four_node_qualification.sh').read_text()
s=s.replace('src=$(cd "$(dirname "$0")/../.." && pwd)', 'src=/data1/efs/src')
s=s.replace('unset EFS_READ_PROF EFS_RPC_PROF EFS_CONTAINER_PATH_PROF EFS_READ_FRAG_PROF',
            'unset EFS_CONTAINER_PATH_PROF EFS_READ_FRAG_PROF\nexport EFS_READ_PROF=1 EFS_RPC_PROF=1 EFS_RAFT_OBS=1 EFS_BATCH_OBS=1')
Path('/data1/efs/rdma-diagnostic.sh').write_text(s)
PYDIAG
EFS_GATE_DEGRADED=0 bash /data1/efs/rdma-diagnostic.sh > /data1/efs/rdma-diagnostic.log 2>&1
```

In a second terminal, once the driver's first-line artifact directory contains
all four PID files and `load1/c0.log`, record only those owned daemon PIDs:

```sh
work=$(head -1 /data1/efs/rdma-diagnostic.log)
perf record -F 99 -g -p "$(cat "$work"/efsd-n?.pid | paste -sd,)" -o /data1/efs/rdma-diagnostic-perf.data -- sleep 30
perf report --stdio --no-children --percent-limit 1 -i /data1/efs/rdma-diagnostic-perf.data
```

Let the driver finish its original gates and cleanup. Observation overhead makes
this diagnostic-only. Then run the unmodified driver quietly for acceptance.

The older18be evidence/provenance below is historical and remains traceable.

## Fixes and demonstrated causes

- **715656fc:** a fragment-pair poll wake was treated as an application reply.
  Segmented RDMA CREDIT/partial-frame wakes can arrive first; blocking receive
  on the paused peer hid the healthy reply until the pair deadline. Poll now
  returns to complete-message readiness checks for both owners. Empty physical
  rings drain stale eventfd wakes, with arm-and-recheck preserving raced pushes.
  Completed receive/send progress no longer enters an unnecessary timed wait.
- **300f5192 / 7b753755:** sequential connection admission could consume all5s
  before the second request, or while an already-sent healthy reply waited.
  Reserve admission/reply time in **both** positions within the same original
  five-second pair deadline; restore the enclosing TLS scope on every path.
  No peer-health cooldown is invented for a local admission cancellation.
- **cc1cf8cc:** ordinary files bypassed pre-probe admission tracking, making
  queued readers demand a later proof even when admitted before the current
  probe. Use the same bounded tickets for files and containers. Only tickets
  registered before a probe watermark may use its round; later arrivals,
  changed terms and saturation retain refusal/new-proof behavior. Mixed local/
  remote pairs also track their locally started round. Own applied-index checks,
  fresh authority and zero FUSE entry/attribute cache timeouts remain intact.
  This does not enable containers or reuse completed authority for later RPCs.

New regressions failed on the corresponding old production blocks before the
fixes: `test_get_pair_readiness.py` covers CREDIT-only wakes and a cold stalled
peer in either checkout position; `test_readindex_tickets.py` covers ordinary
files, pre-probe cohorts, late admission, term change and counter saturation.
`test_rdma_progress_ready.py` exercises completed progress versus waiting and
empty/nonempty ring draining. Make gates now include these regressions.

`2109eea0` adds bounded `EFS_READ_FRAG_PROF=1` diagnostics (first64 insufficient
fragment outcomes), OFF by default. It showed a healthy node4 and paused node3
both timing out, with only the fallback fragment valid. That observation led
specifically to the second-checkout reservation fix, rather than weakening
checksum/length validation or increasing deadlines.

- **5bc313ca:** ordinary files also bypassed queued fresh-vote progress and
  separate read-control scheduling. Enable the existing bounded control lane
  for files too. Empty proof probes retain term/boot/voter/durable-prefix checks;
  they grant no replication/window credit. Original entry packets, ordered
  receive work and progress-bearing ACKs are retained. Core refusal, exact FIFO/
  credits and duplicate-progress negative controls PASS for both engines.

The ReadIndex result test had an early return before delayed-floor/context
assertions. `af05aadc` makes those assertions run; they PASS. Host-flow test's
old physical-receive stub was updated to the negotiated logical-message cap;
native Linux asynchronous sender/control/flow checks PASS on final source.

- **b27a2e99:** complete ordinary-file prefix-zero probe retirement on the
  inline fast path, and track publication-started probe watermarks. Publication
  proof/commit/apply semantics are unchanged.
- **402862eef:** pre-admitted readers were starting redundant probes after
  their already eligible probe completed. Suppress a new probe only when an
  eligible current/retained certificate covers that exact admission and index;
  own apply remains separately mandatory. Already satisfied state is published
  immediately. A fast-completed pre-mutex cohort regression fails on old code;
  removing the late-admission round guard fails the negative control. Threaded
  ticket, term/config/role/storage, coherent publish and apply regressions PASS.

- **18be5d060:** ordinary LOOKUP/GETATTR/batched-path reads bypassed the
  existing suitable-leader route. A402 read-phase diagnostic on a dual
  host measured42,381 forwarded proofs averaging1.511ms, plus1.634ms local
  apply waits; leader-local requests averaged0.809ms on the other leader.
  These are cumulative diagnostic averages, not performance acceptance.
  Route files to a known leader only when its committed membership hosts both
  groups, before reading rows. Unknown/single-group/self/removed leaders keep
  fallback; recipient still obtains fresh quorum and own-apply authority.
  The files routing regression fails on402 and passes with this change;
  replacement, bounded path authority and ReadIndex negatives also PASS.

## Historical18be environment and provenance

AMD-395 / Ubuntu, same-host Soft-RoCE rxe0, GID1 over wlp194s0. Installed four
nodes127.0.0.1:17432–17435; files engine/direct I/O, retained **17G quota/node**.
Original10GiB file and tiny-files namespace retained. Every daemon/client strict
`EFS_TRANSPORT=rdma`; `.efs` client/server, packing, containers, expiry and repair
experiment OFF. Existing filesystem reserve checks unchanged. Test source lives
at `/data1/efs/src`, native build at `/data1/efs/rdma-deploy-src`.

Final test placement: installed cluster retains17G/node and user namespace;
independent private qualification uses four **fresh4G** stores at ports27432–27435
per cohort. Both have GC admission1/backlog idle20ms. No giant payload from a
previous cohort is carried into a new functional acceptance run. The installed
capacity-stress failures remain separately recorded, not converted to passes.

Installed binary SHA256 values:

- `efsd`: `c82ec84c9dda62a7bffdcb4d066543c0a3a4a5c8c4b6dce09e6e06e6fbf0e824`
- `efs-fuse`: `190274eaa63f103bb78b1e4a77f77021dd27696c66e901ff71bd22182e936deb`
- `efs-mgmt`: `b242687caf50fcbb2b973359c90ec5ecc610276745919532af617f9f3f04fc76`

Full exported tracked build/test source was rebuilt:

```sh
make -B -j8 EFS_GIT_ID=18be5d060 EFS_GIT_DESCRIBE=18be5d060 EFS_GIT_BRANCH=devel-rdma efsd efs-fuse efs-mgmt tests/test_rdma_segmented
```

Final build time2026-10-14T01:25:30Z (Oct13 evening in the user's timezone).
Installed runtime `SOURCE_COMMIT` contains full18be SHA. Helpers are committed:
`94cdfbb6` seven-client load; `c6cd527f` fresh-mount degraded read, both included
in the final candidate. Private driver `660d35db` runs one load/peer/cold cohort
per fresh store set and optionally the degraded gate; cleanup stops its owned
processes before discarding only those synthetic stores, preserving logs/verdicts. Intermediate runtime2109 source marker was abbreviated;
its full source is2109eea0cb79bf57b2bb27d17db40d610cbbf4ac.

Full-common-library sanitizer build in a separate fresh exported source tree
(`/data1/efs/rdma-san-18be5d060`), after all quiet live fixtures stopped:

```sh
make -B -j8 CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -fsanitize=address,undefined' LDFLAGS='-lpthread -lm -ldl -libverbs -fsanitize=address,undefined' tests/test_rdma_segmented
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
for mode in normal interrupt tcp-violation deadline churn; do timeout 45s tests/test_rdma_segmented "$mode" || exit; done
```

Sanitizer test-only marker isunknown; inputs are the exact18be tracked source.
Instrumentation covers the whole common library, not running server/FUSE
handlers. All modes explicitly report PASS, not unavailable-device SKIP.
Final process audit: all four installed daemons alive/not paused, only original
`mnt`/`mnt2` FUSE mounts remain, all owned load/fault/trace controllers gone.
Each installed daemon uses files/direct, strict RDMA and the requested OFF flags;
both installed clients also retain OFF flags. Original10GiB digest matches
`7aa0c22312679872fd8771e222398ee44be40c60ece4ded8e76896c9ef02625b`.

## Historical18be gate ledger

| Gate | Verdict |
| --- | --- |
| Mac and Linux focused fragment/deadline/progress regressions (715/402; unchanged blocks in18be) | PASS |
| ReadIndex pair/admission/result gates on18be; coalescer/waiter gates on402 unchanged in18be | PASS; paired-floor ASan/UBSan and stale-floor negative control also PASS |
| Final18be native Soft-RoCE normal/interrupt/illegal-byte/deadline/churn | PASS, all five original45s gates |
| 715 quiet seven-client core | FAIL1508/1512, four15s deep-directory timeouts; before ordinary-file admission fix |
| 715 /300 /210 fresh-reader degraded32MiB | FAIL EIO; before complete two-position admission fix |
| Intermediatecc1 degraded32MiB with follower paused | PASS all256 deterministic128KiB blocks,90.36s, original180s outer |
| Final18be degraded32MiB | PASS:256/256 deterministic128KiB blocks; four8MiB reads,80.44s total, original180s outer |
| Intermediate660 quiet fresh4G core | First1512/1512 PASS; second1511/1512 with one unusual-name15s timeout, not accepted |
| Intermediateb27 quiet fresh4G core | First two1512/1512 PASS; third1503/1512 with metadata BUSY/deep/name/mtime failures, not accepted |
| Intermediate402 quiet fresh4G core | First1512/1512 in56.283s, second1512/1512 in52.114s; third1504/1512 FAIL, eight stock15s deep/name/mtime timeouts |
| Final18be quiet fresh4G core | First1512/1512 PASS56.209s; second1510/1512 FAIL53.359s (one client deep/name15s); third1512/1512 PASS53.329s |
| Final18be peer/cold persistence | PASS in all three fixtures: peer67/67, prepare27/27 and verify27/27 |
| Final18be installed targeted load | PASS across seven clients: deep14/14 in2.025s; names7/7 in1.214s |
| Final18be preserved10GiB digest | PASS:10,737,418,240bytes match baseline SHA256 |
| Final18be full-common-library ASan/UBSan transport gates | PASS all five modes; no sanitizer diagnostics |
| Prior snapshot-pressure failure / actual import | Still open; not closed by these affected fixes |
| Hardware cross-host IB | NOTRUN |

The final18be degraded digest is
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`,
exactly33,554,432bytes. A fresh client read with shared follower3 SIGSTOP;
all four8MiB reads verified,20.08–20.15s each. Correct fallback is still slow;
no performance closure is claimed. Follower SIGCONT and owned cleanup completed.

The second18be cohort's failing client created its deep chain in11.13s,
reached cleanup at11.85s, then exhausted the stock15s. Its unusual-name case
reached removal at14.73s. These are elapsed phase observations, not proof of a
new authority bug. Both failures remain recorded; no timeout/cache TTL changed.

The load baseline's phase datagrams located creation near13.4–14.2s for failing
clients, leaving insufficient time for recursive cleanup within the stock15s.
Passing clients created in5.7–6.5s and completed the whole case in8.7–10.3s.
The instrumented diagnostic recorded thousands of authoritative LOOKUP callbacks
per deep case; one observed worker spent11.8s in2262LOOKUPs. No cache freshness
requirement was relaxed to reduce these waits.

A30s `strace -f -ttt -T` attached all four daemons and seven owned FUSE clients,
recording poll/futex/read/write/fsync/fdatasync/send/receive waits. It retained
108MiB externally. Attachment/ptrace substantially perturbed the load; all seven
suites had failures, some notrun. **Diagnostic only, not acceptance evidence.**
Timeout sent SIGINT; its5s detach allowance ended in137. Subsequent process
inspection found no remaining strace, and all owned clients were drained.

## Historical18be remaining work / W106 stays open

Loaded acceptance remains **FAIL**: two of4,536 final core executions exhausted
15s in the second cohort. Neither passing repetitions nor isolated passes erase
that verdict. Next isolate the failing worker's authoritative path under the
same mixed seven-client/jobs16 load, separating leader quorum wait, remote
proof, own apply, transaction commit and path traversal. Reduce the demonstrated
critical path and rerun prospective independent quiet cohorts at the same
15/75/180s deadlines; do not substitute filtered passes for loaded acceptance.

Degraded-read correctness is fixed; its80s/32MiB latency remains follow-up work.
Sustained store reuse/GC admission, prior snapshot-pressure630s failure and actual
snapshot import (NOTRUN), paused-teardown byte-accounting ambiguity, and hardware
cross-host RDMA remain open. This page does not close broader W106 or claim GPU
memory DMA. No TCP control runs were used.

## Capacity-stress distinction

Retained17G load oncc1 passed1505/1512: one15s deep timeout and six close/flush
EIOs (three clients' `perm_dir_no_x_search` and `err_chdir_into_file`). Three
synthetic mounts refused clean drain. Actual daemon logs show GC admission
rejecting PUTs around16.6GB used/18.25GB quota (>90%) during recent deletions;
a later81.9% status sample reflected reclamation, not the load's peak. Safeguards
were not bypassed. Only those three owned failed mounts were force-discarded,
using standing authorization to discard AMD synthetic data. User mounts/data
were retained.

On a fresh4Gcc1 fixture, the first1512-case cohort PASSED. Immediately reusing
that store set failed later cohorts: PUT admission at3,866MB/4,295MB coincided
with deep/name/mtime timeouts. Peer67/67, cold27/27 and degraded32MiB still passed
there. This remains **FAIL capacity/reclamation stress**, not passing RDMA load
acceptance. Failed physical test stores were subsequently discarded only after
all owned daemons stopped; their external summaries/logs remain.

Independent functional cohorts now each begin with fresh4G stores. This neither
raises quota/timeouts nor disables admission, and it matches the intended4G
qualification fixture. No performance comparison between retained17G and fresh4G
is claimed. Sustained reuse/GC-pressure liveness remains a separate open issue.

## Historical18be affected reproduction (export18be source first)

Exclusive AMD access; sufficient filesystem reserve; all four nodes healthy.
Export/build the candidate, drain both installed clients, stop daemons, install
source/binaries, restart through `EFS_CLUSTER=amd /data1/efs/devops/start.sh`.
Do not wipe installed/user stores. Inspect `/proc/PID/environ` and daemon args
for strict RDMA, files/direct and all disabled flags before starting.

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
export EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0
for mode in normal interrupt tcp-violation deadline churn; do timeout 45s /data1/efs/rdma-deploy-src/tests/test_rdma_segmented "$mode" || exit; done
# tcp-violation injects a forbidden byte after RDMA setup; it is not a TCP comparison.
for t in get_pair_readiness get_pair_budget rdma_progress_ready rdma_reply_budget readindex_tickets readindex_pair readindex_admission readindex_result readindex_coalescer read_waiters; do python3 tests/test_$t.py || exit; done
python3 -u tests/live/rdma_degraded_read.py /data1/efs/src /data1/efs/mnt /data1/efs/rdma-fixes-18be5d060/degraded1
python3 -u tests/live/rdma_installed_load.py /data1/efs/src /data1/efs/rdma-fixes-18be5d060/load1
```

Installed affected-only seven-client reruns (quiet, after private fixtures):

```sh
python3 -u tests/live/rdma_installed_load.py /data1/efs/src OUT/deep --filter dir_deep_nesting
python3 -u tests/live/rdma_installed_load.py /data1/efs/src OUT/names --filter names_crazy_dirs
```

Use a new output directory per repetition (`load2`, `load3`, `degraded2`).
Degraded gate writes/fsyncs a new32MiB deterministic file, mounts a fresh reader
with no data-cache warming, pauses a verified shared FOLLOWER, verifies every
128KiB block via8MiB reads, and restores it in `finally`. Outer180s and existing
30s read/5s pair budgets are unchanged. It deletes only its exact owned file/
directory after restoration and cleanly drains its reader mount.

Load helper mounts seven distinct clients under permitted `/home/efs`, starts
core216 cases/jobs16 per client with distinct tags, default case budgets
(including deep-directory15s) and original outer180s. Nonblocking phase datagrams
are retained up to4096; missing records are unknown. Cleanup drains every owned
mount; require complete result/exit and cleanup markers. Quiet acceptance has no
strace, profiler or parallel build. Do not combine fault injection with that run.

Peer rerun: `python3 tests/posix/posix_2client.py --local /data1/efs/mnt
/data1/efs/mnt2 --parent rdma-fixed-peer --results OUT/peer.tsv`.
Cold persistence follows baseline prepare/all-client+daemon stop/start/verify
commands, stock60s cases. Full10GiB digest reproduction is in the baseline page;
it must match the recorded digest and exact length. No cached timing speedup claim.

Final independent cohorts (serial; default first also runs degraded32MiB):

```sh
bash tests/live/rdma_four_node_qualification.sh > /data1/efs/rdma-fixes-18be5d060/fresh-q1.log 2>&1
EFS_GATE_DEGRADED=0 bash tests/live/rdma_four_node_qualification.sh > /data1/efs/rdma-fixes-18be5d060/fresh-q2.log 2>&1
EFS_GATE_DEGRADED=0 bash tests/live/rdma_four_node_qualification.sh > /data1/efs/rdma-fixes-18be5d060/fresh-q3.log 2>&1
```

The402 intermediate also passed peer67/67 and prepare/verify27/27 in all
three fixtures; its paused-follower32MiB gate passed in the first. The failed
third core cohort had no GC-admission errors. Its diagnostic repetition with
read profiling passed but is not quiet acceptance. Daemon FD limits were
already524288; no EMFILE evidence justified changing resource limits.

Every wrapper writes its private artifact directory as the first log line;
accept only exit-code0, all seven TSVs216/216, clean helper cleanup, peer67/67 and
prepare/verify27/27. Source remains frozen18be throughout. No builds/profilers
run alongside these measurements. Disposable stores are removed only after
owned clients and daemons stop; never point this driver at installed stores.

Raw failures, logs, phase dumps and diagnostic captures remain outside Git:
`/data1/efs/rdma-fixes-{715656fc,300f5192,2109eea0,cc1cf8cc,660d35db,b27a2e99,402862eef,18be5d060}`.
Commit only this summary, clear reproduction scripts and verdict/index updates.
