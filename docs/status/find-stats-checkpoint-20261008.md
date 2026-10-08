# W92 find/stats continuation checkpoint — Oct 8, 01:53 EDT

This work is **in progress, not accepted**. `.stats` still reports zero for
peer-created files. Server-owned rollups and query serving have not been integrated. Offline snapshot
and name-index components are implemented below.
Legacy `.find` still walks on query misses and errors at its explicit limits.
Do not close the task on the strength of ordinary POSIX smoke passes.

## Source and deployment

- Branch `devel-find-stats`, isolated checkout
  `/Users/mike/Documents/Codex/2026-10-07/i-l/work/efs-find-stats`.
- Forked committed `devel` e86b85ac. Main `/Users/mike/git/efs` has concurrent
  unrelated edits; preserve it. Its docs now assign W90 expiry and W91 `.efs`;
  this branch's find/stats work is W92. Historical w90 paths remain unchanged.
- Latest clean deployed binary build: `e9f974b4e148`, NUC four nodes identical,
  TCP, buffered I/O. AMD unchanged. Later harness/docs commits do not require
  redeploying unchanged binaries.
- Commits d078711b, 4f3aa3b0, 467244c3, e9f974b4 fix immutable open bytes/size,
  permissions/errors, versioned inode identity, bounded memory/query admission
  and kernel lookup-reference recovery. They do not implement server stats.
- Latest POSIX: 216 pass / 0 fail / 1 skip in 56.5 s. Peer: 64/64 in 50.3 s.
  Raw TSVs in results/measure/20261008-w90. Focused recovery (stat-only queries,
  sequential opens and repeated exhaustion/drain), snapshot, invalid-query and
  permission gates passed before these full suites. stats_mutations baseline
  deliberately reproduces zero counters; retained raw evidence is a FAIL.

## Running population

SSH `nuc_efs`; base `/data1/efs`, mounts `mnt` and `mnt2`. Existing ordinary data
(~20.38 GiB logical) is preserved. The loader writes only `mnt/w90-scale`:
131,072 files of 16 KiB under 128 partitions and 16 branches, then 64-MiB bulk
files. Stages add 16, 64, 128, 256 and finally 380 GiB logical, toward ~400 total.
Eight workers; directory creation cached within each stage. Files are fsynced,
renamed, and recorded in completed.jsonl with deterministic payload hashes.
Physical free-space and inode reserves are checked during each file write.

Control: `/data1/efs/w90-control/{loader.pid,loader.lock,overnight.log,completed.jsonl,stage-complete}`.
At this checkpoint the resumed loader PID is 2388751; always verify current PID
and command, not the historical number. A controlled pause and resume optimized
ancestor mkdir work; earlier InterruptedError in the log is that intentional
pause. The original pre-repair ENOMEM abort is also retained in the log.

Inspect process/log/space before any cluster changes. To pause safely:

```sh
ssh nuc_efs 'touch /data1/efs/w90-control/PAUSE'
```

Wait for loader and its Python workers to exit/drain. Never deploy/restart
with active population, discard pending writes, wipe stores or delete baseline
files. Resume only after confirming the pause has drained:

```sh
ssh nuc_efs 'rm /data1/efs/w90-control/PAUSE; nohup flock -n /data1/efs/w90-control/loader.lock bash /data1/efs/w90-load-stages.sh >>/data1/efs/w90-control/overnight.log 2>&1 </dev/null & echo $! >/data1/efs/w90-control/loader.pid'
```

`/data2/efs` resolves onto /home storage; inspect actual `n3`/`n4` paths with df,
not `/data2` alone. `/data1` starts ~702 GiB free; /home ~805 GiB. Each must keep
50 GiB and 100,000 free inodes. Existing files skipped on resume are size-checked;
final acceptance must independently verify deterministic payload checksums.

## Next required implementation

Follow virtual-find-stats.md server architecture and mandatory gates. In
particular, do not replace zero stats with a client walk. Relevant integration
points:

- server/raft_host.c host_apply dispatch, groups 0 and 2, shared KV with 4096
  shard prefixes. Not every server hosts every group. Preserve applied vectors.
- meta/meta_apply.c meta_write_batch/put/del covers ordinary mutations, but
  txn.c resolve writes its own atomic batches: hooking only ordinary mutations
  misses cross-shard rename/link. Secondary state must follow committed resolved
  images, survive restart/import, and never publish a half transaction.
- Inode base_size alone is insufficient; committed lane publications determine
  logical size. Directory dentry layout can spread over 64 lanes. Hard-link
  statistics count directory entries, including each name's size contribution.
- Pinned LSM views currently cover segment sets and require a memtable flush;
  they are not an off-the-shelf full KV snapshot including live memtables. Avoid
  doing a whole namespace scan under the Raft pump or global client lock.
- QUERY_STATS (25/26) currently answers unsupported. GET_FEATURES remains, but
  SET_FEATURES is retired. Use a versioned bounded query RPC and update current
  toggle acceptance wording rather than restoring retired persistence blindly.
- Refresh should be bounded background work on N namespace changes or T time;
  rename-only workloads must converge. Expose separate stats/index times and
  source watermarks; failed work must not advance them. Query/output limits must
  return explicit errors, not truncated successful results.

After server integration: cold two-mount stats_mutations/freshness gates,
large-tree targeted searches with measured node/RPC work, repeated cat/stat
without rebuilds, permissions/chmod, simultaneous opens, output limits,
transport/leader/restart faults, data validation, full POSIX/peer. Update the
status entry and retain exact source/build/evidence before declaring success.


## Oct 8, 02:12 EDT — query foundations implemented offline

Two focused commits extend this branch; neither is deployed to running NUC:

- `3ecfec32`: full applied KV snapshots, including copied live memtable updates
  and tombstones, pinned atomically with the segment set. Snapshot scans/gets
  bypass the live mutable point cache while sharing immutable sparse indexes;
  they take no live KV lock after pin and support concurrent readers and
  callback re-entry. A caller-supplied memtable budget refuses excess work with
  BUSY; poisoned/half-applied engines refuse snapshots with IO. Pin still copies
  the bounded memtable under the KV mutex; its tail latency must be measured.
- Process-local per-shard metadata dirty hints increment in the LSM batch apply
  path, covering ordinary mutations, transaction resolve and snapshot import.
  Payload chunk keys are excluded. Snapshot hints are captured with its data.
  They count relevant applied key writes, not user operations or Raft commits,
  and reset on engine open. Rebuild once per server boot; never expose these
  hints as authoritative cross-group watermarks or proof of an empty shard.
- `5d7e1175`: immutable shard-local name candidate indexes (`name_index.h/c`).
  Exact/prefix queries seek a sorted name range; suffix/contains scans have
  explicit visit budgets and resumable cursors. Candidate callbacks can fail
  explicitly and retry the same entry. Memory limits fail explicitly; a partial
  build is freed. Hard-link names and raw UTF8/newline names are retained.
  Dentry tombstones remain candidates so directory layout precedence can be
  resolved correctly. A pending intent/guard/reduction refuses the new shard
  build with BUSY. That local check alone does NOT prove cross-group assembly.

NUC full KV suite and new name-index unit suite pass in isolated
`/data1/efs/w92-build`; no cluster files, stores or deployed binaries were
replaced. Focused new snapshot and name-index tests also pass Clang AddressSanitizer
and UndefinedBehaviorSanitizer on the Mac (leak checking unavailable there).
NUC lacks the ASan runtime. An attempted unfiltered Mac KV sanitizer run hit the
pre-existing Linux-vs-BSD qsort_r call ABI in kv_snap.c/group import; retain that
failure rather than claiming the entire KV suite passed sanitizers. The new
focused env selector EFS_TEST_KV_SNAPSHOT_ONLY runs the relevant tests on Mac.
Raw build/test logs are in results/measure/20261008-w92-query-foundation.

Next: integrate bounded server refresh and publication. Capture group-applied
vectors with the KV cut under the host apply lock (pin only, no scans). Refuse
snapshot-import/partial Raft-entry states, retain the previous published version
on resource failure, rebuild initially and refresh dirty partitions on N/T.
Index candidates still require authoritative layout/generation, directory scope
and permission validation. Cross-group transactions, rename-only convergence,
server-owned rollups with folded publication lanes, versioned bounded RPCs,
freshness reporting and legacy virtual interface consumers remain required.
The running loader and historical w90 control paths continue unchanged.

W91 expiry tests remain required, but this branch has no expiry mutation/deadline
worker or settled command transport yet. Main W90/W91 are independently recorded
as design-open; preserve their documentation and add real expiry lifecycle gates
when the implemented interface can be exercised. Do not simulate expiration by
having the test unlink files itself or report unsupported cases as passing.

## Oct 8, 02:34 EDT — immutable catalog fold tested offline

`query_catalog.h/c` now builds bounded immutable catalogs in the background-only
API, reuses unchanged raw-metadata/name-index partitions, and folds subtree
rollups in RAM. It uses the actual metadata getattr reducer over the captured
RAM KV image, including lane publications and open append frontiers. APPEND_CUR
is now a metadata dirty hint: its frontier can change the logical size without
an inode or lane write. Dentry layout precedence deduplicates split mirrors and
honors hashed tombstones. Hardlinks count each name. Name searches spend an
actual shared candidate visit budget across partitions, preserving exact/prefix
seek efficiency and explicit exhaustion. Query-time operations neither scan LSM
nor rebuild aggregates. Host ownership must serialize catalog builds/frees and
keep readers alive until publication retirement; permissions require fresh host
validation before delivery.

NUC catalog/name-index unit suites and Mac focused ASan/UBSan suites pass.
Raw evidence: results/measure/20261008-w92-query-catalog. One initial expected
ctime was wrong because hardlink advances it; retained failure and corrected
expectation are documented. These are offline applied-image tests, not live
server acceptance. Host refresh/publication and RPC integration remain next.
No deployed binary changed. Loader remains active, ~16,384 new small files /
256 MiB at 02:31 EDT; no GiB stage completed. Reserves ~700 GiB /data1 and
803 GiB /home, hundreds of millions of free inodes. No errors or intervention
required; continue monitoring before any eventual pause/deployment.

## Oct 8, 02:36 EDT — host background publication smoke passed

Catalog builder commit: `ed2808f2`; host publisher commit: `220cf45a`. The host component adds one
publisher with N=256 metadata-key writes / T=2000 ms, 200 ms minimum spacing,
2 s failed-attempt backoff, 256 MiB candidate catalog and 64 MiB memtable-copy
budgets, plus a cooperative 10 s build deadline. One builder retains at most
old/candidate catalog generations. Reader callbacks hold a publication mutex;
publication retires the old image after readers drain. Shutdown cancels/joins
while the Raft pump remains alive.

Capture records actual per-group applied indexes and terms under the host apply
mutex, with no scan/fold there. It refuses non-voters, partial group coverage,
queued/running/partial imports. Post-capture ReadIndex barriers and unchanged
metadata hints reject asymmetric apply cuts; pending records refuse candidate
construction. Failures retain prior source/build times and watermarks. A busy
writer can prevent a consistent cut and exceed T: expose that age/error, never
claim T as already proven. Only n2/n3 currently cohost all groups; other nodes
return a dual-host redirect from the internal read API. Distributed assembly on
clusters without a complete cohost remains a capability limitation.

Private NUC fixture (separate stores and ports 24800–24803) passes initial
publication, actual Raft CREATE-to-rollup refresh, partial-host refusal and
restart rebuild. Existing shutdown gate passes 6/6 buffered/direct variants.
Raw logs and exact binary hashes (rsynced build identifies as unknown) are in
results/measure/20261008-w92-query-publication. Tiny-fixture timings do not prove
loaded pin latency, resource bounds or transaction/fault acceptance.

**Next:** versioned bounded query RPC and client virtual interface consumers,
fresh credential/ancestor validation, explicit stale status and output limits.
No RPC consumer is wired yet; live `.stats` remains wrong and `.find` still
walks on misses. Live deployed build remains e9f974b4e148. Do not deploy while
the loader is active. At 02:36 EDT it has ~19,072 new small files /298 MiB,
~700 GiB free /data1 and ~803 GiB /home; no GiB stage finished. W91 expiry tests
remain pending the real expiry interface, as noted above. This task stays open.
