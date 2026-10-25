# E2 clean client stop on a busy client: bounded revoke, progress-based stop (Oct 24)

**Purpose.** Fix [A11 finding 1](a11-qualification-matrix-20261024.md#validation-runs-nuc)
for checklist item [E2](alpha-1.0-2plus1-checklist.md#e-full-verification-and-orderly-lifecycle):
in 2 of 2 full `nuc-private-3` runs the clean stop in `posix-durability`
failed because the unmount's session revoke took about 11 s (`touched=765`)
while `client_processes.py` allowed 10 s after the unmount. A clean stop must
succeed on a busy client and must stay visibly failed when the stop really
fails.

**Verdict: PASS (scoped: one host, loopback, TCP).** GATE_SUMMARY

- **Branch:** `claude/devel-e2-clean-stop-a53dcb` on `merge-wave6`
  (`646a43041`); not merged, not pushed. Code and tests: `ce4b66a8a` …
  `e6b3317fc`; this page and the docs in the commits after it.
- **Host:** nuc (8 CPUs, 30 GB, Rocky 10, `/home` XFS, `fsync` of a 4 KiB
  write about 3.5 ms), private trees under `~/e2-stop/` and
  `~/efs-qual-matrix/`, copied with `git archive` (no `.git`), built with
  `EFS_GIT_ID=<commit>`. TCP, `O_DIRECT` fragments, three daemons on free
  loopback ports (`tests/faults/efsd`, 1 G quota) for the tests and timings;
  the matrix topology `nuc-private-3` (ports 18960–18962, 4 G quota) for the
  qualification gates. The user's nuc cluster (17432–17435) was not touched.
- **Raw outputs:** nuc `~/efs-artifacts/e2-stop-20261024/` with `SHA256SUMS`
  (SUMS_LINE).

## Why the revoke took 11 s

`efs_session_revoke` (`src/common/session_revoke.c`, used by the unmount and
by `efs-mgmt raft-session fence`) did four sequential steps for every touched
metadata shard: FENCE_LOC, ACK, and the lease/lock and append cleanup
probes, each a Raft commit or a fresh ReadIndex. A session touches a shard
per directory it creates in, so a busy mount touches hundreds to thousands;
the A11 log shows ACKs applied at about 150 per second, then 6 s of cleanup.
Two server costs made each step slower:

1. Every FENCE_LOC apply sealed the storage certificate again
   (`storage_fence_seal`), and the seal always ran three directory `fsync`s
   (`put_session_sync_parents`), even when the certificate already held that
   floor. That ran in the serial apply thread of every replica, so parallel
   clients could not hide it.
2. FENCE_LOC and ACK were one Raft entry per shard: 2 × 2,058 commits for a
   2,058-shard session, all ACKs in the session owner's group.

Measured with `tests/live/a11_revoke_timing.py` (three loopback daemons,
efs-fuse, DIRS directories with one 64-byte file each, `fusermount3 -u`,
revoke time from the client's own log timestamps):

| DIRS | touched | base `646a43041` | + parallel stages `ce4b66a8a` | + seal once `5796ea99c` | + word FENCE_LOC/ACK `7572e05a8` | + cleanup probe `a479b92dc` |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 300 | 281 | 4.87 s | 1.63 s | — | 0.72 s | 1.05 s |
| 1000 | 876–877 | 8.68 s | 2.87 s | — | 1.53 s | 1.45 s |
| 3000 | 2,058–2,059 | **63.10 s** | 41.29 s (fence 23.0, cleanup 17.7) | 17.64 s (10.3, 7.1) | 14.98 s (1.8, 13.0) | **10.87 s** (1.7, 9.0) |

The base grows faster than linearly (63 s for 2,058 shards). The A11
failure (765 shards, about 11 s on a loaded mount) corresponds to the 1000
row: about 1.5 s now.

What is left is real work: in this workload 876 of 877 shards hold one
old-epoch lease or lock row each (`cleaned=876` in the revoke log line; a
debug count showed `select_n=1` on every pending shard), and each is removed
by its own cleanup commit, 16 at a time. Batching that cleanup across shards
would change the cleanup log command and is a follow-up, not done here.

## Changes

**Revoke (`src/common/session_revoke.c`, `src/server/raft_host.c`,
`src/server/put_fence.c`, `src/meta/`).** No format compatibility (AGENTS.md):
a cluster formatted by the base cannot be revoked by this build or back.

- The per-shard stages run on up to `rpc->parallel` workers (16 for the
  client and `efs-mgmt raft-session fence`, at most 32). Stages stay ordered
  (FENCE_LOC+ACK of every shard before FINISH, cleanup after); the first
  failure stops new work, in-flight work finishes, every step is idempotent,
  and a partial revoke stays resumable by `efs-mgmt raft-session fence`.
- FENCE_LOC is `[uuid][epoch][word][mask]` and ACK `[uuid][word][mask]`:
  one entry per bitmap word and Raft group (odd and even shards are in
  different groups) instead of one per shard. The submit path refuses a
  mask with a shard of another group; ACK writes the session record once
  (`efs_session_ack_fence_word`).
- `storage_fence_seal` skips the parent directory syncs when this process
  already synced a certificate of the same path, UUID and a floor at least
  as high. Process memory only: after a restart the first seal syncs again;
  a failed sync is never remembered.
- New RPC-only request `EFS_MD_REQ_SESSION_CLEANUP_PROBE` (tag 94):
  for one word and group it runs the admission and fresh views of both
  cleanup phases once and answers which shards either phase could still
  act on (`efs_session_cleanup_select`, and the new read-only
  `efs_meta_append_session_idle`, factored out of the append retirement).
  The coordinator cleans only those shards; a failed probe cleans all of
  its word. A shard is skipped only on the same proof as before.
- D31's guarantee is unchanged: the barrier still seals every storage member,
  fences and ACKs every touched shard, finishes at the captured epoch and
  cleans every lease, lock and append row of the old epoch before it reports
  success; the group leaders still drain the revoked session's streams into
  the orphan ledger (`tests/test_put_drain.py` D31 cases PASS).

**Stop progress (`src/client/stop_control.h`, `efs_fuse.c`,
`stop_phase.c`, `node_cache.c`, `put_ticket_owner.c`, `inode_rpc.c`).**
efs-fuse keeps a stop record next to its control endpoint,
`/tmp/efs-control-<hash>.stop`, one line replaced by rename:
`pid= phase= step= total= rc= reason=`. Phases: `serving`, `flushing`,
`draining` (step = report attempt), `settling` (step = settle round),
`revoking` (step = shards fenced, then shards cleaned, of 2 × touched), and
the verdict `done` or `failed` with the first reason (a failed flush,
report, settle or revoke, for example `revoke rc=-1 phase=begin shard=0
touched=0`). The revoke log line now carries `ms`, `fence_ms`,
`cleanup_ms`, `cleaned` and `probe_failed`.

**Stop tooling (`scripts/client_processes.py`, `scripts/client.sh`).**
`retire` no longer waits a fixed 10 s: after the unmount (or SIGTERM to an
owned endpoint) it follows the stop record while (phase, step) changes. No
change for 30 s (`--stall`), or 600 s in all (`--deadline`), is a failed
stop: "client stop stalled … no progress for 30 s in phase revoking step
…; client remains alive, no forced kill attempted". A final `failed` record
fails the stop with the client's reason; a client that exits without a final
record, or keeps no record, fails too. The record must be a regular file of
the client's uid naming its PID. Nothing is ever force-killed.

**Fault hook.** `EFS_FAULT_REVOKE_BEGIN_FILE` (EFS_FAULTS builds only,
A6 inventory row added): while the file exists every session BEGIN is held;
once it reads `fail`, BEGIN is refused with `EFS_ERR_IO`.

## Tests

| Test | Where | What it checks |
| --- | --- | --- |
| `tests/test_stop_progress.py` | `make test-long` | Three daemons + efs-fuse. Busy mount: 1,500 directories with a file each (3,000 objects, about 1,300 touched shards), `scripts/client.sh stop` succeeds and the revoke logs `rc=0`. Small mount: stop under 5 s. Stalled revoke (hook): `retire --stall 3` reports no progress in phase `revoking`, the client stays alive; then `fail`: retire reports `failed: revoke rc=-1 phase=begin`, the client exits 1 |
| `tests/test_session_revoke_parallel.py` | `make test` | The real coordinator against a thread-safe fake owner, 600 shards: FENCE_LOC < ACK < FINISH < cleanup per shard, one group per FENCE_LOC, steps overlap with `parallel=16` and stay in the caller with 0, progress reaches the total, probe-driven cleanup (and per-shard fallback on a failed probe), failures stop the stage with the right phase; parallel ≥ 4× faster than serial at 100 µs per step |
| `tests/test_client_processes.py` | `make test-long` | Synthetic clients with stop records: progress for 3.3 s outlasts a 1 s stall window; a stall fails without a kill; a `failed` record fails with its reason; no final record fails; the deadline fails; another PID's record, a wrong owner and a symlink are not the client's record |
| `tests/test_put_storage_fence.c` | `make test` | A repeated seal at or below a synced floor does no `fsync`; another certificate path still syncs; failed syncs are retried |
| updated | `make test` | `test_session_finish_retry.py`, `test_session_fence_finish.py`, `test_put_drain.py` (probe from the real KV selections), `test_mgmt_session_routing.py` (35-byte FENCE_LOC retry) |

`tests/test_stop_progress.py` runs: at `ce4b66a8a` (parallel stages only,
3,000 directories) the busy stop succeeded through progress waiting after a
40.3 s revoke (`touched=2058`), where the fixed 10 s would have failed it; at
`a479b92dc` revoke 7.2 s (fence 0.6, cleanup 6.5), stop 7.6 s; at
`e6b3317fc` (1,500 directories, 8 threads) the whole test takes 66 s; small
stop 0.4 s, stalled retire refused after 3.3 s with the client alive, failed
revoke reported as `failed: revoke rc=-1 phase=begin shard=0 touched=0
(rc=1)`.

## Qualification gates (nuc-private-3)

The A11 topology `nuc-private-3` (driver-owned three daemons on
127.0.0.1:18960–18962, two mounts, 4 G quota per node), source
`~/efs-qual-matrix/src` = `e6b3317fc` (`efs-fuse ge6b3317fc`), with the
gates that load the client before `posix-durability`, as in the failing
runs:

```bash
cd ~/efs-qual-matrix/src && TMPDIR=$HOME/efs-qual-matrix/tmp python3 scripts/qualify.py run \
  --matrix scripts/qualify_matrix.json --topology nuc-private-3 \
  --gates identity,posix-core,posix-extras,posix-2client,posix-durability,e1-manifest,e2-client-stop \
  --out ~/efs-artifacts/e2-stop-20261024/qual-nuc-private-3 --work ~/efs-qual-matrix/work
```

| Gate | Verdict | Time | Stop steps |
| --- | --- | ---: | --- |
| identity | PASS | 0.3 s | |
| posix-core | PASS | 55 s | |
| posix-extras | PASS (4 cases, 1 SKIP out of scope) | 4 s | |
| posix-2client | PASS | 78 s | |
| posix-durability | **PASS** (was FAIL in 2 of 2 runs) | 78 s | busy mount A: `client.sh stop` rc 0 in **2.27 s**; revoke `touched=747 rc=0 ms=1927 fence_ms=647 cleanup_ms=1087 cleaned=291` (the probe proved 456 of 747 shards empty) |
| e1-manifest | PASS | 75 s | both mounts stopped (1.07 s, 1.57 s), daemons stopped and restarted, manifest verified before and after |
| e2-client-stop | PASS | 4.6 s | stop refused while a process held the mount (rc 1, mount kept), then rc 0 in 0.57 s, pending write verified after remount |

Driver exit 0. Before: A11 run `nuc-private-3-r3-ea3360a75`, posix-durability
FAIL, `touched=765`, revoke about 10.8 s (19:51:40.6 settle → 19:51:51.4
revoked), stop "client remains alive … no forced kill attempted"
(archive in nuc `~/efs-artifacts/a11-20261024/`). Raw:
`qual-nuc-private-3/` (its own `SHA256SUMS`, 147 files, sha256
`a95d8c12…ebaf`).

## Code gate

TEST_ALL_SECTION

## Reproduction

```bash
git archive claude/devel-e2-clean-stop-a53dcb | ssh nuc 'mkdir -p ~/e2-stop/fix && tar -x -C ~/e2-stop/fix'
ssh nuc 'cd ~/e2-stop/fix && make -j8 EFS_GIT_ID=<commit> all tests/faults/efsd'
ssh nuc 'cd ~/e2-stop/fix && TMPDIR=$HOME/e2-stop/tmp python3 tests/live/a11_revoke_timing.py 300 1000 3000'
ssh nuc 'cd ~/e2-stop/fix && TMPDIR=$HOME/e2-stop/tmp python3 tests/test_stop_progress.py'
ssh nuc 'cd ~/e2-stop/fix && EFS_TEST_JOBS=1 bash scripts/make_test_summary.sh'   # summary.txt
```

The base column is the same timing script copied into a `git archive` of
`646a43041`. The qualification run is the A11 command with the gate list
below (`--gates identity,posix-core,posix-extras,posix-2client,posix-durability,e1-manifest,e2-client-stop`),
from `~/efs-qual-matrix/src`. Run long commands in `screen`. Cleanup:
`edelete --force --delete ~/e2-stop ~/efs-qual-matrix` (the timing script
and the tests delete their own work directories; `--keep` keeps one).

## Limitations and remaining gates

- One host, loopback, TCP: regression evidence, not distinct-host
  qualification (A4). RDMA not driven.
- Cleanup of old-epoch lease/lock rows still costs one commit per shard that
  holds one; 2,058 such shards take about 9 s. A multi-shard cleanup batch
  is a follow-up.
- `tests/test_get_integrity.py` does not compile on `merge-wave6` (and
  identically on the base `646a43041`): `read_reprobe_down` is defined twice
  in the generated test. Not touched here.
- The 30 s stall and 600 s overall bounds are built in; a stop phase that
  legitimately makes no visible step for 30 s (for example one RPC waiting
  out a long deadline) is reported as stalled although the client may still
  finish cleanly afterwards; the record then says so.
