# Raft follower batching default — 2026-10-16

**Scoped PASS (performance), qualification NOTRUN.** Live Intel workloads
measured on release `3fba43af72fe` with the existing opt-in bundle
`EFS_RAFT_BATCH_OVERLAP=1 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_APPLIED_INTERVAL_MS=100`:
open/close lease edges (Raft session commands) fell from about 25 ms to
3.7–3.9 ms mean, and the AMD client's small-file creates rose from 77/s to
345/s. The follow-up commit makes this bundle the default for both storage
engines (each flag still overridable). That default commit is not yet
deployed; crash/power-loss and seven-client qualification are NOTRUN.

Linked item: lease-edge latency follow-up to
[asynchronous PUT acknowledgement](intel-async-durability-20261015.md).

## Finding

Each open and last close commits a lease edge through the session Raft group.
Under the user's workloads (xorinox: ecopy plus 4-worker md5 scan; AMD:
4-worker tiny-file creates plus md5 scan) these averaged 24–26 ms. Raft batch
observability (`EFS_BATCH_OBS=1`) on the four daemons showed:

- Read confirmations were already batched (one probe round in flight per
  group); they were not the cause.
- The followers of the busiest group (n1, n3 for leader n2) synced roughly
  every received AppendEntries separately: 1.1–1.6 WAL entries per sync,
  pump drains of 16–32 ms (p50 bin 16 ms) and inbox ages of 25 ms (n1) and
  125 ms (n3) mean. 311,754 receive batches on n1 were refused as
  `tail_behind` — AEs overlapping entries the follower already had, which the
  files storage never batched because `EFS_RAFT_BATCH_OVERLAP` and
  `EFS_RAFT_BATCH_NOOP` defaulted on only for the container engine.
- The n3 pump thread was ~16% on CPU and blocked almost entirely in
  `fdatasync` (page writeback and device cache flush).

## Results

| 10 s client strace / 50 s batch-obs window | Default (off for files) | Bundle on |
| --- | --- | --- |
| Lease edge (session command) mean, xorinox | 26.4 ms (p95 36.6) | 3.9 ms (p95 6.3) |
| Lease edge mean, AMD | 25.0 ms (p95 36.7) | 3.7 ms (p95 6.1) |
| AMD CREATE_RETAINED mean / rate | 15.3 ms / 77 per s | 2.1 ms / 345 per s |
| AMD REPORT_CHUNKS mean | 25.8 ms | 7.3 ms |
| Busy-follower pump drain mean | 12.2 ms (n1), 16.6 ms (n3) | 1.3 ms (n2), 1.6 ms (n3) |
| Busy-follower inbox age mean (p50 bin) | 24.6 ms (32 ms), 124.5 ms (131 ms) | 4.8 ms (2 ms), 17.8 ms (2 ms) |
| Follower overlap batches | 0 | ~1,160–1,190 per s |

Leadership moved at the restart (the busy followers were n2/n3 in the second
run), so per-node rows compare the busy followers, not node ids. Workload mix
is the user's live load, not a fixed fixture; rates differ between windows.
WAL entries per follower sync rose only to 2.5–2.9; further group commit is
possible.

## Environment

- Intel cluster intel1/intel2 (Core Ultra 7 256V, 8 cores each), 4 efsd (two
  per host: n1/n3 on `/data1` NVMe, n2/n4 on the system NVMe LVM), files
  engine, TCP, O_DIRECT fragments, asynchronous PUT acknowledgement (200 ms /
  256 MiB window), 2.5 GbE. Clients xorinox and AMD (`/data1/efs/intel-mnt`).
- Release `3fba43af72fe` (`v0.3.2-pre-alpha-526-g3fba43af7`, built 01:59Z on
  each host): efsd SHA-256 intel1
  `a6c44b21bc71103d61c02a2028820c1ca7f2132372609d3c0d8b054cdd0fc743`, intel2
  `8bdb9dfad98c3a6bbfe81c3f44ec65de0a5876e17972b43b1f683af5c40733e0`; efs-fuse
  xorinox `ed49811136be6cbcfd95827dded5bce312d4d92856ec7b3a1cc29a2a385fa5b7`,
  AMD `8738052e52b0745e3ba8e98d3598f7b34a636a22fea5ff43c006a45721437c60`.
  Baseline daemons ran with `EFS_BATCH_OBS=1 EFS_RAFT_OBS=1`; the second run
  added the three bundle flags. Both were started by the guarded rollout
  (`~/git/devops/profiling/runs/20261015-rollout-next/rollout.py RELEASE`,
  daemon environment via `EXTRA_ENV`), 64-sample controls passing on all
  four mounts.

## Change (default commit)

`src/server/raft_host.c`: `host_batch_flag()` makes `EFS_RAFT_BATCH_NOOP` and
`EFS_RAFT_BATCH_OVERLAP` default to 1 for both engines (`0` disables, other
values refuse startup); `HOST_APPLIED_PERSIST_US` default 100 ms (was 10 ms;
`EFS_RAFT_APPLIED_INTERVAL_MS` 10..1000 overrides). The receive-overlap path
itself is unchanged (prefix, term, suffix, admission, byte and config
validated in `src/raft/raft.c`). A longer applied-checkpoint interval only
lengthens replay after a crash. As in the original bundle acceptance, a clean
shutdown is required before downgrading to binaries without the atomic
recovery cursor. `tests/test_applied_interval.py` checks both defaults and
the flag parser; `tests/test_raft` covers no-op/overlap receive batching.

## Reproduce

```sh
EXTRA_ENV="EFS_BATCH_OBS=1 " python3 rollout.py RELEASE        # baseline
EXTRA_ENV="EFS_BATCH_OBS=1 EFS_RAFT_BATCH_OVERLAP=1 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_APPLIED_INTERVAL_MS=100 " python3 rollout.py RELEASE
```

Run a small-file read/create load on the clients, take the last `batch-obs`
JSON line of each `/data1/efs/logs/efsd-n*.log` before and after a ~50 s
window and diff counters/histograms (`sync`, `batch_overlap`, `tail_behind`,
`wal_entries`, `pump_drain_us`, `inbox_age_us`), and strace a client's
efs-fuse for 10 s, summarised with `scripts/summarize_intel_rpc_trace.py`
(the session command shows as `EFS_MSG_RAFT_MKFS`, message type 95).

Raw (not in Git), `~/git/devops/profiling/runs/`:
`20261016-live3-3fba/batchobs-before.txt`
(`7dce55a6dea4ebd23ee2401f1e7f9cda9d5f3a338be0ec15221a57cb64308a54`),
`batchobs-after.txt` (`0ea9af1110dcb03d228a5e9e7263461842769343187b56293122f097f0258b67`),
`xorinox_efs-rpc.txt` (`dee7460c755814acecabf7f72f80dc247a53f3fb722f1fbe840b5b69093abffb`),
`amd_efs-rpc.txt` (`584869de6ac38997dc06f4b7bdd563c6ab8eb8f83f79bab77c91ab916ea49228`);
`20261016-live4-batch/batchobs-before.txt`
(`5f8ce1764c924be07515148c21f3c5565c6dd243453b749acef4da47fcfaa667`),
`batchobs-after.txt` (`764703209f18bbd9228c7bc842fd82f0bdf33e80219b004ec96c2b6b8d361ed0`),
`xorinox_efs-rpc.txt` (`cfd933c777565605411316c33ae9f1a3e902f77248aac504afd73233d53c0cb9`),
`amd_efs-rpc.txt` (`616be540031a312d76934c1f0c8708ab70f8f8830a6f069d8f71a35070e4a064`).

## Same-day client and KV releases (context)

`ae3a49d8a` fixed retained last-close lease releases (a non-blocking CLOSE
identity attempt fell back to a 10/s drain; 24,456 retained rows on xorinox)
and hashed the open table; `98a30a4e8` made the open's FileID GETCHUNKS ask
for chunk 0 only; `3fba43af7` made LSM point gets and scans run concurrently
(writer-preferring reader/writer lock; ThreadSanitizer-clean stress test).
With these, the earlier ~43-thread queue on the metadata leader's KV mutex is
gone.

## Review follow-up (2026-10-16)

A review of this session's commits found a deadlock introduced by
`3fba43af7`: scan callbacks read the same KV (getattr's reduce scan asks the
transaction coordinator, which reads the decision key), and a nested shared
acquisition of the writer-preferring lock blocks behind a queued Raft apply
that waits for the outer one. `tests/test_kv_lsm_concurrent` (nested gets in
scan callbacks under a writer, with an alarm) deadlocks against `d24b17feb`
on its first 10 s run and passes with `6b5c34624` (per-thread shared depth;
only the outermost reader drops the hold around a pread). `d24b17feb` and
`3fba43af7`/`511968299` carry this hazard; the fix is deployed with the
merge release below. Also: the queued-vote experiment cherry-picked as
`e71fdfa56` is reverted (`f1ae5b7a8`, as on the AMD branch: loaded POSIX
worsened, ~37% of a busy follower pump's CPU on Intel); a release's CLOSE
identity wait is bounded to 250 ms (`bfe654266`); `devel-amd-durability-20261015`
is merged (`d23da0951`: all-node COMMIT with boot verifiers and verified
clean-restart verifier reuse). Full `make test` on the merged tree: 1,346
commands, only the stale architecture regeneration failing (regenerated here).

## Limitations and remaining gates

- The default commit itself is not deployed or measured; the measurement used
  the same code paths via environment flags on `3fba43af72fe`.
- Live mixed workloads, one window each; no fixed-fixture repeat.
- No crash/power-loss, seven-client POSIX or persistence qualification.
- Follower syncs still carry only 2.5–2.9 entries; n3 inbox mean 17.8 ms
  shows a remaining tail.
