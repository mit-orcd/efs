# K40 · W162 · two clients on shared chunks: unit stages left undecided, BUSY until EIO (Oct 25, 2026)

**Purpose.** Checklist item K40 in
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) (section K),
work item W162 in [work-queue.md](work-queue.md). Found by K38
([evidence](k38-stale-span-replay-20261025.md)): two FUSE clients writing
one shared file (chunks shared across clients) got `EIO` from
`write()`/`fsync`. The client's REPORT units were answered BUSY for the
whole 8 s budget and `report-resolve` ended `outcome=unknown -> EIO`; the
servers showed unit transactions coordinated on the file's inode shard left
prepared until `txn-recover ... age=5-20s -> ABORT`. Alpha 1.0 requires that
an application write never gets `EIO` on a healthy cluster.

**Verdict: PASS (scoped: code fix, deterministic regression and loopback
evidence).** Branch `claude/musing-grothendieck-1a6c0c` on the K38 branch
`claude/vibrant-wiles-325684` (`77d973b1c`, itself on devel `5af760ff1`),
not merged, not pushed. Gate: see [Code gate](#code-gate). Live clusters and
the orcd-mini ior hard rerun are **NOTRUN**, for the parent session.

## Cause

A W77/K32 publication unit (`host_pub_unit_commit`, `src/server/raft_host.c`)
PREPAREs one stage per chunk on each chunk's lane group, one `TXN_BATCH`
entry per group. The group the REPORT's host leads is proposed locally and
its verdict comes back through a counted ref (`k`); the other group's batch
is forwarded to that group's leader, whose answer is the batch verdict and
leaves no ref. A `TXN_BATCH` applies each PREPARE on its own: when one
chunk's PREPARE loses its CAS (`publish_inner_as` dry run, STALE: the other
client published that chunk first) or meets another unit's stage (BUSY),
the batch answers STALE/BUSY while the other chunks' stages are installed.
If that forwarded batch was the first one submitted, `k == 0`, and the unit
skipped its ABORT (`if (k)`). The stages hold their chunks until the 5 s
recovery scan; every REPORT touching them — the other client's and this
client's own retry under a new publication id — gets BUSY, and the client's
8 s same-identity bound ends in `EIO`. With two clients the CAS loss is
routine, so the leaks pile up (base runs: up to 823 `txn-recover ... ->
ABORT` per run) and the BUSY persisted for minutes.

The same defect was found independently by the F1 follow-up session under a
30-minute D4 load and fixed in `d7a6eed7e` (branch
`claude/devel-f1-followups-ae2deb`, not merged); this branch cherry-picks it
(`601ce6a6e`, test refinement `6273e13ee`). The trace signature matches:
every failing unit in the K38 two-client traces logs `publication unit ...
rc=-14 verdict=0`, the forwarded submit's STALE with no ref.

## Fix

`host_txn_batch` records that an entry was submitted; the unit ABORTs on any
failure after a submission, not only when a ref exists
(`if (k || batch.submitted)`). ABORT is decide-once and RESOLVEs every
participant shard, so it also covers stages of a batch whose answer was lost.
No data path change: an aborted unit's records are re-sent by the client
(STALE replay or BUSY retry) exactly as before. `--clients 2` is now the
default of `tests/test_k38_shared_chunk_pressure.py` (`47939c29a`), budget
row 120 s (`30d7caf79`).

## Evidence

Environment: intel2 (8 CPU, 15 GB, Rocky 10), loopback, three `efsd` of
the `EFS_FAULTS` build (`tests/faults/efsd`) with private stores under
`$HOME/efs-k40/` on XFS, buffered I/O, TCP, `--quota 1G`, free ports (never
17xxx), efs-fuse clients with `EFS_DCACHE_HARD_BYTES=32 MiB`,
`EFS_DCACHE_BYTES=1 MiB` (test defaults). Builds from `git archive` of each
commit; `make -j8 all tests/faults/efsd`.

| Run | Tree | Result |
| --- | --- | --- |
| `test_k38_shared_chunk_pressure.py --clients 2 --writers 16 --records 240`, 4 runs | base `77d973b1c` | **3 FAIL** (writer `EIO`; 45–823 `txn-recover -> ABORT`, 250–3,705 `publication unit ... rc=-14` lines per run), 1 PASS |
| same, 10 runs | fix + a log line on the new ABORT path (`K40DBG`, not committed) | **10 PASS**, 19.5–43.0 s; 0 `txn-recover -> ABORT`; the new path fired 13–133 times per run (sampled run: 35 STALE, 2 BUSY forwarded refusals) |
| same test, default arguments (24 writers, 2 clients, 200 records), 3 runs | `47939c29a` | **3 PASS**, 37.9–43.4 s |
| `tests/test_pub_unit_forward_abort.py` (deterministic: `EFS_FAULT_FWD_TXN_STALE` answers one forwarded `TXN_BATCH` STALE after it applied), 5 runs | fix with `if (k)` restored (hook kept) | **5 FAIL**: 4 × `fsync` `EIO`, 1 × write+fsync 5.7–6.6 s in 6 of 12 rounds (bound 3 s) |
| same, 5 runs | fix | **5 PASS** |

The base failure rate (3 of 4) matches the K38 report (~3 of 7 on devel
`5af760ff1`). Raw output: intel2 `~/efs-artifacts/k40-20261025/runs/` (see
[Code gate](#code-gate) for the hash); the K38 failing traces are in intel2
`~/efs-artifacts/k38-20261025/two-client-busy/`.

### Reproduce

On a Linux host with `/dev/fuse` and `fusermount3`, in a private tree:

```bash
make -j8 all tests/faults/efsd
```

```bash
TMPDIR=$HOME/efs-tmp python3 tests/test_k38_shared_chunk_pressure.py --clients 2 --writers 16 --records 240
```

```bash
TMPDIR=$HOME/efs-tmp python3 tests/test_pub_unit_forward_abort.py
```

`EFS_K38_KEEP=1` keeps the stores and logs; the failure signature is client
`report-loop ... busy=1 ms=8000 rc=-13` and `report-resolve ... outcome=unknown
... -> EIO`, server `raft-host: publication unit ... rc=-14 verdict=0` and
`raft-host: txn-recover shard=... coord=... -> ABORT`. For the red run,
restore `if (k)` in `host_pub_unit_commit` and keep the
`EFS_FAULT_FWD_TXN_STALE` hook. Cleanup: the tests stop their daemons and
remove their directories unless kept.

## Code gate

intel2 `make test-all` through `scripts/make_test_summary.sh` at
`EFS_TEST_JOBS=8`, tree `30d7caf79` (`git archive`, private dir under
`$HOME`, `TMPDIR` there). Binaries: `tests/faults/efsd` sha256
`9f1dffc5…a6f4`, `efsd` `a454996b…e109`, `efs-fuse` `d2a6d292…f1c3`.

- **Run 2: 484 commands, 0 FAIL, 16 NOTRUN (host limits: no RDMA device,
  valgrind, sanitizer runtime, cluster scripts, `EFS_FAULTS` unit build;
  status INCOMPLETE only for those), 574 s.**
  `test_k38_shared_chunk_pressure.py` (two clients) 38.9 s,
  `test_pub_unit_forward_abort.py` 20.0 s.
- Run 1: 484 commands, 1 FAIL, 16 NOTRUN, 585 s. The FAIL is
  `test_k33_payload.py` step 5 (`node k down` read `EIO`), the known
  intermittent fixed on the unmerged branch `claude/kind-nash-0f2244`
  (stale protection counts in `efs-mgmt status`; base rate there 8 of 30). Alone, interleaved on intel2: base
  `77d973b1c` 2 of 8 FAIL, this branch 3 of 8 FAIL, same read-path
  signature; not related to the publication unit ABORT.

Raw output (run logs, gate summaries and timings, the kept stores of the
failing base runs): intel2 `~/efs-artifacts/k40-20261025/` (10,848 files,
943 MB), `SHA256SUMS` sha256 `ba8bf80a…888d`. The private tree is deleted.

## Limitations and remaining gates

- Loopback on one host is regression evidence, not qualification. The
  orcd-mini ior hard rerun (RDMA, one shared file, now also with two
  clients) is NOTRUN for the parent session.
- The fix is shared with the unmerged F1 follow-up branch; whichever merges
  second keeps one copy of `d7a6eed7e`.
- Contention itself remains: two clients publishing the same chunks still
  abort and retry each other's units (13–133 aborted units per run); they now
  release promptly, so progress depends on the clients' retries, not on the
  recovery scan.
