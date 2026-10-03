# Testing and profiling

[Quick start](../../README.md#quick-start) · [Operations](../operations/operations.md) ·
[What to work on](../status/README.md) · [Verification plan](verification.md)

## `make test`

Builds and runs the unit suites: `test_erasure`, `test_placement`, `test_wire`,
`test_data`, `test_kv`, `test_kv_lsm`, `test_raft`, `test_raft_store`,
`test_meta_apply`, `test_sim`, `test_txn`, `test_session`, `test_lock`,
`test_stage_evict`, `test_conn_fd`. `test_sim` is the deterministic simulator
and is the correctness gate for protocol changes; the cluster is the perf
harness, not the correctness harness.

Everything must be green. There is no accepted-failure list.

## Cluster gates

Build and run on the dedicated fcstor test cluster — never Slurm, never the NFS
home (see `.cursor/rules/efs-fcstor-deploy.mdc` for the deploy procedure and
`.cursor/rules/efs-remote-timeouts.mdc` for timeouts).

`bash tests/preflight.sh` first (read-only: one efsd per server, one build
ID, both Raft leaders, `commit == applied`, idle commit rate, every mount
`fuse.efs-fuse`). `tests/cluster.sh stop|start|restart [--clients] [--perf]
[--strace]` is the only way to bounce the cluster.

`tests/run_tests.sh <cmd>`:

| Command | What it gates |
|---|---|
| `setup` | rsync + build + mount the clients |
| `posix` / `posixstress` | POSIX suite 1 vs an XFS baseline, one client or N in parallel |
| `posix2` | cross-client visibility, one pair or several |
| `posixpersist` | durability across unmount/remount (the only suite that proves anything is durable) |
| `nvme` | the local `/data1/01-06` NVMe ceiling (no efs involved) |
| `meta` | `efs-bench --meta` metadata op rates |
| `leaks` | valgrind memcheck, client and server, TCP and RDMA |

A timeout is a FAIL, not a skip.

## Honest throughput numbers

A write number counts only with the flush inside the clock: `dd bs=1M
conv=fsync` of a non-zero source (all-zero chunks skip PUTs), or fio with
`--end_fsync=1` and no `time_based`; read numbers only after a remount of
every reading client. A `time_based` fio with `--direct=1` skips the kernel
page cache but not the client's userspace dcache, so its write column is
memory bandwidth. Method and the number history live in
`.cursor/rules/efs-fio-honest.mdc`; the current references are in
[the status page §1a](../status/README.md) ("Baselines"). `tests/stress/fio_honest_matrix.sh` is the
fio form; `tests/measure/*.sh` hold the dd and IOR runbooks.

Before every number: `findmnt -o FSTYPE /tmp/efs-mount` must print
`fuse.efs-fuse` and `stat` of the mount must succeed. Sanity check any
write number against on-disk `du` of `/data1/0*/efs`.

## Other harnesses

- `tests/stress/raft_host_smoke.sh` — targeted metadata-op smoke (layout
  variants, cross-group transactions). Its EXIT trap stops the cluster it
  started, so do not point it at a cluster you want to keep.
- `tests/stress/unlink_storm.sh`, `tests/stress/scale_grow.sh` — mass
  unlink and namespace-growth harnesses.
- `tests/rdma_first_inode.sh` — localhost repro for the empty-table first
  `mkdir` over RDMA.
- `tests/perf/io500/` — IOR easy/hard through the FUSE mounts (needs an MPI
  stack and the IO-500 driver; not a valid list submission).
- `tests/perf/tcp_rdma/` — paired TCP/RDMA benchmarks. Transport echo and
  the SEND baseline are separate from filesystem runs. They do not switch
  the live cluster. See `tests/perf/tcp_rdma/README.md`.
- `tests/faults/` — fault injection beside the POSIX suites. The simulator
  layer and a private 3-server FUSE cluster are separate. Repair stays a
  gap. See `tests/faults/README.md`. It is not part of `make test`.

## Profiling

`--perf` on `server.sh` or `client.sh` attaches `perf record -F 499 -g` to
the daemon. `stop` writes `flat.txt`, `by_thread.txt`, and `callers.txt`
next to the data file under `~/orcd/scratch/efs/perf/` (`efsd-<port>/` or
`<mount-name>/`).

```bash
./scripts/server.sh --perf 127.0.0.1:17432 /tmp/efs/s1:5G
./scripts/server.sh stop 127.0.0.1:17432
./scripts/client.sh --perf 127.0.0.1:17432 /mnt/efs
./scripts/client.sh stop /mnt/efs
```

On the test cluster `tests/cluster.sh restart --perf [--strace]` does the
same for the four servers (`/tmp/efs-perf/efsd.data`, `efsd.strace`);
`cluster.sh stop` SIGTERMs so the perf file gets its footer.

Attaching to a live daemon: `perf record -g -p $(pgrep -x efsd)`. A
profile taken at mount sees only the initial threads — the client's
reclaim/put pools are created at the first write, so attach after the I/O
has started. `perf record -a` is refused at `perf_event_paranoid=1`. Run
`perf report` on the node that recorded. Never restart a daemon under
`strace` and then quote the throughput; a traced daemon sits on the strace
plateau (~100 K lines/s) and the MB/s is a tracer number.
