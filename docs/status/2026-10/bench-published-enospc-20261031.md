# bench_published `rc=-28`: not ENOSPC, a refused session (Task CE, K86)

**Verdict: FAIL (product defect, K86 OPEN, blocks alpha: yes).** No code changed.
Linked item: the "2 of 10, not investigated" line in [gate-load-failures-20261030.md](gate-load-failures-20261030.md).

## Mechanism

- `rc=-28` is `EFS_ERR_UNREGISTERED` (`include/efs/common.h:154`, "session not registered on shard"), not errno ENOSPC. No store, tmp or FUSE write is involved; `efs-bench` here is a single process that talks to the daemons over TCP (no mount).
- `efs-bench` (`src/client/efs_bench.c`) starts one session (`efs_client_session_start`) and then runs three `data_worker_main` threads, each creating `bench-data-170-w<i>` under the root. One worker's `mkdir` is refused, so the bench prints `bench worker=<i> bytes=0 rc=-28` and `BENCH_FAIL`; the test's `assert (returncode == 0) == ok` fails.
- The daemon log names the refusing call: `raft-host: mkdir parent=1 name=bench-data-170-w<i> rc=-28 ... psh=1 csh=<n> dsh=1 stage=4` (`server_raft_host_mkdir`, `src/server/raft_host.c`). `stage=4` is the last stage assigned before the namespace session admission (`host_namespace_session_admit_covered`, `raft_host.c:17676`) and `stage=5`, so the `UNREGISTERED` comes from either the owner lookup (`NOT_FOUND` mapped at 17705) or the closing `efs_session_accept` (`src/meta/session.c:463-472`, shard not yet established for this epoch). The log does not separate the two.
- The session was valid: the other two workers on the same session succeeded, and the daemon applied the session entry (`applied session index=4 kind=1`) 14 ms before. The refusal is therefore a transient wrong answer under load, not a missing session. Admission is documented as host-side (`Hosts never create a session on a client's behalf`), and the bench create path does not retry `UNREGISTERED` (`efs_fuse.c:4870` maps it to `-EIO`).
- All three reproduced failures and both of BM's have `dsh=1`: the sibling workers admit the same session on the same shard at the same time. A race between concurrent first admissions is the working hypothesis; it is not proven.

## Evidence from Task BM (intel1, read only)

`~/efs-artifacts/taskBM-20261030/` on intel1 (SHA256SUMS present there):

- `intel1/loaded-4/fail-1.log` (8-job `test-long`, 04:04:54Z): `bench worker=1 bytes=0 rc=-28`; node log `raft-host: mkdir parent=1 name=bench-data-170-w1 rc=-28 hint=2 psh=1 csh=2783 dsh=1 stage=4 ino=0`.
- `intel1-after/full-3/fail-481.log` (8-job `test-all`, 05:39:25Z): `bench worker=2 bytes=0 rc=-28`; `raft-host: mkdir parent=1 name=bench-data-170-w2 rc=-28 hint=1 psh=1 csh=173 dsh=1 stage=4 ino=0`.
- BM saved no `df`. Current intel1: `/home` 874 G, 817 G free (7 %); `/` 70 G, 63 G free. The bench writes 2 MiB plus 5 small files per run into three 1 GiB quotas.

## Runs on intel2 (this task)

Host: intel2, 8 CPU, XFS `/home` (790 G free, unchanged by the runs), idle before start. Tree: devel `8e53122cd`, `make efsd efs-fuse efs-bench efs-mgmt` (build id `unknown`, local `git archive`). Each run has its own `TMPDIR` under `~/efs-taskCE/tmp`, deleted after the run.

| Load | Runs | PASS | FAIL | wall per run |
|---|---|---|---|---|
| solo, one at a time | 10 | 10 | 0 | 5.7-8.6 s |
| 8 concurrent copies x 10 rounds | 80 | 80 | 0 | |
| 24 concurrent copies x 10 rounds (load average 38) | 240 | 234 | 6 | 5.5-10.4 s (median 8.9 s) |

The 6 failures at 24 copies: 3 are `rc=-28` at `stage=4`, `dsh=1` (`r6-13`, `r7-16`, `r8-2`; `r8-2` also in a `--meta` create, `bench-meta-*-d1`); 3 are a different load failure (`bench worker rc=-4`, daemon `mkdir ... rc=-13 stage=3` or all workers `-4`: read-index/timeout class, `r4-6`, `r4-14`, `r8-15`), not examined here. `gate_load_repro.sh loaded` (full `test-long`, 15 min per run) was not repeated: the 24-copy load reproduces the signature in minutes.

## Attribution

Not environment: the disk was never short (790 G free before and after, `df` per run in `before.txt`/`after.txt`), and the errno reading was a misreading of a Raft-side error code. Not harness: the test and `efs-bench` use one valid session and fail on the first refused call, with no temp handling involved. **Product defect**: a daemon refuses a valid session with `UNREGISTERED` under CPU load, and nothing retries it. Reproduction rate 3/240 at load average 38 (about 1 %), 2/10 and 1/5 in BM's gate runs.

## Follow-up (not done here)

Separate the two `UNREGISTERED` sources in `server_raft_host_mkdir` (log which one), and test concurrent first admissions of one session on one shard (`host_namespace_session_admit_covered`: the local-miss path re-queries the owner and issues `REGISTER_ESTABLISH` while a sibling's identical command is in flight). Decide whether the host should wait for the in-flight admission or the client should resend, as it does for appends.

## Reproduction

```sh
git archive <commit> | ssh HOST 'mkdir -p ~/efs-taskCE/src && tar -x -C ~/efs-taskCE/src'
ssh HOST 'cd ~/efs-taskCE/src && make -j8 efsd efs-fuse efs-bench efs-mgmt'
# per run: TMPDIR=<fresh dir> python3 tests/test_bench_published.py ; rc and wall recorded
# load: 24 such runs started at once, repeated 10 rounds (each with its own TMPDIR)
```

Prerequisites: FUSE available (else NOTRUN). Cleanup: delete `~/efs-taskCE`. Raw output: intel2 `~/efs-artifacts/taskCE-20261031/` (1323 files, `SHA256SUMS` sha256 `97f0918b6cefc9141f964c67f9b87c7609f063380b002f4a9eefe46e521ab0ac`).

Remaining gate: K86 root cause and fix, then 240 runs at 24 copies without `-28`.
