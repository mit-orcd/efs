# AMD RDMA load deadlines and degraded fragment reads

W106 / [installed deployment baseline](rdma-installed-amd-20261013.md).
Branch `devel-rdma`; candidate **18be5d0601b0e95a0cb9fb744c5d02250d901105**.
Status: PARTIAL; degraded-read correctness PASS, loaded deadlines remain FAIL. No TCP control/comparison, no push. Earliercc1/660/b27/402 are intermediate candidates, not the final verdict.

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

## Environment and provenance

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

## Gate ledger

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

## Remaining work / W106 stays open

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

## Exact affected reproduction

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
