# Testing and profiling

[Quick start](../README.md#quick-start) · [Operations](operations.md) ·
[What to work on](arch/START-HERE.md) · [Verification plan](arch/verification.md)

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

## Honest fio

`tests/stress/fio_honest_matrix.sh` is the only efs throughput harness:
writes with `--end_fsync=1` and no `time_based`, remount every client before
reads. A `time_based` fio with `--direct=1` skips the kernel page cache but
not the client's userspace dcache, so its write column is memory bandwidth.
Method and the current table live in `.cursor/rules/efs-fio-honest.mdc`; the
current single-client numbers are in `results/perf/20260917-honest/` and
`results/perf/20260918-dd-1c/`.

Sanity check any write number against on-disk `du` of `/data1/0*/efs`.

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

`--perf` on the server or client runs `perf record` against that process and
writes `perf.data` on exit (server: `<storage>/log/`; client:
`/tmp/efs-fuse-perf-<pid>/`). Missing `perf` is a warning, not a fatal error.

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --perf
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf
perf report -i /tmp/efs/s1/log/perf.data
```

To profile a live cluster, attach instead of restarting: `perf record -p
$(pgrep -x efsd)` and the same for `efs-fuse`. Never restart a daemon under
`strace` and then quote the throughput.
