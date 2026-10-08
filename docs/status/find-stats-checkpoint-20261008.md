# W92 find/stats continuation checkpoint — Oct 8, 01:53 EDT

This work is **in progress, not accepted**. `.stats` still reports zero for
peer-created files. Server-owned rollups/name indexes have not been implemented.
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
