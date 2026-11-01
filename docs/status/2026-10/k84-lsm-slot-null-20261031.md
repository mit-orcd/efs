# K84: UBSan "member access within null pointer" in `lsm_slot_in`

**Verdict: K84 FIXED (ASan lane PASS on the fixed tree, FAIL on the base).** The full
`make test-all` gate on amd ended `TOTAL 583 FAIL 4 NOTRUN 3` under load from a sibling
session; three of the four pass alone and the fourth fails identically on the unfixed
base (see "Gate"). Purpose: close register row K84 from the
[batch F rehearsal](batch-f-rehearsal-20261031.md) (finding 3). Acceptance is the batch F
lane F2 (ASan+UBSan+LSan) on the candidate. This is not a release verdict.

## What it is

**Not a real null dereference.** The slot lookup finds its slot, and the sanitizer report
comes from a code-generation defect in GCC 15.2's `-fsanitize=null` instrumentation. The
program computes the right answer; the sanitizer aborts it anyway because the gate runs
with `UBSAN_OPTIONS=halt_on_error=1 exitcode=67`.

Evidence on the base (devel `bc2af2bf8`, gcc 15.2.0, `-O1 -march=x86-64-v3`):

- `gdb` at `__ubsan_handle_type_mismatch_v1` (`Pointer=0`) in `kv_flush_locked` ->
  `lsm_view_reclaim`: the thread's `lsm_owned` table is
  `{l = <this kv>, depth = 1, yielded = 1}, {0}, {0}, {0}`: the writer slot exists. After
  the report the program continues and `yielded` is `0`, i.e. `lsm_owner_slot(l, 0)->yielded
  = 0` stored into the real slot.
- A scratch build that replaced the store with `o = lsm_owner_slot(l, 0); if (!o) abort();`
  never aborted and still produced the same three reports.
- The disassembly of the inlined `lsm_slot_in` in `kv_flush_locked` has, for the null
  check on the address of the thread-local array element:
  `lea rax,[rax-0x40]` followed directly by `je <report>`. `lea` sets no flags, and the
  `test rax,rax` is missing, so the `je` reads the flags left by the previous
  `pthread_mutex_lock` return and fires when they happen to say zero. The report block
  passes the constant `0` as the pointer. Which calls fire is therefore not deterministic
  in principle; in this tree the `lsm_view_reclaim` after a yielded flush did, every time.
- A reduced program (same shape, one or two translation units) does not reproduce it, so
  it needs the full inlining context of `kv_flush_locked`.

Consequence for the batch F symptom: n1 and n3 of `d4_progress` died of `exitcode=67`
(UBSan with `halt_on_error=1`) on a spurious report, not of corrupt state. The daemons were
not dereferencing null.

## Fix

`src/kv/kv_lsm_internal.h`: `lsm_slot_in` was a function over `struct lsm_owner *t`
called with the TLS arrays `lsm_owned` / `lsm_reading`; after inlining, GCC instruments
`t[i]` as a possibly-null pointer and emits the broken check. It is now a macro over the
named array (`(arr)[i_]`, `&(arr)[i_]`), so no pointer parameter exists to check. The lookup
logic (first slot with `l == owner && depth`, else first free slot, claimed when `add`) is
unchanged. All callers (`lsm_owner_slot`, `lsm_lock`, `lsm_rlock`, `lsm_runlock`, the
reader depth helpers) keep their call shape: `lsm_slot_in(lsm_owned|lsm_reading, l, add)`.
No `no_sanitize` attribute, no new test: `kv_lsm` and `pack_ledger_recovery` already take
the path (the empty-slot and yielded-flush cases), and they are the negative control.

Tried first and rejected: a pointer-walk loop (same report), `__attribute__((nonnull(1)))`
(the broken check moves to the argument at `lsm_owner_slot`).

## Verdicts

Base is devel `bc2af2bf8`; the fixed tree is `75bb45f46` (its child, one header changed).

| Run | Result |
| --- | --- |
| Base, `EFS_SAN_ONLY="kv_lsm pack_ledger_recovery k36_recovery"` asan | `VERDICT: FAIL (3 run, 3 failed, 171s)`, each `1 UBSan errors` |
| Fixed, same command | `VERDICT: PASS (3 run, 0 failed, 42s)` |
| Fixed, `EFS_SAN_ONLY=d4_progress EFS_SAN_TIMEOUT=900` asan | `PASS d4_progress (96s)`, `VERDICT: PASS (1 run, 0 failed, 96s)`: efsd n1 and n3 survive; no timeout |

The d4_progress lane ran 3,604 s to its timeout in the rehearsal; fixed it takes 96 s.

## Gate

`TMPDIR=$HOME/efs-taskBW/tmp EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` on the
fixed tree on amd (shared with Task BV's runs): `TOTAL 583 FAIL 4 NOTRUN 3 STATUS FAIL
(1622s) OVERBUDGET 6 BUILD 0s`. NOTRUN: `test_cluster_stop` (no `EFS_CLUSTER_SCRIPTS`),
`test_qualify_rdma_live` (no RDMA), `test_store_durability` (TMPDIR not XFS): the same
environment limits as the rehearsal. The four FAILs, rerun alone on the fixed tree:

| Test | Gate | Alone |
| --- | --- | --- |
| `test_k54_member_down_fence` | AssertionError (fence incomplete) | PASS, 27 s |
| `test_stop_progress` | `fusermount3 -u ... busy` (rehearsal fault 3, gvfs) | PASS, 15 s |
| `test_stop_blocked_stderr` | TIMEOUT 900 s | PASS, 36 s |
| `test_settings_listing` | efs-fuse `-f` did not exit in 30 s after `fusermount3 -u` | FAIL in 33 s; **FAIL the same way on the unfixed base** (host mount/unmount, fault 3 family) |

The `0 FAIL` line is therefore not reached on this host: three failures are load
intermittents, one is a host limit that predates the fix. An unloaded candidate host
(headless session, own XFS TMPDIR) is needed for a clean `TOTAL ... FAIL 0`; this is a
remaining gate, not a pass.

## Reproduction

Prerequisites: Linux host with gcc 15, ASan/UBSan runtimes.

```bash
git archive <commit> | ssh amd 'mkdir -p ~/efs-taskBW/src && tar -x -C ~/efs-taskBW/src'
ssh amd 'cd ~/efs-taskBW/src && TMPDIR=$HOME/efs-taskBW/tmp EFS_SAN_ONLY="kv_lsm pack_ledger_recovery k36_recovery" bash tests/sanitizer_check.sh asan'
ssh amd 'cd ~/efs-taskBW/src && TMPDIR=$HOME/efs-taskBW/tmp EFS_SAN_ONLY=d4_progress EFS_SAN_TIMEOUT=900 bash tests/sanitizer_check.sh asan'
```

Expect FAIL (UBSan, `kv_lsm_internal.h:305`) on `bc2af2bf8` and PASS with the fix.
Cleanup: `edelete --force --delete ~/efs-taskBW/src ~/efs-taskBW/tmp`.

## Raw location

amd `~/efs-artifacts/taskBW-20261031/` (not in Git; 38 files, 272 KB):
`base-asan3/`, `fixed-asan3/`, `fixed-d4/` (sanitizer reports and entry outputs),
`*.log` lane logs, `summary.txt`, `timings.tsv`, `fail-*.log`, `rerun-*.log`, `rerun.txt`,
`SHA256SUMS`. `sha256sum SHA256SUMS` =
`7834e35996e3f28823313a6f62e3e2f9d37c2d4582a2b217cfe64a8f4b34f135`.

## Remaining

Batch F lane F2 (full ASan+UBSan+LSan) on the candidate; a gate run on an unloaded
host. A compiler bug report for GCC 15 (missing `test` after `lea` in the TLS-address
null check) is not filed.
