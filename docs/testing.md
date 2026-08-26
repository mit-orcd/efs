# Testing and profiling

[Quick start](../README.md#quick-start) · [Operations](operations.md)

## `make test`

Runs:

- unit tests (`test_erasure`, `test_placement`, …)
- cluster integration, quota, migrate, direct-io, rejoin, query, list-exports
- `test_rw.sh` — real FUSE mount smoke test

Build and test on the dedicated fcstor test cluster (never Slurm). See
`tests/run_tests.sh` (`posix|posix2|perf|setup|all`) and
`.cursor/rules/efs-fcstor-deploy.mdc` for the deploy/gate procedure.

## Profiling

`--perf` on the server or client runs `perf record` against that process and
writes `perf.data` on exit (server: `<storage>/log/`; client:
`/tmp/efs-fuse-perf-<pid>/`). Missing `perf` is a warning, not a fatal error.

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --perf
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf
perf report -i /tmp/efs/s1/log/perf.data
```
