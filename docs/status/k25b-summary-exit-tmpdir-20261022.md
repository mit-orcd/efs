# K25b: summary-runner exit status (R1) and test TMPDIR hygiene

**Verdict: PASS (scoped)** on branch `devel-k25b`, not merged. The fix covers
alpha 1.0 Stage 1 row K25, which the
[integration review](devel-integration-review-20261022.md) reopened with
finding R1 (P1). It also covers the K25 TMPDIR item in the
[checklist](alpha-1.0-2plus1-checklist.md).

- **Tested source:** `aea319575` on devel `1bf4bd45f`. R1 is `8c630edd5` and
  the TMPDIR work is `aea319575`. The tree built on efs2 was compared file by
  file with `git archive aea319575` and all 1,937 files matched. The build ID
  reads `unknown` because the copy has no `.git`.
- **Environment:** efs2 Tart VM, Fedora 42 aarch64, kernel 6.19.14, gcc
  15.2.1, Python 3.13. `TMPDIR=/data1/tmp/k25b` is a private subdirectory of
  `/data1/tmp` on XFS (`/dev/vdb`), so concurrent sessions' files stay out of
  the counts. There was no cluster, no RDMA device and no valgrind headers.

## Exit-status contract of `scripts/make_test_summary.sh`

| Exit | Meaning | `summary.txt` line 1 |
| --- | --- | --- |
| 0 | every command ran and passed | `TOTAL n FAIL 0 NOTRUN 0 STATUS PASS (Ns)` |
| 1 | a command failed or timed out (124), or the build failed or listed no commands | `... STATUS FAIL ...` |
| 2 | usage error (no Makefile) | none |
| 3 | all passed, but at least one printed a NOTRUN or SKIP line | `... STATUS INCOMPLETE ...` |

Each failure line names `tmp/fail-<index>.log`, which holds the command's
full output. Commands run in their own process group, so a timeout kills the
whole command. `EFS_TEST_CMD_TIMEOUT` bounds each command and defaults to
900 s. No script in this repository, `devops` or `cluster` reads the runner's
status or `summary.txt` programmatically. The runner still sets
`TMPDIR=$PWD/tmp` for its own runs.

## TMPDIR hygiene

C tests now use `tests/test_tmpdir.h`. Every scratch path lives under
`$TMPDIR`, which defaults to `/tmp`. The paths are removed when the test exits
0. On a nonzero exit or a crash they are kept and printed as
`efs-test: kept <path>`. These were converted:

- 40 C tests and the embedded C in two Python tests.
- Two Python tests that passed `dir='/tmp'`.
- The Makefile `mktemp` recipes.
- `tests/live/server_wrapper_start.py`, which kept its evidence even on
  success.

`tests/test_tmpdir_hygiene.py` stops new hard-coded paths. Two latent
defects turned up and were fixed:

- **`test_store_durability`** assumed a `/tmp`-deep path for its
  ancestor-sync count.
- **The query negative controls** let their deliberately aborting mutant
  keep scratch in the caller's TMPDIR.

Product defaults that still use `/tmp` were left unchanged, as briefed:

- `EFS_FAULT_FILE` and `KV_FAULT_COMPACT_FILE` default to `/tmp/efs/fault`.
- The client control socket is `/tmp/efs-control-*.sock`.
- The perf and strace outputs of `efs-fuse`, `efs-bench` and `bench_local`
  go to `/tmp`.
- The socket of `scripts/posix_phase_runner.py` is in `/tmp`.
- Cluster operation scripts such as `tests/cluster.sh` and
  `tests/roll_efsd.sh` use the `/tmp/efs` deployment tree.

## Commands

```bash
git archive aea319575 | ssh efs2 'mkdir -p ~/build/devel-k25b && tar -xm -C ~/build/devel-k25b'
ssh efs2 'cd ~/build/devel-k25b && make -j4'
ssh efs2 'mkdir -p /data1/tmp/k25b && cd ~/build/devel-k25b && TMPDIR=/data1/tmp/k25b make test'
```

Before and after each run, the driver listed and sized the top level of
`/data1/tmp/k25b`. Locally, on macOS with Python 3.9, these three ran:

```bash
python3 tests/test_make_test_summary_runner.py
python3 tests/test_tmpdir_hygiene.py
python3 tests/test_make_test_recipe.py
```

On the Mac, the reproducer from review R1 now exits 1 with
`STATUS FAIL`. To clean up, delete `~/build/devel-k25b` and
`/data1/tmp/k25b` on the VM.

## Results

| Run | `make test` | `/data1/tmp/k25b` before → after |
| --- | --- | --- |
| 1 | FAIL after 34 s in `tests/live/server_wrapper_start.py` | 0 → 1 entry, 16 KB: its kept evidence, path printed |
| 2 | PASS, exit 0 | 1 → 1 entry, 16 KB |
| 3 | PASS, exit 0 | 1 → 1 entry, 16 KB |

- **Runs 2 and 3** were consecutive full passes. Scratch did not grow beyond
  the one directory that run 1 kept on its failure.
- **The run 1 failure** happened when efsd's port refused a connection right
  after `scripts/server.sh` started it. The kept log shows `server.sh` could
  not open `efsd.log` at launch, which points to a start race in its
  stale-PID path rather than to this change. The same test passed in runs 2
  and 3 and in an earlier `make -i test` pass of this tree. The flake is
  unresolved.
- **NOTRUN lines** printed by passing tests in runs 2 and 3 cover RDMA
  transports with no device, the memcheck gate without valgrind headers and
  the cluster stop scripts. Under the new runner these make the status
  INCOMPLETE (exit 3), not PASS.
- **A concurrent session** on efs2 ran a pre-fix tree during the window. It
  added about 128 `/tmp/efs-*` entries, such as `efs-sort-*` and
  `efs-query-*`. This branch writes no such paths.

## Limitations and remaining gates

- **Runner not run end to end:** `scripts/make_test_summary.sh` was not run
  in full on node9901, so this run does not exercise its private libfuse
  setup. The self-test exercises the embedded runner.
- **Not merged:** K25 stays REOPENED until `devel-k25b` is merged and the
  integration review closes R1.
- **Open flake:** the `server.sh` start race behind the run 1 failure is not
  addressed here.
