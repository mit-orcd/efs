# Testing and profiling

[Quick start](../using/quickstart.md) · [Operations](../operations/operations.md) ·
[What to work on](../status/README.md) · [Verification plan](verification.md)

## `make test`

Builds and runs the suites listed in the current Makefile, including the
simulator, metadata/session/transaction, store, buffer, publication and
writeback gates. The list evolves; use `TEST_BINS` and the `test` target
rather than this guide as a fixed suite inventory. Focused protocol tests
exercise logical behavior; public callbacks, real storage and RDMA require
their own acceptance gates. [Verification](verification.md) distinguishes
simulation, process restart and hardware power loss.

Everything must be green. There is no accepted-failure list.

## Current release acceptance and local POSIX suites

[v0.2.0-pre-alpha AMD acceptance](../status/v020-amd-release.md) records
four modes: buffered/direct fragment I/O × TCP/RXE RDMA. Each has 216 POSIX
passes, one unsupported writable-mmap skip, 64 peer passes and 26 cold-remount
verification passes. Full Linux `make test` passes. These are functional gates,
not power-loss durability or cross-host hardware-RoCE measurements.

Against two already serving disposable mounts:

```bash
python3 tests/posix/posix_suite.py /mount/one --jobs 4 --timeout-s 30 --results /tmp/posix.tsv
python3 tests/posix/posix_2client.py --local /mount/one /mount/two --parent posix-peer --results /tmp/posix2.tsv
python3 tests/posix/posix_persist.py /mount/one --phase prepare --results /tmp/persist-prepare.tsv
# Cleanly stop and remount /mount/one with the same intended transport/settings.
python3 tests/posix/posix_persist.py /mount/one --phase verify --results /tmp/persist-verify.tsv
```

Do not run verify before a real remount, discard failed writes to obtain a
pass, or let the remount silently change an RDMA test into TCP. Retain the
binary/source identity, daemon I/O flags, actual transport and full verdicts.
Totals evolve with the suite; a historical count is not a fixed test contract.

The site's `~/git/devops/nuc-efs` drivers select NUC/AMD, while
`~/git/cluster` controls xorinox's VMs. Those directories and the external
portal are not bundled here. The retained
[AMD matrix driver and evidence](../../results/measure/20261008-v020-amd/README.md)
document that environment; they are not a general cluster installer.

## Recorded fcstor cluster gates

The following commands describe the recorded fcstor harness, not a live
inventory or the NUC/xorinox controller. Verify its configuration and use the
correct cluster runbook before invoking setup/restart. Build on node-local
scratch rather than the NFS home (see [cluster test operations](../operations/cluster-testing.md) for
pre-flight, deployment and timeout guidance).

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
| `posixpersist` | acknowledged data survives the selected remount/process-crash scenario; does not establish hardware power-loss durability ([W71](../backlog/work-items.md#w71)) |
| `nvme` | the local `/data1/01-06` NVMe ceiling (no efs involved) |
| `meta` | `efs-bench <seed:port> --meta` cluster RPC metadata op rates; distinct from local `--bench meta` |
| `leaks` | valgrind memcheck, client and server, TCP and RDMA |

A timeout is a FAIL, not a skip.

## Honest throughput numbers

A write number counts only with the flush inside the clock: `dd bs=1M
conv=fsync` of a non-zero source (all-zero chunks skip PUTs), or fio with
`--end_fsync=1` and no `time_based`; read numbers only after a remount of
every reading client. A `time_based` fio with `--direct=1` skips the kernel
page cache but not the client's userspace dcache, so its write column
can include buffered acceptance/cache work. Confirm publication boundaries,
working-set size and post-run integrity before calling it storage throughput.
Flushing inside the clock does not independently prove target persistence
(W71). The dated references and measurement limits are in
[performance.md](performance.md) ("Baselines"). `tests/stress/fio_honest_matrix.sh` is the
fio form; `tests/measure/*.sh` hold the dd and IOR runbooks.

Before every number: `findmnt -o FSTYPE /tmp/efs-mount` must print
`fuse.efs-fuse` and `stat` of the mount must succeed. Sanity check physical fragment allocation and readback on each actual storage
root; `/data1/0*/efs` is specific to the recorded fcstor layout.

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
