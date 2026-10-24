# W155: per-request receipts and bounded same-identity resolution (Oct 24)

**Verdict: code fix PASS (scoped) on branch `claude/devel-w155-ab5120`,
commit `b36710b65`, not merged, not pushed. `make test-all` on nuc PASS
(serial, rc 0, no `make … Error` line); the parallel summary runner shows
438 commands, 1 FAIL that fails the same way on the base, 14 NOTRUN host
limits. The nuc b1/b2 live reruns are NOTRUN** (parent session).

- **Purpose:** W155 ([work queue](work-queue.md)), an alpha 1.0
  correctness item. On nuc (W148 rerun, attempts b1 and b2,
  [gates-nuc](gates-nuc-20261022.md#w148-modify-under-load-rerun-on-the-fixed-build))
  flush or close returned EIO after a receive timeout and a BUSY sequence,
  and later reads matched the attempted rewrite. The client could not tell
  whether a timed-out request had been applied, and the logs could not
  either: gap G2 of the [correlation page](w155-oracle-correlation-20261023.md)
  is the server's missing per-request receipt; G1–G8 are the logging gaps.
- **Tested source:** branch `claude/devel-w155-ab5120` on base
  `merge-k32-k33` = `2d03d9837` (devel plus K32 and K33/W157 increment 1).
  Code, tests and the protocol doc are commit `b36710b65`; this page is the
  docs commit after it.
- **Numbers taken:** no KV kind, no Raft tag and no wire message type.
  One REPORT flag bit, `EFS_REPORT_F_RECEIPT = 8` (the REPORT flag word is
  per message; the K14 `EFS_REPORT_F_PUBID` is 4). A host of an earlier
  build refuses the flag as INVAL (fail closed); mixed builds are refused
  at HELLO anyway.

## What changed

**Server: a receipt per REPORT** (`src/common/request_receipt.c`,
`include/efs/request_receipt.h`, the W155 block in `src/server/raft_host.c`,
`src/server/handler.c`).

- Every REPORT that carries a publication id runs under a receipt keyed by
  (session UUID, epoch, publication id) on the host that received it. The
  receipt records state (pending/done), the last rc, the attempts run
  there, the failing stage (`fail_phase`) and an outcome merged over the
  attempts: `none` (nothing proposed: refused, routed away, or failed in the
  ReadIndex or session admission, before any publication proposal), `unknown` (proposed or forwarded,
  verdict not observed), `partial` (STALE: all but the CAS losers
  committed), `applied`. Outcomes only rise.
- A duplicate of an `applied` request is answered OK without running. A
  duplicate of a running attempt waits for it (5 s, then BUSY) and takes an
  `applied` or `partial` result; any other end runs the same identity
  again. Re-running is safe because the REPORT path is idempotent by
  construction (a committed record skips, a W77 unit joins its
  deterministic transaction id); the receipt only stops two attempts of one
  request from running side by side and avoids re-running applied work.
- `EFS_REPORT_F_RECEIPT`: a REPORT with no records, the session and the
  publication id is a read-only receipt query; the reply
  `struct efs_msg_report_receipt_reply` gives state, outcome, rc, attempts
  and age.
- In memory and bounded (4096 slots, `EFS_REPORT_RECEIPTS`), not in Raft:
  an entry per REPORT would undo K32. Finished receipts are evicted before a
  running one. A missing receipt is never read as "not applied".
- Lines (closing G2): `report-receipt id=<uuid8>/<epoch>/<mint>:<seq> ino=
  nrec= how=ran|replay|joined|waiting rc= outcome= attempts= joined=
  replayed= phase= ms=` for every duplicate and every failed (not STALE, not
  the one-record route bounce), repeated or slow (≥ 1 s) attempt, all
  attempts with `EFS_RECEIPT_TRACE`; `report-receipt query id= state=
  outcome= rc= attempts= age_ms=` per query; `report-split` now ends with
  `ino= pub=<mint>:<seq>`.
- Identified directory ops (CREATE/UNLINK/LINK/RENAME_AT with the I16 op
  id) already have a durable receipt, the op-id window. A same-identity
  retry it answers now logs `opid-receipt: replay shard= id=<uuid8>/<epoch>/<seq>
  rc= ino=` (G2/G4 for metadata).

**Client: resolve before EIO** (`src/client/inode_rpc.c`,
`src/client/write.c`).

- A sync REPORT (fsync, close/flush) whose loop ends with an open outcome
  (NET, BUSY, NO_QUORUM, NOT_PRIMARY, NOT_FOUND, or the 8 s budget spent)
  after reaching at least one host is resolved by
  `efs_client_rpc_report_resolve` in a fresh window of `EFS_RESOLVE_MS`
  (default 8000 ms): ask every host it reached for the receipt; `applied`
  is success, `partial` is a definite STALE (the CAS losers are replayed by
  a later fsync, as before), `pending` waits and asks again, anything else
  (absent, none, unknown, host unreachable) re-sends the same publication
  id. A re-send that answers OK is success; a definite answer stands. Only
  an outcome still unknown at the end of the window returns EIO:
  `report-resolve id=… ino=… outcome=unknown … -> EIO, outcome unknown
  after the bound`. Other outcomes log `outcome=applied|partial|resent|
  answered` with `from=`, `queries=`, `resends=` and `ms=`.
- An identified directory op whose reply was lost after a send (a receive
  failure or timeout, or a reply that arrived after the deadline) and whose
  retry loop then ended BUSY or NET is re-sent under the same op id with a
  fresh bounded deadline (`rpc_resolve_identified`, both the shard and the
  dual-host paths): the op-id window answers a completed id with its
  recorded reply and runs a new one once. `inode-rpc: resolve type= id=
  outcome=answered|refused|unknown`. EBUSY/EIO only when still unknown at
  the bound. Unidentified requests (reads, GETATTR) are unchanged.
- Lines: `inode-rpc: retry … nid= id=` on both retry loops (G1/G4);
  `report-loop … pub=<mint>:<seq> sent=<mask>` (G3); `flush-meta
  stage=report|writeback ino= report_rc= rc= stalled=` (G5); the BUSY
  exhaustion line names the op id (G6).
- `scripts/w155_correlate.py` joins a failure's retries, report-loop,
  resolve lines and the servers' receipt lines by request identity (whole
  logs, not only the window) and reports G1–G6 as closed when a run carries
  them. Legacy logs are handled as before.

**What is not claimed.** No rollback and no old-or-new atomicity: a
resolved request is the server's recorded verdict, an unresolved one stays
unknown and is an EIO (the W155 oracle still records and quarantines it).
G7 (why a daemon stops logging) is not addressed. The REPORT receipt is
per host and in memory; a host restart or eviction degrades a lookup to a
same-identity re-send, never to a guess. A close can now take up to the 8 s
budget plus the 8 s resolution window (more only through receive timeouts
bounded by the same deadline).

The contract is in
[sessions.md § Ambiguous completion is resolved by identity](../how-it-works/protocols/sessions.md#ambiguous-completion-is-resolved-by-identity-w155).

## Tests (`make test`)

Deterministic: a fake clock, no sleeps on the client side, injected faults.
Production functions are extracted from the sources and compiled with a
fake transport.

- `tests/test_w155_report_resolve.py`: the client REPORT builder, the
  dual-host loop, the receipt query and the resolver, against three
  simulated nodes that each run the production server receipt block (one
  copy per node) over one shared committed state and per-node receipt
  tables. Each case also checks the data was applied exactly once.
  - A applied, reply lost, budget spent in a 30 s receive → receipt
    `applied`, OK.
  - B request lost before any server saw it → receipt absent, re-send
    applies, OK.
  - C applied, every reply in the budget BUSY (server verdict unknown) →
    receipt `unknown`, re-send skips the committed records, OK.
  - D applied on n1, reply lost, n1 then down → n2 answers the re-send
    (records skipped), OK.
  - E applied, reply lost, the whole cluster unreachable for the bound →
    the only EIO, after 8–9 s of fake time, logged `outcome=unknown`.
  - F one record lost its CAS, reply lost → receipt `partial`, STALE, not
    EIO.
  - G the first attempt still running when asked → `pending`, then
    `applied`, OK.
  - H reply lost inside the budget → the loop's own same-identity re-send
    is answered from n1's `applied` receipt, not run again.
  It also checks the log lines join on the identity on both sides, and
  that `report_dirty_ino_run` resolves after the budget and before the
  commit bookkeeping, resetting the reached-node set at every mint.
- `tests/test_w155_identified_resolve.py`: the shard and dual loops, their
  wrappers and `rpc_resolve_identified`, against a server whose receipt is
  the real op-id window (`src/sim/opid.c`). For CREATE on both paths: M1
  applied, reply lost, budget spent → replayed original reply (ino 501),
  applied once; M2 request lost → applied now, once; M3 applied, reply
  lost, cluster down → error after the 8 s bound, logged unknown; M4
  GETATTR with a lost reply → no resolution. RENAME_AT M1 on the shard
  path. M1/M2 end OK with the correct reply.
- `tests/test_w155_receipt_table.py`: outcome mapping and merge, replay,
  re-run after `unknown`, join, join timeout (BUSY, nothing run twice), a
  joined attempt that ends unknown (the joiner runs it), a joined `partial`
  (STALE), eviction of finished receipts while a running one keeps its
  slot, the identity format. Five runs in a row PASS.
- Negative control: with `W155_RESOLVE_MS=0` (resolution disabled) both
  path tests fail (abort in the reply-loss cases). All three also PASS under
  `EFS_SANITIZE=1` (ASan + UBSan).
- Existing tests adjusted for the wrapped loops (`test_metadata_retry_budget`,
  `test_create_retry_identity`, `test_report_frame_boundary`) keep their
  assertions; `test_w125_harness` gained a W155-format correlation case
  (identity joined to three server receipt lines, G1–G6 reported closed).
  K13/K14/K32 tests unchanged and passing (`test_k32_entry_count`,
  `test_pub_units`, `test_report_*`, `test_put_ticket`, …).

## Commands and results

nuc (user maik, 8 CPU, 30 GB, Rocky Linux 10.2, kernel
6.12.0-211.67.1.el10_2, gcc 14.3.1, Python 3.12.15; libtsan now installed),
no efsd or efs-fuse running on the host. No cluster: the W155 tests are
unit-level (fake transport); the other `test-all` commands use their own
loopback fixtures (TCP, `EFS_TRANSPORT=tcp`). Private tree from `git archive`,
built with the deployment identity variables:

```sh
git archive b36710b65 | ssh nuc 'mkdir -p ~/efs-w155/g-b36710b657c5 && tar -x -C ~/efs-w155/g-b36710b657c5'
ssh nuc 'cd ~/efs-w155/g-b36710b657c5 && export TMPDIR=$HOME/efs-w155/tmp EFS_TRANSPORT=tcp &&
  V="EFS_GIT_ID=b36710b657c5 EFS_GIT_BRANCH=claude/devel-w155-ab5120 EFS_GIT_DESCRIBE=b36710b657c5" &&
  make -j8 $V && make test-all $V'          # in screen (SCREENDIR=$HOME/.screen)
grep -E '^make(\[[0-9]+\])?: .*Error' <log>
# negative control
W155_RESOLVE_MS=0 python3 tests/test_w155_report_resolve.py      # expected FAIL
W155_RESOLVE_MS=0 python3 tests/test_w155_identified_resolve.py  # expected FAIL
```

Build identity: `efsd b36710b657c5 (branch claude/devel-w155-ab5120, built
2026-10-24T02:29:51Z on nuc)`; sha256 efsd
`6d9fef854d838a2e5d5bac5647551bf302ee903857606942ab3c320e390038df`,
efs-fuse `c937733f94230686fa39a8b2c6c08da961f1beb848f77c4b6a9e57d353158bca`,
efs-mgmt `77133f7525439834656a8c970e1d783dde0c5c3ea9c54e57cb6c15b06b15490b`,
libefs.a `0aef42f7590fe1c266ac3fb517bb9f7ce48c9973e62820a31730481602889582`.

- `make test` at `f9f0c85ba717` (the same tree as `b36710b65`, before the
  WIP commits were squashed; `git rev-parse HEAD^{tree}` equal): TEST_RC=0.
  The three W155 tests ran inside it and passed.
- **`make test-all` at `b36710b65`: TESTALL_RC=0**, 02:29:51Z–02:36:18Z
  (about 6.5 min including the build), no `^make(\[n\])?: .*Error` line.
  NOTRUN/SKIP lines are the host limits listed below.
- `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`
  on the same tree: `TOTAL 438 FAIL 1 NOTRUN 14 STATUS FAIL (168s)`.
  - **FAIL `tests/test_k32_entry_count.py` also fails on the base.** It
    passes serially (in `make test-all` above and alone). Run eight at a
    time, the summary runner's load, it fails on the base `2d03d9837` 16 of
    40 and 9 of 16 runs, on this branch 15 of 40 and 9 of 16, on the same
    two assertions in both trees: the K13 ticket-batching count
    (`admit` after batching `65+64`, expected `64+64`) and the derived
    `per_write == 3.0`. This change does not touch ticket admission
    (`src/client/put_ticket_owner.c`) or the K32 publication code the test
    extracts. A load-sensitive count in that test, reported to the parent
    as a follow-up, not fixed here.
  - NOTRUN (host limits): `test_cluster_stop.py` (no `~/git/cluster`),
    `test_rdma_memcheck.py` and `test_leak_gate.py` (no valgrind),
    `test_declaration_gate` RDMA path, `test_rdma_xprt` and the seven
    `test_rdma_segmented` modes (no IB device), `test_follower_bound`
    stalled-compactor case (needs `EFS_FAULTS=1`),
    `test_make_test_summary_runner.py` (its NOTRUN fixture). The ThreadSanitizer
    gates ran (libtsan is now on nuc) and passed.
- Negative controls (`W155_RESOLVE_MS=0`): both path tests FAIL, as
  expected; with the default they PASS.

## Raw outputs

Outside Git on nuc, `~/efs-artifacts/w155-20261024/` with `SHA256SUMS`:

| file | sha256 |
| --- | --- |
| `make-test-all-b36710b657c5.log` | `a0808a4186c1b6fe1ca2ecd180516f6bdf9d9a5f474137012fb7915441bcf4f1` |
| `build-b36710b657c5.log` | `2229e5faef38a084058fc8cbb60a53c18f3dd8cb13f7726774d7d62e897b3474` |
| `make-test-f9f0c85ba717-same-tree.log` | `efa71dd63bf986dca3d60604122a38ff2419ff5a8ac56b18e7009e79477e4a57` |
| `summary-runner-summary.txt` | `0ee44d72eebeec8c9f9f464bdf20383b2676178942a6264ab949475acb777247` |
| `summary-runner-fail-91-test_k32_entry_count.log` | `f0fc559bd2e76d7bdfa534de7f884f9a81f45403c824d1de560ed624140336f3` |
| `k32-flake-base-vs-w155.txt`, `k32flake.sh` | `8b194e9fef29b537be77c2e4641101dc8482b39f6d4418128c6192a57d66d0d7`, `ff4584ff34abb39417d490aff0a1493d4b3d5d874f8e7b8123d64a7eb81b593b` |
| `negative-W155_RESOLVE_MS0-report.log` | `3016a909c2740cf0f889c328e489627e4a48f9751938374826c6b4f592d59b26` |
| `negative-W155_RESOLVE_MS0-identified.log` | `cb5be5f591ec9a5620c2e09a007b31893c76282023edec5fb39df201ac5b98c9` |
| `test_w155_*.stdout`, `w155-tests-rc.txt` | in `SHA256SUMS` |
| `binaries.sha256`, `efsd-version.txt` | in `SHA256SUMS` |

Also there: the summary runner's console and timings, and the base build
log. `k32flake.sh TREE ROUNDS` runs the K32 test eight at a time from a
built tree; its counts are in `k32-flake-base-vs-w155.txt`.

## NOTRUN (parent session)

- **nuc b1/b2 live reruns**: the W148 modify-under-load campaign
  (`gates_nuc_w148_campaign.sh` or `w125_modify_repro.sh` × 9, TCP, direct
  I/O) with the W155 oracle on this build, expecting no EIO from an applied
  rewrite, and `w155_correlate.py` tables whose failures (if any) join
  their receipts by identity.
- Any live run with RDMA, orcd-mini, a host restart during resolution, and
  the eviction path under a real REPORT rate.

## Limitations

- The REPORT receipt lives on one host and in memory. It is a fast answer
  and a join point, not a durable record; durability of the verdict comes
  from re-sending the same identity.
- The resolution window adds up to 8 s to a failing close or fsync; a
  request still unknown after it is an EIO, as before but now named.
- Async REPORTs are not resolved (their records stay dirty and the next
  sync REPORT carries them).
- The pending-receipt eviction path (every probe slot running) is covered
  by code review, not by a test.

## Cleanup

Gate trees on nuc: `~/efs-w155/g-b36710b657c5`, `~/efs-w155/g-f9f0c85ba717`,
`~/efs-w155/src` (an iteration tree, with a stray empty `.git` from a
mistaken `git init`; the gate trees were built with explicit
`EFS_GIT_ID`, so it touched no build identity) and
`~/efs-w155/base-2d03d9837`. **Not deleted:** nuc has no `edelete`
(`~/bin/edelete` and `command -v edelete` both absent), and trees are not
removed with `rm -rf`; listed for the parent. The screen sessions have
ended. No daemon, mount or port was started outside the tests' own
loopback fixtures; no cluster port (17432–17435, 27432/27433) was touched.
