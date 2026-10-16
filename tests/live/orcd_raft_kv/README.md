# ORCD Raft/KV ablations and W121 kernel gate

These are explicit, disruptive live-cluster phases for the dedicated ORCD
scratch cluster. They use four daemons (fcstor003–006) and seven clients
(fcstor007–013), RDMA mlx5_2, files engine with server direct I/O, `.efs`
enabled, packing/containers/expiry disabled. Run as the site user, never root.
SSH agent forwarding through `erbmi1@orcd-login.mit.edu` and the private
libfuse prefix used by `orcd_qvote/control.py` must work first.

Keep raw output outside Git. Use a **new unique run name** for each experiment:

```sh
export EFS_ORCD_RK_ARTIFACTS=/absolute/path/outside/repo/orcd-raft-kv
export EFS_ORCD_RK_RUN=raft-kv-unique-run
export EFS_ORCD_RK_REV=4d6530dd83b086493a65b97cb344daf93291d166
python3 tests/live/orcd_raft_kv/runner.py prepare
```

Preparation exports exactly that commit into isolated `/tmp/efs-$RUN` trees,
builds daemon/FUSE/management binaries, saves hashes, and builds three KV
ablations. It does not alter a Git checkout or stop existing processes. It
runs the concurrent KV semantics test for each ablation on the storage nodes.
Stop the existing deployment with its own client/server identity helpers
before the first round; `round` refuses an existing daemon or storage root.
Existing stores remain untouched. Each round creates four fresh 17 GiB-quota
stores `/data1/01/efs-$RUN-$LABEL`. These must be on scratch storage with enough
free capacity. No automatic removal of old stores occurs.

```sh
python3 tests/live/orcd_raft_kv/runner.py round default1 default 1 1 100 --posix
python3 tests/live/orcd_raft_kv/runner.py stop default1
python3 tests/live/orcd_raft_kv/runner.py round batchoff default 0 0 100
python3 tests/live/orcd_raft_kv/runner.py stop batchoff
# Continue the matrix below, always stopping the preceding round first.
```

Arguments after LABEL are KV variant, no-op batching, overlap batching and
applied checkpoint interval in milliseconds. Keep the same source throughout:

| Label | KV variant | NOOP | OVERLAP | Interval |
|---|---|---:|---:|---:|
| default1 | default | 1 | 1 | 100 |
| batchoff | default | 0 | 0 | 100 |
| overlapoff | default | 1 | 0 | 100 |
| noopoff | default | 0 | 1 | 100 |
| checkpoint10 | default | 1 | 1 | 10 |
| default2 | default | 1 | 1 | 100 |
| serial | serial | 1 | 1 | 100 |
| noseek | noseek | 1 | 1 | 100 |
| stripe1 | stripe1 | 1 | 1 | 100 |
| default3 | default | 1 | 1 | 100 |

Use `--posix` on the first and final default rounds. A round runs seven
90-second mixed workloads with a common scheduled start: eight writers and
two query loops per client. Writers create unique 512-byte files, fsync/close,
verify exact bytes, then unlink. Queries check JSON and exact 32-entry immutable
fixtures (`ls`/`find`) and successful `stats`. There is a 30-second fixture/query
budget and a 180-second outer workload timeout. Core POSIX uses 16 jobs/client,
unchanged case deadlines and a 600-second outer timeout. POSIX failures are
retained separately; they are not hidden by a successful mixed workload.

The query processes are forked **before** writer threads start. This avoids
W121's triggering fork in the address space containing read buffers; it does
not fix or qualify the affected kernel. Run the separate `fork_repro.py` gate
on a new directory when qualifying a client kernel. Neither a posix_spawn
control nor this isolated harness closes that gate.

`variant.py` changes only one mechanism in a fresh exported source:

- `serial`: take the exclusive view lock for outer readers. Nested-read
  ownership, writer locking, TLS buffers and all correctness fixes stay in
  place. This isolates shared-reader concurrency; it is not a full historical
  source rollback.
- `noseek`: retain cached seek lookup, but disable filling the seek block on a
  shared scan cache miss. Private/sequential scans retain their behavior.
- `stripe1`: one segment-cache lock stripe instead of 32. Atomic cache-budget
  accounting remains unchanged.

No tuning switch is added to production. The comparison binaries are isolated
artifacts, and `default` restores the unmodified source binary. Query-count,
readback or outer workload failures stop the runner for investigation.

After the final default round, `runner.py cold` writes/fsyncs seven 2 MiB
markers, cleanly stops/remounts the same stores, verifies all seven markers on
all seven clients, then verifies each client's 32 fixed payloads and cold query.
It leaves that configuration running. No crash/power-loss acceptance is claimed.

Run the collector on a host that sees the shared output directory:

```sh
python3 tests/live/orcd_raft_kv/collect.py /home/erbmi1/orcd/scratch/efs/orcd-test/$EFS_ORCD_RK_RUN > comparison.json
```

The collector pools operation latency samples, retains failures and POSIX
outcomes, checks runtime feature/transport identities, and reports per-process
CPU cores and peak RSS during the measured workload. Its timing window uses
workload completion-file time minus reported elapsed time; synchronized clocks
and shared-file timestamps are prerequisites. These short fresh-store runs
are not a large-namespace, 400 GiB, cold-cache or crash-recovery qualification.
