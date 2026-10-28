# Task O — directory operations exhausting transient BUSY retries

Branch `agent/devel-task-o`, base devel `063154e0e`, product fix `b78955995`:
qualification IN PROGRESS; no full-gate PASS claimed. Five standalone K36
and W159 runs each PASS. Base loaded core: 211/218, seven case timeouts;
deterministic RMDIR negative control FAIL
with EBUSY after 16 refusals. Evidence: this page. Real-host/RDMA acceptance
NOTRUN (assigned xorinox private TCP loopback only). K50: confirmed client
retry-cap defect, fix committed; acceptance pending.

## Purpose and contract

Task O follows W159's directory EBUSY recurrence, separately from W166's
PUT/REPORT pressure repairs. Transient metadata admission refusal must not
exhaust an unrelated attempt-count limit before the operation's bounded
callback deadline. Errors, ambiguous outcomes and exhausted deadlines stay
errors; no application retry or changed POSIX case timeout qualifies a PASS.

## Source and environment

Base product devel `063154e0e`; stress controller `243b92113` changes no
product code. Negative control `f75257527` adds only a fault-build server
hook, its inventory entry and the deterministic test. It retains the base
client unchanged. Product changes at `b78955995`; `50a32db2a` additionally
preserves W159 failure fixtures and emits the original client/server logs.
Binary identities and final tested source manifest are owed after the gate.

Assigned host xorinox, Linux, private trees beneath
`/home/maik/efs-taskO/`, raw outputs beneath
`/home/maik/efs-artifacts/taskO-20261027/`. Four loopback fault daemons,
normal FUSE client, TCP, O_DIRECT, 1 GiB quota per member, online addition
from three to four members; no existing cluster, `/data1/efs` store, user
mount or reserved port is changed. Sixteen CPUs; initial host load was
0.02 with no other test-account processes or screen sessions.

## Reproduction and findings

### Unmodified product under load — failures retained, causality limited

Build and run the committed controller from the source root:

```sh
make -j8 test-bins
# load1 and load2 are independent built copies of this same source tree.
python3 scripts/qualify_dir_ops.py --work ~/efs-taskO/base-stress \
  --output ~/efs-artifacts/taskO-20261027/base-stress --rounds 10 \
  --load-tree ~/efs-taskO/load1 --load-tree ~/efs-taskO/load2
```

Each load tree runs `scripts/make_test_summary.sh` with
`EFS_TEST_TARGET=test-long EFS_TEST_JOBS=8`; the controller runs the unchanged
`posix_suite.py MOUNT --suite core`, with its default deadlines, against a
separate four-member cluster. First core result: **211/218, seven FAIL,
156.5 s**. Failures: `names_crazy_roundtrip`, `names_crazy_dirs`,
`dir_deep_nesting_beyond_64`, `concurrent_creates_same_dir`,
`concurrent_write_and_readdir`, `mtime_monotonic_many_writes` (15 s each)
and `concurrent_appends_two_proc` (30 s). These are measured deadline
failures, not proof of the directory BUSY conversion. The script stops core
rounds at the first failed round; it does not re-run until a PASS erases it.
Both load pools finish and their failures remain evidence too.

### Deterministic application-visible EBUSY — confirmed K50

`tests/test_dir_ops_busy.py` starts four private fault daemons and one normal
FUSE mount. The fault-only `EFS_FAULT_RMDIR_BUSY_FILE` is a shared finite
counter, consumed under a file lock, only for `task-o-busy` RMDIR and before
reads, receipts or proposals. It injects twenty admission BUSYs across all
members combined, then permits the ordinary operation. No log, store, wire
format or durability behavior changes. Each refusal logs the remaining count
and operation sequence.

```sh
# Negative-control commit f75257527; unchanged base client.
make -j8 all tests/faults/efsd
python3 tests/test_dir_ops_busy.py
```

The single `rmdir()` syscall fails **EBUSY** after sixteen refusals, leaving
four unconsumed refusals. Server traces show one sequence (2), count 19 down
to 4, spaced by the existing 50–800 ms backoff: about **9.55 s**. There is
no lost reply or committed RMDIR to resolve. The original application error,
client and all four daemon logs remain in `negative-live.log`; the failed
fixture remains under the private run tree pending final evidence pruning.
The extracted production-loop assertion in `base-retry-probe.log` fails too:
it requires success on reply 21 within an inherited 30 s deadline.

### Fixed check during the same load

The repaired single RMDIR passed while both background pools were active:
twenty refusals, one unchanged operation identity, success on reply 21 in
**13.570 s**; `fixed-dir-loaded-home.log` and `.exit` retain it. An earlier
auxiliary launch omitted the home TMPDIR and the private seed refused startup
under `/tmp`'s headroom guard. `fixed-dir-loaded.log` retains that setup failure;
no application operation ran there. Correcting the fixture location, not a
product retry or changed headroom threshold, allowed the actual test to start.

The intentional double-load baseline also recorded a D4 plateau failure in
load1: one packer PUT error (`put_errors=1`, `read_errors=0`, 1,709 packed files),
`tmp/fail-29.log`. The down-member B6 test also failed its coverage assertion:
“2 files packed, no segment written degraded” (`tmp/fail-24.log`). Neither
outcome is a passing gate or evidence caused by the Task O change. The D4
harness removed its fixture at exit on this base, so its retained failure log
supports the observed counter but not a fresh raw-daemon re-analysis.

## Fix and boundaries

- `rpc_send_recv_shard_once` formerly ended every operation after sixteen
  attempts. `rpc_send_recv_dual_once` extended only identified MKDIR to 64.
  Both now use the caller's existing monotonic deadline for transient
  BUSY/STALE, routing and transport retries; callers without a deadline keep
  the finite sixteen-attempt cap. The original request identity and receipt
  resolution rules remain in force. FLOCK grant conflicts, APPEND barriers
  and retained-CREATE results still return to their owning caller directly.
- `ll_rmdir_run` now installs the same **30 s callback bound as MKDIR**, with
  any tighter inherited deadline taking priority, then restores the previous
  bound. This covers the parent read, local receipt-slot wait and RMDIR
  together. Previously RMDIR had no callback-wide deadline and hit the
  9.55 s retry cap (or a per-receive timeout); this intentional deadline
  change is explicit. The existing W155 lost-reply resolution allowance
  (default 8 s, same identity) is separate and may follow the normal attempt
  budget; this patch does not change it. MKDIR, REPORT and POSIX deadlines do
  not increase. No new behavior switch is introduced.
- Exhaustion logs name the RPC, actual attempts, deadline versus attempt cap,
  last inode status, elapsed time, deadline and operation identity. Unsupported
  encodings, real namespace errors and a genuinely exhausted deadline are
  not converted into success.

The injected count proves the conversion and bounded retry fix. It does not
prove that every W159 occurrence has this origin, or that unbounded healthy
load has bounded latency. The loaded case-timeout findings remain separate.

## Acceptance results and remaining work

Final test source `503e17f2d` matches all **2,239 tracked regular files** on
xorinox. Both normal and fault binaries report:
`efsd 503e17f2d (branch agent/devel-task-o, built 2026-10-28T01:23:01Z on xorinox)`.
The timestamp is UTC; this is the Oct 27 local campaign. Source manifest
`qualification-503e17f2d/SOURCE-MANIFEST.sha256` SHA-256:
`033b6b6226591e2801939d1ee9485cae02c7ce8f671baa1a69f5036ebb448af2`.
Environment: AMD Ryzen 9 7940HS, 16 logical CPUs, Linux
`6.12.0-211.67.1.el10_2.x86_64`; exact toolchain/FUSE/filesystem versions
are in `environment.txt`. No live existing cluster is used.

Run the committed isolated controller after the deliberate load pools finish:

```sh
bash scripts/qualify_dir_ops.sh ~/efs-taskO \
  ~/efs-artifacts/taskO-20261027/qualification-503e17f2d \
  503e17f2d ~/efs-taskO/DONE
```

The controller keeps every outcome, exits at a failed stage and seals its raw
manifest. Existing `EFS_K36_KEEP` / `EFS_D4_KEEP` test-retention settings preserve
fixtures for later log capture and pruning, without changing product behavior.
A preliminary pre-test build with a literal placeholder identity was stopped;
`qualification-2552/SETUP-ERROR.txt` records it. No test verdict is attributed
to that discarded build.

| Check | Result |
| --- | --- |
| Extracted production-loop negative control, unchanged client | FAIL on reply-21 success assertion; retained |
| Live negative control `f75257527`, unchanged client | RMDIR EBUSY at refusal 16; retained |
| Fixed deterministic RMDIR under double load (`b78955995`) | PASS, 20 refusals then reply 21, 13.570 s, one syscall/opid |
| Fixed isolated deterministic RMDIR (`503e17f2d`) | PASS, 21 s including formation/teardown |
| Fast suite | PASS, 59 s |
| K36 recovery alone | **5/5 PASS**, 55/53/58/44/44 s |
| W159 slow barriers alone | **5/5 PASS**, 52/51/52/50/53 s; all own-entry wait-timeout counters zero, no application error |
| Original double-load core (`243b92113`, unchanged product) | **FAIL**, 211/218, seven unchanged case-deadline failures |
| Quiet core on fixed source | **FAIL**, 217/218; `mtime_monotonic_many_writes` exceeds 15 s |
| Base core intended quiet, but overlapped by a newly started Task I gate | **FAIL**, 215/218; same mtime deadline, plus `names_crazy_dirs` and `concurrent_creates_same_dir` deadlines |
| Xorinox full eight-job code gate | INTERRUPTED after foreign Task I gate overlap; partial results are not acceptance |
| Intel2 deterministic RMDIR and fast suite | PASS, 19 s / 57 s |
| Intel2 K36 alone | **5/5 PASS**, 64/62/68/66/65 s |
| Intel2 W159 alone | **5/5 PASS**, 51/51/49/52/52 s |
| Intel2 complete POSIX core | **218/218 PASS**, unchanged deadlines, 33.9 s |
| Intel2 eight-job full gate | RUNNING, exact source `503e17f2d`, user-authorized alternate idle host |

Both original load pools ran to completion: load1
`TOTAL 87 FAIL 2 NOTRUN 2 STATUS FAIL (1268s) OVERBUDGET 29 BUILD 0s`;
load2 `TOTAL 87 FAIL 0 NOTRUN 2 STATUS INCOMPLETE (1261s) OVERBUDGET 28 BUILD 0s`.
Those were deliberately simultaneous pools, not quiet-host timing acceptance.
Their successful commands do not erase the core or load1 failures.

The unmodified loaded client had one successful BUSY retry and **no spontaneous
retry exhaustion** (`base-stress/ladder-audit.json`). The counted fault proves
the independent K50 conversion defect; it does not establish that the original
orcd-test W159 recurrence has the same cause. W159's broader latency findings
remain OPEN. The fixed quiet core finished before Task I started; the later base core
comparison overlapped Task I's new gate (start `2026-10-28T01:42:28Z`). These
are not a controlled latency A/B; all mtime failures remain failures. No POSIX
deadline was increased and no core PASS is claimed. The failed isolated
controller result and its manifest remain unchanged when the full gate runs
separately.

`test_dir_ops_busy.py` is wired in `make test-long`; its provisional 35 s / two
slot budget must be finalized from the full-pool measurement. Final full-gate
result, closed top-level manifest/hash and pruning receipt are owed. Real-host
TCP/RDMA and multi-host acceptance remain NOTRUN for the coordinator.

## Host handoff after concurrent activity

Task I started a separate candidate gate on xorinox after Task O's isolated
regressions and core completed. Its process ownership and start times are
recorded in `full-gate-host-overlap.json`. It overlapped the later base core
comparison and Task O's first full-gate attempt. The user authorized trying
alternative hosts; intel1 and intel2 were idle. Task O moved to **intel2**,
private `~/efs-taskO`, with a fresh copy of exact source `503e17f2d`.
Its raw root is independently `intel2:/home/maik/efs-artifacts/taskO-20261027/`.

Only Task O's xorinox runner was suspended to stop new admissions, and its
owned test process groups were interrupted and allowed to tear down their
private fixtures. The runner was released after those children exited.
`INTERRUPTION.json`, `private-unmount.json`, and the top-level
`private-stop-check.json` record this. Task I was not signalled or changed.
The partial xorinox gate is **INTERRUPTED**, never a passing or completed
failing gate, and its snapshots are labelled partial. The move requires a
fresh gate, not reclassifying shared-host timing as a clean result.

Intel2's normal and fault binaries both report
`efsd 503e17f2d (branch agent/devel-task-o, built 2026-10-28T01:51:47Z on intel2)`.
Its source manifest has the identical 2,239-file hash above. Intel2 has eight
logical GenuineIntel CPUs and Linux `6.12.0-211.67.1.el10_2.x86_64`.
The repeat uses the same committed controller and assertions; the host change
does not reclassify xorinox's core failures as passes. Full-gate acceptance and
raw closure on intel2 are still pending.

K50 is Task O's final checklist identifier. The initial branch used K49; Task L
independently allocated K49 for its TSan teardown finding, so Task O was renumbered
before handoff. No product or test behavior changed.
