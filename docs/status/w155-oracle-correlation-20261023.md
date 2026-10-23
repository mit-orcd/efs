# W155: operation-aware modify oracle and correlation tooling (Oct 23)

**Verdict: harness and tooling PASS by their self-tests on nuc; `make test`
324 of 327 recipe commands PASS, 3 FAIL on host prerequisites (no libtsan; a foreign-uid client); the W148 campaign rerun with the new oracle is NOTRUN** (a
parent-session gate on a dedicated host with retained evidence). No product
code changed.

**Purpose.** W155 ([work queue](work-queue.md)): in the nuc W148 rerun
([gates-nuc](gates-nuc-20261022.md#w148-modify-under-load-rerun-on-the-fixed-build))
a rewrite returned EIO and the file later read back as exactly the attempted
version, and the harness counted that as a mismatch. EIO promises no
rollback, and visibility does not prove durability. The review
([devel-integration-review](devel-integration-review-20261022.md#review-of-the-oct-23-gate-merges-and-recap--october-23))
set the harness rule: no blanket old-or-new after an error, record the
failing stage, bytes submitted and attempted content, quarantine an
unresolved file until it is classified, require the final known successful
version after quiescence, and never turn an error or an unresolved outcome
into a clean PASS. This page covers that oracle and a tool that turns a
run's logs into one table per failure, so the product investigation starts
from evidence.

**Source.** Branch `devel-w155h` (worktree branch
`claude/devel-w155h-655101`), base devel `bf16436e8`; scripts and tests in
`383abd654`, this page in the following commit. Not merged, not pushed.

## What the oracle records and decides

`scripts/w125_concurrent.py` (the W125/W148 workload):

- **Stages.** A modify runs as recorded syscalls: `open`, `fstat`, `truncate`
  (rewrite, shrink, extend), `seek`, `write` in 1 MiB calls (`WRITE_CHUNK`),
  `close`. The client's FUSE FLUSH (`flush-meta`) runs inside close(2), so a
  flush failure is the `close` stage. The workload issues no fsync, as before.
  Creates (`open`, `fstat`, `write`, `close`, `rename`) and unlinks are staged
  the same way.
- **Per failed attempt** (one JSON line in `--modify-log`, which
  `w125_modify_repro.sh` sets to `OUT/attempts.jsonl`): name, inode, base and
  attempted version, operation, every stage with start/end time, errno and
  bytes (offset, requested, returned), the failing stage and its start time,
  bytes submitted of the total, the attempted content's length and sha256,
  the base's length and sha256, and the candidate states. The ERR line in
  `a.txt` now carries the stage, bytes, inode and the UTC time.
- **Candidate operation identity.** The client log exposes none per syscall
  (gap G1-G3 below), so the attempt records the inode and the stage window.
  `w155_correlate.py` attaches the request lines in that window.
- **Unresolved and quarantined.** On any error the file is unresolved: no
  modifier or deleter touches it again, the known version does not advance,
  and the failure counts as an error.
- **Candidates, not old-or-new.** The candidate states are the contents at the
  syscall boundaries the attempt actually reached: the base (last known
  successful version), the state after each completed call, and the full
  effect of the failing call. A write that failed in its second call yields
  base, truncated, first-MiB and second-MiB states, never the full attempted
  version. A close that failed yields every write boundary, including the
  attempted version.
- **Reads while unresolved.** A local read that matches a candidate is an
  observation (label, count, first/last time). Any other bytes, mixed or
  foreign, are a mismatch, saved with the candidate list in `meta.json`.
- **After quiescence** (final verification, writers stopped): every resolved
  file must be exactly its last known successful version, as before. Every
  unresolved file is classified as `base`, `attempted`,
  `intermediate:<boundary>`, `foreign` (a mismatch, saved), `missing` or
  `unread`, and the classification is logged.
- **Verdict.** Exit 0 PASS only with no error, mismatch or unresolved file;
  1 FAIL on any mismatch or error, so every failed modify is a FAIL whatever
  the file holds; 5 INCOMPLETE when nothing failed but an outcome is
  unresolved or unclassified. The final lines print the counts, one
  `UNRESOLVED` line per file, and `verdict=PASS|FAIL|INCOMPLETE`.
  `w125_modify_repro.sh` writes `INCOMPLETE` into `done` for status 5, keeps
  the store, and copies `efsd-nI.log` into OUT after the stop.

The remote mode (`--remote`, no modifiers) is unchanged: it accepts any model
version and cannot know another client's unresolved attempts.

## Correlation tool

`scripts/w155_correlate.py OUT [--efsd-log F ...] [--pad S] [--json FILE]`
prints one table per failed attempt. The window is the failing stage
± 5 s. A receive retry belongs to the window when its request was sent inside
it (log time minus `recv_ms`) or it was logged inside it.

The table rows are:

- the attempt and its stages, bytes submitted, attempted digest, candidates
  and observed reads;
- the client retries, with the type named from `include/efs/protocol.h`,
  attempt, why, rc, req_len, recv_ms and the derived send time;
- `report-loop` and `efs-fuse` error lines, marked when they name another
  inode;
- `rename_at`, `opid-admission: BUSY` and `slow-ok` per type;
- per daemon: report-split total and failed, `txn-recover`, `report-resolve`,
  raft votes, elections and rejected AppendEntries, `kv-flush`, the
  apply-sleep count and the longest silence;
- the receipt/admission outcome (any server line naming the inode);
- the final observed version.

A legacy OUT (before this change) has no `attempts.jsonl`. The tool then uses
`a.txt` ERR lines and the inode and captures in `mismatch/N/meta.json`, and
anchors on client lines that name the inode.

Applied to the retained nuc W148 captures (raw, outside Git; see below), it
shows:

- **b1, `w1/d016/f000216` v1→v2 rewrite, EIO.** In the same millisecond
  (04:03:59.781Z) the client logged a `REPORT_CHUNKS` (type 67) receive retry
  with `recv_ms=30012` (sent at about 04:03:29.769Z), `report-loop
  ino=22740 busy=1 ms=3099 rc=-13` and `flush-meta ... ino=22740`.
  `opid-admission: BUSY oldest=38401 next=38417 live=1` came at 04:03:57–58.
  Raft group 0 changed leader (n2 `became leader term=2`) in the window.
  n1 had one `report-split rc=-14 fail=finish/-14`. No server line names
  the inode. Both saved reads are exactly v2. The retry's 30 s receive wait
  and the report-loop's 3.1 s budget do not fit one request, and the logs
  cannot say which request the retry belonged to.
- **b2, `w2/d041/f000741` v0→v1 rewrite, EIO.** The file's `flush-meta`
  (ino 61723, 04:19:31.647Z) has **no `report-loop` line of its own**. The
  only retry and report-loop in the window (04:19:31.951Z, `recv_ms=30007`,
  `ms=8001`) name inode 68173, one of two other `flush-meta` failures within
  300 ms (the two create EIOs are the likely owners, but a.txt gives them no
  inode). n1 logged 8 failed report-splits (`finish/-14`, `act/-13`,
  `act/-15`), n2 logged 5 (`wait/-13`, `act/-13`), and raft prevotes for
  term 3 ran across all three daemons. The final read is exactly v1.
- **b7, a create EIO.** a.txt gives it no time and no inode, so it cannot be
  anchored (gap G8). Its fuse.log shows the same retry, report-loop and
  flush-meta triple, for ino 30562.

## Logging gaps (first implementation gap for the product side)

The W155 row says request/receipt correlation is missing. Concretely:

| gap | what the logs lack | effect |
| --- | --- | --- |
| G1 | `inode-rpc: retry` names type, attempt, rc, req_len and recv_ms, but no inode, file handle, opid, request id or target node | a retry is attributed to a failure by time only |
| G2 | the server logs no per-request receipt, admission or apply outcome; `report-split` has no inode, client session, opid or request id | whether a timed-out REPORT_CHUNKS was applied cannot be read from the logs. This is the W155 question |
| G3 | `opid-admission: BUSY` names opids but no inode or type; `report-loop` names the inode but no opid | the admission window cannot be joined to the failing flush |
| G4 | a receive retry does not log whether the resend reused the same identity (opid), or what the resend's reply was; `slow-ok` names only the type | same-identity outcome resolution cannot be checked from logs |
| G5 | `efs_fuse_flush_ino` logs the final rc only: not whether `efs_append_flush_report` or `efs_file_sync_error` (stalled write-back) produced it; every non-quota code maps to EIO | b2's flush-meta without a report-loop cannot be explained from the log |
| G6 | `rename_at`/unlink EBUSY name the parent inode and the name, but no opid; an unlink EBUSY logs nothing beyond a type 87 retry | namespace EBUSY failures are anchored by name and time only |
| G7 | a daemon's silence (b2 n3 04:18:55–04:19:18) has no logged reason | stalls are measurable, not explainable |
| G8 | harness, fixed here for new runs: pre-W155 a.txt ERR lines had no time, stage, bytes or inode | 4 of 7 b1 failures, 6 of 7 b2 failures and the one b7 failure cannot be anchored at all |

A minimal product-side step: one client line per failed or retried
data/metadata RPC carrying `ino`, `opid` (or request id), `type`, target node
and attempt, and one server line per REPORT_CHUNKS outcome carrying the same
`opid`/session and the apply verdict. Then a failure's request, its retries
and the server's receipt join exactly, and G1–G4 close.

## Commands and results

On the Mac, then on nuc (user maik, 8 CPU, 30 GB, Rocky Linux 10.2, kernel
6.12.0-211.64.1.el10_2, Python 3.12.15), from `git archive` of `383abd654`
into `~/efs-w155h/src`, `TMPDIR=$HOME/efs-w155h/tmp`, `EFS_TRANSPORT=tcp`:

```
python3 tests/test_w125_harness.py
make -j8
make test            # stopped at tests/test_apply_ring.py, see below
make -k test         # every remaining target
grep -E '^make(\[[0-9]+\])?: .*Error' LOG
python3 scripts/w155_correlate.py OUT --efsd-log RUN/logs/efsd-n1.log ...
```

- `tests/test_w125_harness.py`: **PASS** on the Mac (Python 3.9.6) and on nuc
  (`SELF_RC=0`), in about 13 s. The existing stop/wrapper/sidecar/remote/
  capture/inspect checks pass unchanged. The new checks:
  - `w155 stages`: seven failure cases (open; truncate not applied and
    applied; the second write; close not applied and applied; a shrink
    truncate), each with the failing stage, errno, inode, bytes submitted,
    attempted and base digests, the exact candidate labels, no version
    advance, the ERR line with stage and time, the final classification
    (`base`, `intermediate:…`, `attempted`), verdict FAIL, and the two JSON
    records. Also a create that fails at rename.
  - `w155 quarantine`: no second modify and no unlink of the unresolved
    file; candidate reads are observations through the reader thread; a mix
    of base and attempted bytes is a mismatch saved with its candidates and
    is `foreign` at final verification; INCOMPLETE (5) without errors, FAIL
    with them, PASS only when clean.
  - `w155 main`: `main()` end to end with an applied-then-EIO close gives
    `verdict=FAIL`, an `UNRESOLVED` line and `classified attempted=1`.
  - `w155 wrapper`: status 5 makes `done` say INCOMPLETE, keeps the store,
    copies the daemon logs and passes `--modify-log`.
  - `w155 correlate`: verbatim b1 lines (times rebased) put the type 67
    retry, report-loop, flush-meta, admission BUSY, failed report-split,
    txn-recover, kv-flush, the silence, the missing receipt and the
    `attempted` classification into the failure's table. Out-of-window
    lines are excluded. A legacy OUT is anchored by inode and its capture
    is reported as exactly v1. G1–G8 are listed.
- `make -j8`: `MAKE_RC=0`, no `make: *** Error` line.
- `make test`: **FAIL on host prerequisites only, none in the W125/W155
  files.** It stopped at command 52 of 327, `tests/test_apply_ring.py`
  (`TEST_RC=2`): nuc has no `libtsan` package (`/usr/lib64/libtsan.so.2.0.0`
  missing), and the TSan gates fail rather than skip (W138). `make -k test`
  does not help, because `test` is a single recipe. So every one of the 327
  recipe commands was run in order, continuing past failures. That took
  about 4 min. The runner, from the source tree:
  `awk 'NR>363{if ($0 !~ /^\t/) exit; sub(/^\t/,""); print}' Makefile | sed 's/\$(MAKE)/make/g'`
  writes the `test:` recipe (this commit's Makefile: line 363 is `test: all`).
  Each line then runs through `bash -c` with its rc recorded. 324 passed and
  3 failed:
  - `tests/test_apply_ring.py` and `tests/test_session_latch_tsan.py`: the
    TSan build does not link (no libtsan). The native parts of both passed.
  - `make test-stop-control` (`Makefile:294`), in `tests/test_client_processes.py`:
    "client discovery requires root". The user's `efs-fuse` on nuc runs as
    another uid, which is the host condition the
    [gates-nuc page](gates-nuc-20261022.md#harness-and-script-changes)
    documents. With `EFS_CLIENT_SKIP_FOREIGN=1` the whole target passes
    (rc 0).

  Exactly one `^make(\[[0-9]+\])?: .*Error` line appears, for
  `test-stop-control`. The first `make test` log has one more, for the
  `test_apply_ring` stop. NOTRUN lines are host-caused: RDMA (no IB
  device), valgrind memcheck headers, and the cluster scripts
  (`~/git/cluster` absent). `tests/test_w125_harness.py` passed inside the
  recipe as well.
- Correlation on the retained b1/b2/b7 captures: the tables summarized above.

## Raw captures

The nuc W148 bundles the [gates-nuc page](gates-nuc-20261022.md) records as
lost still exist, outside Git, in the session-family notes folder
`~/Library/Application Support/Claude/side-session-notes/local_19ae04bd-0721-46f2-9b3d-34642bbc893d/gates-nuc-w148-captures/`
on the Mac. That folder is not a stable store, so retain it before the
family is archived. sha256:

| bundle | sha256 |
| --- | --- |
| b1.tgz | `e14e991cd9c528386828a9cfe1410a8e1bed14c5d6586e05570c20c6b51628eb` |
| b2.tgz | `1d5e461fa3ccb6f840602090b50f340e114fb0c30851662168cc2f041ae11db3` |
| b4.tgz | `6cffdd2d4f597c7675615b2d411e8efa47892165eadf64229fe833f22db756b5` |
| b5.tgz | `7e18ee918eb62429af624ba2b1c4912b576c412660625739080272dbebc11130` |
| b7.tgz | `86330fd2e0fb268f0a94a32d1c80b8689c565a5d45c2ac3dc9ac001a097aa924` |
| b9.tgz | `d0079e28cff402f0f160d40971121515be9908a62305b93774b02a4cff0e03b9` |
| gate-logs.tgz | `f87b045133cea55765b396dc52cf38833ece35d4ce0a6080f53ad0b351ba085f` |

b1 and b2 hold OUT plus the three daemon logs. b4, b5, b7 and b9 hold OUT only.

## Limitations and remaining gates

- **NOTRUN: the W148 campaign with this oracle** (`gates_nuc_w148_campaign.sh`
  or `w125_modify_repro.sh` × 9 on a dedicated host). Until it runs, the
  oracle is proven against injected faults only, not against the product's
  real EIO.
- Close(2) covers FUSE FLUSH and RELEASE, so the harness cannot separate them.
  The client log's `flush-meta` line is the only witness.
- Writing in 1 MiB calls replaces Python's single buffered `write`. FUSE
  splits large writes into requests either way, but the syscall pattern is
  not byte-identical to the runs that reproduced W148.
- Candidates are boundary states of completed or failing calls. A server that
  applied part of one write(2) would show up as `foreign` (a mismatch). That
  is intended: the oracle does not guess sub-call atomicity.
- A file deleted by another client's deleter (`--remote`) is classified
  `missing`, and the remote side has no unresolved model.
- Attribution in `w155_correlate.py` is by time and inode (G1–G3). Co-timed
  lines are listed, not proven related.
- The failing `make test` target is listed above; it is a host package gap,
  not a regression from this change.

## Cleanup

`~/efs-w155h` on nuc was deleted after the runs, including the screen
socket directory. No daemon, mount or port was started; the user's cluster
(17432–17435) and `~/efs-d39model` were not touched.
