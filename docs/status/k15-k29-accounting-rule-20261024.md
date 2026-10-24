# K15 / K29 · append harness accounting rule (Oct 24, 2026)

**Purpose.** K15 and K29 in
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md): the append
harness `tests/live/append_failover.py` counted every non-injected failure,
and the hole it left, as a mismatch. On intel2 a load stall with no leader
change and no data loss therefore FAILed the lost-reply gate
([K15 live gates](k15-live-gates-20261022.md)). Add a rule that separates
accounted failures from lost records, report the accounted ones separately,
and check the rule in `make test` with negative controls.

**Verdict: PASS (scoped).** The rule and its unit check are in place. The
live failover gates are **NOTRUN** (parent session).

## Tested source and build

- Branch `claude/devel-k9-append-836bbe` on devel `19313ab06`; tested commit
  `6f57ee8b1` (this page: the commit after it). No product source change. No new KV kind, Raft tag or wire
  message type.
- The branch also keeps `tests/test_append_interleave.c` in `make test`
  (0.1 s, passes; a seeded model of the reservation state machine left from
  the stopped K9 work, kept per the scope change because it is cheap).
- Builds on amd from `git archive` trees (`EFS_BUILD_ID=unknown`, gcc,
  x86_64, `-O3 -march=native`).

## Environment

amd (`amd-395`): 32 CPUs, Ubuntu, kernel 7.0, `/tmp` on tmpfs, `/dev/fuse`
available. Private loopback daemons on free ports (never 17xxx/27xxx), TCP,
buffered I/O. No running cluster, mount or `efs` user was touched. Another
session's W123 campaign ran on amd at the same time (load average about 6).

## The rule

`account()` and `client_events()` in `append_failover.py`; `verify()` now
takes the hole interval that the abandons explain.

- **Accounted, not a mismatch:** a write the client saw fail (errno), whose
  record is absent, with every zero hole explained:
  - an injected lost reply leaves exactly one hole;
  - a real failure whose abandon retired the reservation (`rc=0 retired=1`)
    leaves exactly one;
  - a real failure whose abandon retired nothing (`rc=0 retired=0`) or did
    not complete (BUSY) leaves at most one, because the reservation may never
    have applied, or session or last-close cleanup resolved it;
  - a failure with no abandon (refused before reserving) leaves none.

  Abandons on either mount count. For an identity logged twice, the last line
  decides.
- **Mismatch:** an acknowledged record missing, duplicated, torn, corrupt or
  out of order; a failed write's record visible; an unknown block; more holes
  than the abandons explain, or fewer than the retirements require; an
  injected loss without an `rc=0` abandon of its identity; more abandoned
  identities than failed writes; a size that is not 4 KiB × (acknowledged +
  holes); warm and cold views that differ.

The report keeps `verdict`, which still includes the unchanged W52
REPORT-timeout and fsync-bound checks. It adds `integrity` (PASS/FAIL from
the rule alone), `accounted_failures` and `checks.accounting` (injected
losses, failed writes, accounted abandons by kind, failures without an
abandon, the explained hole interval, mismatches). The lost-reply check no
longer requires failed writes = injected losses. It requires at least one
injected loss, an `rc=0` abandon for each, and peer writers that completed.

Applied to the counts the K15 live-gate page records for the intel2 `lost-1`
run (its raw logs are gone): 29 failed writes, 4 injected losses, 5 holes,
2 BUSY abandons, no lost record. That gives integrity PASS with 25
accounted failures instead of a mismatch. The run's `verdict` still FAILs
on its 12 REPORT receive timeouts (the W52 check).

## Unit check

`tests/test_append_failover_accounting.py` (`make test`, under
`test-report-pressure`) uses synthetic 4 KiB record files and client-log
lines.

Positive cases:
- a load stall with three real failures: one retiring abandon, one that
  retired nothing (with and without a hole), one refused before reserving;
- the intel2 shape: two injected losses plus a BUSY abandon, with and without
  cleanup resolving the BUSY one as a hole;
- a repeated identity;
- abandons on the peer mount.

Negative controls, all refused:
1. an acknowledged record overwritten by a hole;
2. one missing without a hole;
3. a hole with no abandon;
4. a retiring abandon whose hole vanished;
5. a failed write that landed;
6. an injected loss with a BUSY abandon;
7. a duplicate;
8. more abandons than failures;
9. broken per-writer order.

## Harness port to the K33 CLI

`append_failover.py` (and the oracle helper `append_oracle_cluster.sh`)
still started efsd with `--node-id`, which K33 increment 2 refuses, so
neither could start a cluster. Both now work this way:
- node 1 starts as `efsd ADDR:PORT STORE`;
- nodes 2 and 3 first start as `efsd --join SEED --listen ADDR:PORT STORE`;
- every restart is by endpoint, with `--discovery-port 0`;
- they wait for formation (`raft-getattr` of the root) instead of
  `raft-mkfs`;
- `--rel` resolves to an absolute path, which fixes the `--rel .` problem
  noted on the K15 live-gate page.

Smokes on amd (not gates, run before the scope change):
- `--records 400 --writers 4`: **PASS**, 400 records, three identical views,
  27.9 s.
- With a fault-built `efs-fuse`, `--records 1000 --writers 4 --peer-writers 1
  --lose-reserve-every 37 --lose-reserve-max 4`: **PASS**, 4 injected
  losses, 996 acknowledged, 4 holes, three identical views, 78 s,
  `integrity` PASS, 0 accounted failures.

## Gate

`EFS_TEST_JOBS=4 scripts/make_test_summary.sh` (= `make test-all`) on amd, on
the branch tree and on the untouched base `19313ab06`, both from `git archive`:

| Tree | Result |
| --- | --- |
| branch `6f57ee8b1` | 444 commands, FAIL 10, NOTRUN 5 (158 s, build 12 s) |
| base `19313ab06` | 442 commands, FAIL 11, NOTRUN 5 (160 s, build 12 s) |

- Every branch FAIL also fails on the base (amd host limits, the set the K32
  evidence recorded): `tests/faults/test_create_retry_fault.py` (30 s case
  timeout), `test_declaration_gate`, `test_rdma_xprt`, and seven
  `test_rdma_segmented` cases. The base additionally failed
  `tests/test_k32_entry_count.py` once (an admit-count assertion; it passed on
  the branch, and the branch does not touch K32 code).
- NOTRUN, the same in both: `test_cluster_stop.py`, the `test_follower_bound`
  stalled compactor, `test_leak_gate.py` (no valgrind), the
  `test_store_durability` async case (TMPDIR not XFS), and
  `test_make_test_summary_runner.py`.
- The new commands pass on the branch: `tests/test_append_failover_accounting.py`
  (0.0 s) and `./tests/test_append_interleave` (0.1 s).
- `cd docs && python3 gen-architecture-full.py` and
  `python3 docs/check-architecture.py`: all 5 checks passed.

## NOTRUN gates (parent session)

- K15 live gates: W52 20k, W64 `step-down` and `kill`, lost reply, lost
  reply + step-down, second runs, W52 A/B; K29 dedicated-host rerun.
- Expect `--failover kill` to fail its final fsync on availability. Seen on
  amd during the stopped K9 work: after a SIGKILL and restart the store logs
  "recovery uncertain; COMMIT returns EIO until verified recovery", and
  every fsync on the mount returned EIO for at least 60 s. An fsync while a
  node is down also fails its every-node COMMIT (`data_commit.c`). Both are
  fail-closed by design. Logs: amd `~/efs-artifacts/k9-20261024/s5-sticky-evidence/`.

## Reproduction

```
git archive <commit> | ssh amd 'mkdir -p ~/k15t && tar -C ~/k15t -xf -'
cd ~/k15t && make -j32 all
python3 tests/test_append_failover_accounting.py
python3 tests/live/append_failover.py --rel . --root /tmp/k15af --port 18760 --records 400 --writers 4
EFS_TEST_JOBS=4 scripts/make_test_summary.sh      # make test-all
```

Cleanup: `edelete --force --delete` the tree and `/tmp/k15af`.

## Raw outputs

Outside Git on amd under `~/efs-artifacts/k15-20261024/`, with `SHA256SUMS`:

| File | sha256 |
| --- | --- |
| `branch-make-test-all-summary.txt` | `928d833d8945a689904bb03b04ee90ba3b012c160fd1dad4e196484ca5936968` |
| `branch-make-test-all-timings.tsv` | `cfd328f291291ce5359bd876fdb20d841c67db178beb16fb8accbf6a110a61ff` |
| `base-make-test-all-summary.txt` | `d3f45d884e519b3818df5c7081131aea3df5c1c8ba0f00fff86e6ba1ce1fbf87` |
| `base-make-test-all-timings.tsv` | `1deb306d4545f0bdb0700c804826d63c303505f6521b8b0dd7b0533f83543964` |
| `base-fail-test_k32_entry_count.log` | `d453d7ca1960a93f5247a72b7553999d4378aead997ce146ec4b2802614dbf99` |
| `failover-smoke-plain.out` | `5ee60fc9f918bbf382e38c46c158b37ea04e7aec9ec7e5542bf4ac8e06adcf4a` |
| `failover-smoke-lost.out` | `f59dd2b64f9ec01e35b686f6a0cb7e0a7e291cd3b1b5e20fdbae3c1cd9e7fccd` |

`SHA256SUMS`: `0e20e87f99343745290b38b0152b17642c8f3f5de6b6892b06894b0eab45d21f`.
The work trees were deleted after the runs. The other branch failure logs
are summarized by the FAIL lines in the summaries.
