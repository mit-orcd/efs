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

**Fio bandwidth:** do not quote `run_tests.sh perf` write numbers — they are
cache-inflated. Honest method + Aug 29 1/4/9-client table:
`.cursor/rules/efs-fio-honest.mdc`. Harness:
`tests/stress/fio_honest_matrix.sh` (writes `--end_fsync=1`, remount before
reads). Results: `results/perf/20260829-honest/`.

**2PC gen-sync:** after a one-client ecopy, all 4 servers must report the
same committed generation (the joiners-stuck-at-gen-2/5 bug). Repeat:

```
bash tests/clean_cluster.sh && bash tests/run_tests.sh setup
bash tests/stress/ecopy_gen_sync.sh
```

Workload is `~/git/direct_copy/ecopy ~/orcd/scratch/ecrawl-synt-small/ /tmp/efs-mount/`
on fcstor007. Default copy window 180s (tree is ~6M files; finishing it is
not the gate). `TRACE=1` attaches perf/strace to existing efsd pids.

## Profiling

`--perf` on the server or client runs `perf record` against that process and
writes `perf.data` on exit (server: `<storage>/log/`; client:
`/tmp/efs-fuse-perf-<pid>/`). Missing `perf` is a warning, not a fatal error.

```bash
./scripts/server.sh 127.0.0.1:17432 /tmp/efs/s1:5G --perf
./scripts/client.sh 127.0.0.1:17432 /mnt/efs myexport --perf
perf report -i /tmp/efs/s1/log/perf.data
```
