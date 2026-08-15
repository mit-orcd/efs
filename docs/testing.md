# Testing and profiling

[Quick start](../README.md#quick-start) · [Operations](operations.md)

## `make test`

Runs:

- unit tests (`test_erasure`, `test_placement`, …)
- cluster integration, quota, migrate, direct-io, rejoin, query, list-exports
- `test_rw.sh` — real FUSE mount smoke test

On MIT Engaging, compile and run binaries on a compute node (Slurm), not on
a login node. See `.cursor/skills/slurm-execution/SKILL.md`.

## Slurm harness

`slurm-jobs/run.sh` builds on a compute node and starts a three-server
cluster plus two client smoke jobs via `sbatch`. Server data and mounts use
node-local `/scratch`; shared state and logs go under the path configured in
those scripts.

```bash
./slurm-jobs/run.sh
```

Client jobs are small `dd` / `cp` / `rsync`-style checks with short timeouts.

`slurm-jobs/stress-mixed-parallel.sh` is a single-job harness: 3 servers +
FUSE client on one node, mixed-size create/write/verify, and `perf record`
on the client and one server.

## Profiling

`--perf` on the server or client runs `perf record` against that process and
writes `perf.data` on exit (server: `<storage>/log/`; client:
`/tmp/efs-fuse-perf-<pid>/`). Missing `perf` is a warning, not a fatal error.

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --perf
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf
perf report -i /tmp/efs/s1/log/perf.data
```
