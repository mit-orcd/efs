# K53: B6 full-quota oracle, one writer succeeded (Task BE)

**Verdict: K53 CLOSED, harness oracle defect; product admission not shown
faulty. Not reproduced on the current tree.** Item: K53, quota admission
correctness (blocks alpha: yes, now released). Branch
`claude/devel-be-k53-eb5e89`, from devel `7514b2145`; not pushed.

## What K53 was, in POSIX terms

`tests/test_b6_unavailable.py` (`EFS_B6_MODES=full`) gives node 3 a 16 MiB
quota (`--quota 16M`, one 64 KiB fragment per 128 KiB chunk per node). It
fills node 3 until a `write`/`fsync` returns `ENOSPC`, then writes W0..W2
(300,000 B) and Q0..Q3 (1 B to 16 KiB) one after another, each as
open + write + fsync + close, and required `ENOSPC` from every one. On the
ORCD dd-profile gate W0 returned success and the rest `ENOSPC`.

POSIX does not promise `ENOSPC` forever: if blocks are freed (another
process unlinks, a deferred reclaim lands) a later `write` may succeed. So
the open question was whether node 3 freed room after the refusal (the
oracle is too strong) or admitted bytes it had no room for (a `store.c`
defect).

## Attribution: room was freed 360 ms before W0, by the packer

Source: the retained failed store on fcstor007 (`3769f44bb`, case-464),
read-only. Times are `2026-10-28T19:48:xx`Z; "dir mtime" is a directory
modification time in node 3's fragment tree (`n3/data`).

| Time (s) | Event |
| --- | --- |
| 43.514 | n1 packer pass: `packed_total=4` |
| 44.302–.307 | inode 176129 (a 1 MiB fill file, 8 chunks) puts 5 fragments on node 3 |
| 44.314 | mnt-a: `fsync: quota exceeded` for inode 176129; the fill ends |
| 44.408 | node 3 `segments/g0/c01/…1.2` (16,384 B) created |
| 44.410 | n1 packer pass: `packed_total=8` (= 2 × the 4 small seeds) |
| 44.4256, 44.4266 | dir mtimes of the `0001` and `0004` inode directories: entries removed. Eight fragment inode directories are absent from node 3 that the sequence 4097…184321 (step 4096) should hold: 16385, 20481, 24577, 28673, 45057, 49153, 53249, 57345 |
| 44.642–.749 | mnt-b unmount: session revoke `cleaned=1` |
| 44.784–.793 | node 3 admits 8 fragments: 3 more for 176129 (the client's retried flush), 3 for 180225 (W0), 2 for 184321; W1 then gets `ENOSPC` |
| 44.795–.812 | W1.. and the rest: `fsync: quota exceeded` |
| 59.462 | node 3 `gc-health`: `removed=8 file_bytes=557056 payload_bytes=524288` |

Eight freed fragments (8 × 64 KiB = 524,288 B) and eight admitted fragments
match, and every free is dated before every admission (about 360 ms apart).
The ledger and the files agree (255 fragments, `usage.bin` 16,736,256 B,
room 40,960 B at the end), so a stale or undercounted `local->used` (shape
b) is not indicated. This is shape (a): room legitimately appeared between
the fill and W0. The fixture's under-fill (shape c) is also real, in a
different way: that build filled with 1 MiB files, so the last one was
refused part-way.

The 8 freed fragments are the old fragments of the small seed files the
packer had just packed (inferred from the count: `packed_total` 8 equals
2 × `len(SMALL)`; the test does not name them in the logs). The exact
deleting call (retire pass, GC, or the mnt-b cleanup) is not named in the
logs, which print no per-unlink lines; the reclaim journal was retired at
shutdown. That is a limit of the retained capture, stated plainly.

Both reported occurrences ran builds older than the harness fix
`d2edef6e9` (Oct 28 21:06 EDT, "establish stable fragment-granularity quota
exhaustion before ENOSPC checks", which waits for every small seed to be
packed and its old fragments retired and fills with 128 KiB files): the
dd-profile build `3769f44bb` (15:31 EDT) and Task X's node9900 build
`86035a633` (built 19:32Z). Devel `7514b2145`, where this task started,
already contains that fix.

## Reproduction

Not reproduced. `scripts/b6_full_loop.sh` with `EFS_PACK_DBG=1` on
`7514b2145`: nuc 40/40 PASS (170–201 s per run) and fstor007 40/40 PASS
(two loops of 20; native build with the private libfuse prefix, stores under
`/tmp`). With the attribution above this is the expected result.

## What changed (tests only)

- `tests/b6_full_oracle.py`: `judge()`. A success on the full node counts
  only if the bytes it admitted fit the room left at the fill plus what node
  3 reclaimed since (`efs-mgmt gc-status` `reclaimed_payload_bytes`, or a
  visible ledger decrease); a ledger above its quota, or an unexplained
  success, fails. An unexplained success is the shape-b product finding.
- `tests/test_b6_full_oracle.py` (in `make test`): replays the retained K53
  trace (fill 16,736,256 B, W0 success, 524,288 B reclaimed). The old rule
  (every writer `ENOSPC`) rejects that trace; `judge()` explains it. It also
  rejects the same success with nothing reclaimed, a ledger above quota, and
  another errno. This is the failing-on-base test; a live one cannot fail on
  base because nothing reproduces.
- `tests/test_b6_unavailable.py` (full mode): a quiet-ledger fill (3 s still,
  refill if room appeared), a top-up before the writers, a usage trail sampled
  before each writer and after the last, reclaim counters at the fill, the
  failing call (open/write/fsync/close) kept per refused writer, all in
  `b6-result-full.json` (`usage_trail`, `writer_stages`, `full_verdict`).
  A new `room_scenario`: unlink one 128 KiB fill file, wait for a quiet
  ledger (it frees exactly 65,536 B), then exactly one 128 KiB write succeeds
  and the next gets `ENOSPC`, ledger never above its quota.

- `tests/budgets.tsv`: the full-mode row goes from a stale 85 s to 400 s
  (318 s at 8 jobs on nuc, 220–232 s alone; the refused writers wait 13–20 s
  each, the unlink check adds about 45 s); the recipe test still passes
  (budgeted gate 1,616 s of 1,650 s).

Quiet is a stabilizer, not the fix: a late free is now explained or flagged
instead of forbidden.

## Not done

- A runtime reclaim hold (the review's step 3). Holding every reclaim source
  needs five sites: the reclaim deleter (`rj.hold` exists, test-only), the
  accept-time credit (`credit_record` debits `used` at acceptance, so
  holding the deleter alone does not stop room appearing), the GC reaper
  pass (`EFS_FAULT_GC_DISABLE` is startup-only), `put_orphan` delete and the
  packer's retirement. Left as a follow-up rather than guessed at.
- The shape-b admission-invariant scenario (concurrent writers, sum of
  fragment bytes on node 3 equals the ledger, at most the quota) needs that
  hold; also a follow-up. Today shape b is covered only by the ledger-never-
  above-quota check and the explained-success rule in every full run.

## Evidence and results

| Run | Tree | Result |
| --- | --- | --- |
| nuc loop, 40 runs | `7514b2145` | 40/40 PASS |
| fstor007 loops, 2 × 20 | `7514b2145` | 40/40 PASS |
| nuc, quiet-wait harness only (superseded) | `7514b2145` + early test edit | 31/31 PASS, stopped on instruction |
| nuc, final harness, 3 runs | `7514b2145` + this branch's test files | 3/3 PASS, 220–232 s |
| `make test-all`, 8 jobs, nuc, `545e5012d` | this branch | **TOTAL 568 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (1609s)**, OVERBUDGET 37, build 62 s; NOTRUN = host limits (no RDMA device, no valgrind/memcheck.h, no cluster scripts) |
| `make test-all`, 8 jobs, nuc, `b00e92c72` (first run) | this branch | TOTAL 568 FAIL 2 (1248s): `test_d4_progress` and `test_d4_plateau` timing failures under load; both PASS alone on idle nuc (81 s, 164 s); not B6 |

Raw captures (not in Git): `~/efs-artifacts/taskBE-20261030/` on the Mac
(`fcstor007-case464/`: the n1/n2/n3 and mnt-a/mnt-b logs and node 3's
fragment-tree mtimes, `SHA256SUMS`); on nuc `nuc/`, `nuc-fixed/`, `nuc-new/`
(each `summary.txt`, `runN/`; `nuc/summary.txt` sha256 `603de0d1…`,
`nuc-new/SHA256SUMS` `c89bce53…`); on fstor007 `fstor1/`, `fstor2/`
(`summary.txt` `b3475f37…`, `25a21f11…`). The fcstor007 failed store stays
as it is under
`/tmp/efs-dd-regression-3769f44bb/tmp/case-464/efs-b6-9z7vya65/full`; it is
no longer needed for this verdict.

## Reproduce

```sh
# attribution: the replay of the retained trace (fast, in make test)
python3 tests/test_b6_full_oracle.py
# the live case, on a Linux host with /dev/fuse (private TMPDIR, keep outside Git)
EFS_B6_MODES=full EFS_B6_ARTIFACTS=$HOME/efs-artifacts/NEW python3 tests/test_b6_unavailable.py
# the loop used for the not-reproduced result
EFS_PACK_DBG=1 scripts/b6_full_loop.sh TREE OUTDIR 40
```

Environment: nuc (Linux 6.12 el10, 8 CPUs, 30 GiB, loopback TCP, buffered
I/O, three daemons plus two FUSE mounts, node 3 `--quota 16M`) and fstor007
(Linux 5.14 el9, 96 CPUs, libfuse 3.16.2 private prefix). The failed
capture ran on fcstor007 with RDMA. Cleanup: the test removes its private
stores; delete `~/efs-artifacts/taskBE-20261030/` when the verdict is no
longer cited.

## Limits

K53 was never reproduced after the fix, and the retained capture orders the
frees against the admissions but does not name the deleting call. The
verdict rests on the dated ordering, the matching byte counts and the
ledger/file agreement, not on a repeat. If a full run ever shows an
unexplained success in `full_verdict`, reopen K53 as shape b.
