# K36 / F23: verified recovery after an unclean stop (Oct 24)

**Verdict: PASS (scoped, loopback).** Implementation and crash-recovery tests
for [K36](alpha-1.0-2plus1-checklist.md) under decision
[D42](decisions.md) (row F23), finding
[F23](s-docs-alpha-1.0-20261024.md). Code gate on intel1 at `e8d25cc22`:
`make test` PASS, `make test-long` PASS with host-limit NOTRUNs; the one
first-run FAIL (`test_k33_payload.py`, K34/F24) also fails on the base
([Gate](#gate)). Full power-loss qualification, multi-host and RDMA runs:
**NOTRUN**. Not merged.

## What changed

A storage node whose previous stop was unclean (crash, `SIGKILL`, forced
exit rc 3, power loss) keeps every root "recovery uncertain" and COMMIT
returns `EIO`, as before. New: a recovery thread (`src/server/recovery.c`)
proves the store and then clears that fence. The phases are specified in
[data plane: verified recovery](../how-it-works/protocols/data.md#verified-recovery-k36):

| Phase | Proof |
| --- | --- |
| identity | every root's durability record names this boot (previous + 1, running, this verifier); the member store advanced the incarnation |
| journal | the reclaim journal was verified and replayed at startup, or rebuilt by its own recovery thread |
| ledgers | every storage session certificate (K13 PUT floor) reads back intact; interrupted seals removed |
| fragments | every chunk and segment fragment file read whole: I25 record names the object its file name names, BLAKE3(payload) equals the record digest; torn, misnamed or corrupt files dropped |
| raft | each hosted metadata group answers a ReadIndex from this node (local replica applied to the leader's commit); a replica that missed a committed row is resynced from the leader's snapshot first |
| tickets | ADMITTED tickets placing a fragment here whose fragment is not proven are remembered |
| metadata | every published object (chunk base and live span) placing a fragment here is checked against the published digest; a missing or mismatching one is dropped and its stripe gets a W154 degraded mark through Raft; referenced packed-segment fragments are checked too (counted, no segment rebuild) |
| fence | every root's fence clears; COMMIT works again |

After the fence the thread settles the remembered tickets (a published one
is marked) and counts what W154 rebuilt here. A transient obstacle is
retried and shown as the reason; a failed proof leaves the store fenced
with `recovery FAILED` and the reason until a restart verifies it. Every
step is idempotent. Progress and counts: the log (`recovery: ...`) and
`efs-mgmt sync-status` (`recovery running|verified|FAILED phase=... files
checked= dropped= ... refs checked= missing= marked= already_marked=
superseded= unrecoverable= rebuilt= rebuild_waiting= ...`).

The report-only restart scan of recently modified files is removed.

**Two outcomes stay separate.** A recovered store serves and COMMITs again.
An earlier ambiguous write is not thereby proven: the PUT verifier changed
with the unclean boot, so a fragment the crashed run acknowledged and did not
COMMIT stays lost in its client's per-stripe `fsync` (K35, `data_commit.c`):
the stripe is proven on its other two fragments with a degraded mark, or is
`EIO` when it needs that fragment, and never counts it durable. Test step 4
checks both.

**New numbers** (taken from the high end of each free range; unique per
`tests/test_kv_kind_unique.py`):

| Kind | Number | Name |
| --- | --- | --- |
| Raft log command | 248 | `EFS_MD_CMD_RECOVERY_MARK` (`[tag][mark]`, 139 B; apply `efs_meta_apply_recovery_mark`; 249 is K35's `EFS_MD_CMD_DEGRADED_MARK`) |
| Wire message | 243 / 244 | `EFS_MSG_RECOVERY_SCAN` / `_REPLY` (inventory page from a member hosting a group this node does not host) |
| KV kind | none | marks reuse W154's `EFS_KV_KIND_DEGRADED` (51) |

`struct efs_msg_sync_stats_reply` gained `struct efs_recovery_stats` and
`efs_sync_root_stats.uncertain` (no compatibility: new daemons and
`efs-mgmt` together, per AGENTS.md).

**Fault hooks** (`EFS_FAULTS` builds only, inventory rows added to the
[A6 inventory](a6-settings-inventory-20261024.md)):
`EFS_FAULT_CRASH=<point>[:<n>]` exits 137 at `put-acked` (after a PUT
reply), `publish-apply` (a publication entry committed, not yet applied
here), `reclaim-accepted` (a reclaim batch journaled, not answered),
`recovery-local` and `recovery-marked`; `EFS_FAULT_RECOVERY_HOLD_FILE` holds
recovery before the fence clears. `tests/faults/efsd` now also links
`kv_compact.c` and `kv_lsm.c` built with `EFS_FAULTS`, for
`EFS_FAULT_COMPACT_DIE` and the small-memtable hooks.

## Tests

`tests/test_k36_recovery.py` (in `make test-long`): three, later four,
`tests/faults/efsd` daemons on loopback ports the test picks, efs-fuse,
stores under `TMPDIR`, buffered fragment I/O, TCP. Unsynced-loss is emulated
by truncating and deleting fragment files of the stopped store (a process
kill keeps the page cache).

| Step | Contract item | Check |
| --- | --- | --- |
| 1 | — | three nodes form; four files written and fsynced |
| 2 | crash during a PUT; torn and missing fragment | node 3 exits after its 5th PUT acknowledgement while a file is written without fsync; one of its fragments of the fsynced files truncated, another deleted; the fsynced files read exact from the two peers |
| 3 | crash during recovery | restart exits right after recovery's first degraded mark; the next start recovers again (idempotent; the torn file was dropped by the interrupted run) |
| 4 | fence holds until proven, then clears; COMMIT after recovery; the earlier ambiguous write stays unproven | held before the fence: `sync-status` `recovery running phase=fence`, `data-commit` to node 3 fails, a fresh mount's fsync proves its stripe on the other two only (K35 `2 of 3 durable; marked degraded`); released: `recovery: verified`, `data-commit` OK, a fresh fsync proves all three fragments (no new mark); mount A's fsync of the file written while node 3 died never counts node 3's crashed-run fragments durable; every mark retired, the torn and deleted fragments rebuilt |
| 5 | no acknowledged byte lost while two peers hold it | node 1 down, a fresh mount on node 2 reads every file exact (needs node 3's rebuilt fragments) |
| 6 | crash during publication | node 2 exits applying its first publication entry; verified on restart, `data-commit` OK, files exact |
| 7 | crash during reclaim | node 3 exits after journaling a reclaim batch (packed small files, deletions); journal replayed, verified |
| 8 | crash during compaction | node 1 exits at its first KV compaction output (64 KiB memtable); verified on restart |
| 9 | verification fails | a damaged storage session certificate: `recovery FAILED in phase ledgers` with the reason, `data-commit` fails; without it the next start verifies |
| 10 | — | at four members node 1 hosts group 0 only; its recovery reads group 2 from a member (`groups proven=1/2`) |
| 11 | — | a referenced segment fragment deleted: counted `segments missing`, verified, files exact |

Between outages the test waits until `efs-mgmt status` reports `Degraded
chunks: 0`, so each new outage meets whole stripes.

Unit tests (`make test`): `test_store_durability` (the fence persists across
orderly restarts until `server_put_sync_recovered`; the boot-counter proof;
local recovery keeps proven fragments and drops torn, misnamed, corrupt and
digest-mismatching ones, credits the quota, and a second pass changes
nothing); `test_meta_apply` (`efs_meta_chunk_refs_scan` over base and spans,
resume and stop; `efs_meta_apply_degraded_mark` writes once, is idempotent,
refuses a second fragment, a dead object, a disagreeing row and a
two-fragment mask, and W154 retires it). Updated for the new contract:
`test_shutdown_stuck_worker.py` (a single unformed daemon cannot verify, so
its stop after a forced one stays rc 4), `test_shutdown_bounded.py` (new
`recovery` teardown phase), the fault harness integration case
(`restart_recovered`: fsync OK once recovery verified).

## Gate

**Host:** intel1 (Intel Core Ultra 7 256V, 8 CPUs, 15 GiB, Rocky Linux 10.2,
kernel 6.12.0-211.67.1.el10_2, GCC 14.3.1, Python 3.12.15), private tree
`~/build/k36-gate-e8d25cc22` from `git archive e8d25cc22`, `TMPDIR` on
`/home` (XFS), TCP loopback only, in `screen`. **Source:** code commit
`e8d25cc22` (branch `claude/devel-k36-recovery-70f9de`, base `55ad94072`);
build identity `EFS_GIT_ID=e8d25cc22`. This page is the docs-only commit
after it.

| Run | Command | Result |
| --- | --- | --- |
| A | `make -j8 EFS_GIT_ID=e8d25cc22 all`, then `make -k EFS_GIT_ID=e8d25cc22 test-all` | build OK; `test` part: 419 commands PASS. `test-long` stopped at its 8th command, `tests/test_k33_payload.py`, `OSError: [Errno 5]` in `verify(a, 'node k down')` (line 190): K34/F24, see the base row. A make recipe stops at its first failing line, so the remaining `test-long` commands did not run in A |
| B | `make -i EFS_GIT_ID=e8d25cc22 test-long` (same tree, 29 min) | **exit 0, all 51 commands PASS**, none ignored: `test_k33_payload.py` PASS (685 s), `test_k36_recovery.py` PASS (111 s), `test_shutdown_stuck_worker.py`, `test_shutdown_blocked_io.py` PASS |
| base | `python3 tests/test_k33_payload.py` ×3 on `git archive 55ad94072`, same host | 1 of 3 runs FAIL at the same line 190 (`verify(a, 'node k down')`, `EIO`), 2 PASS (454 s, 284 s): the FAIL is the base's (K34/F24; 4 of 6 on nuc at `a2326a369`) |
| repeat | `python3 tests/test_k36_recovery.py` ×8 consecutively, same code and test source (`dev-repeat5-*.log`) | 8 of 8 PASS (54–119 s) |

NOTRUN host limits (A and B): no RDMA device (`test_rdma_xprt`,
`test_rdma_segmented` ×7, the RDMA frame path of `test_declaration_gate`),
no `valgrind/memcheck.h` (RDMA Memcheck and leak gates), `~/git/cluster`
absent, `test_stalled_compactor` needs `EFS_FAULTS=1`.

Docs gate: `cd docs && python3 gen-architecture-full.py` and
`python3 docs/check-architecture.py`: all 5 checks pass.

Raw logs (build, gate, test-long, base and repeat runs; 1.6 MiB of text) on intel1:
`~/efs-artifacts/k36-20261024/`, `SHA256SUMS` `dfc06e6c…7e6b`. Earlier
development runs there (`dev-*.log`) show the test's own fixes (waiting for
`Degraded chunks: 0` between outages, the reclaim trigger, K34 files); the
first gate attempt on `48c091fd5` is under `gate1-48c091fd5/` (its
stuck-worker assertion read a truncated log tail; fixed in `e8d25cc22`).

## Merged tree (merge-wave5)

K36 merged onto `merge-wave5` (devel + READ_VERIFY + K34 + A7 + K35):
merge `9fa0079c7`, fixes `87034b518`, merge of `1898c4c63` (the
`test_get_integrity.py` stub for K34) at `c19bf9fdb`. Changes against the
branch version above:

- K35 took Raft tag 249 for its fsync `EFS_MD_CMD_DEGRADED_MARK`; K36's
  command is now `EFS_MD_CMD_RECOVERY_MARK`, tag **248**
  (`EFS_MD_RECOVERY_MARK_LEN`, `efs_meta_apply_recovery_mark`,
  `apply_recovery_mark_cmd`). The two applies stay separate: K35 builds the
  mark from the chunk row and a node, K36 checks a full mark against it.
- Recovery reads fragment files through A7's O_DIRECT helpers
  (`server_frag_open`, `server_frag_pread`); the unit fixtures use the
  payload || 4 KiB tail layout.
- The `reclaim-accepted` crash point moved into `server_reclaim_accept` (a GC
  leader accepts its own batch without the handler); the hook is weak so
  storage unit tests link without `recovery.c`.
- Step 4 follows K35's per-stripe fsync: while node 3 is fenced a fresh
  fsync proves its stripe on the other two (`2 of 3 durable; marked
  degraded`); after recovery on all three; the fragments node 3's crashed
  run acknowledged never count durable in mount A's fsync (state `K`, never
  `D`, in its log).

**Gate on intel1** (same host as above), tree `git archive 87034b518`,
`EFS_GIT_ID=87034b518`, `TMPDIR` under `$HOME`, in `screen`:

| Run | Command | Result |
| --- | --- | --- |
| A | `make -k EFS_GIT_ID=87034b518 test-all` (19:13–19:41Z) | build OK; **`test-long` all 53 commands PASS** (`test_k33_payload.py` 349 s, `test_k36_recovery.py` 100 s); `test` stopped at `tests/test_get_integrity.py` (its stub source did not declare K34's `read_reprobe_down`; `merge-wave5` fixed that in `1898c4c63` after this merge) |
| B | `make -i EFS_GIT_ID=87034b518 test` | 419 commands: every one PASS except that same `test_get_integrity.py` (the only ignored error) |
| C | `c19bf9fdb` (= `87034b518` + the two-line test fix): `make -j8 all`, `python3 tests/test_get_integrity.py` | PASS |
| repeat | `python3 tests/test_k36_recovery.py` ×6 on the merged code | 6 of 6 PASS (64–96 s) |

NOTRUN host limits as above (RDMA ×9, valgrind ×2, `~/git/cluster`,
`test_stalled_compactor`). Raw logs: intel1
`~/efs-artifacts/k36-merge-20261024/`, `SHA256SUMS` `2a1adf2b…d6dc`.

## Limitations and remaining gates

- **NOTRUN:** full power-loss qualification (D42: a separate scope
  decision); multi-host, cross-host and RDMA runs (parent session); the
  fault harness integration run (`run_faults.sh --integrate`, needs the
  parent's FUSE/RDMA setup); a direct-I/O variant of the daemon test.
- Recovery reads every local fragment file: its time grows with the stored
  bytes (loopback stores here verify in tens of milliseconds); not measured
  on a large store.
- The raft phase's resync branch (a replica whose apply missed a committed
  row, `kv_incomplete`) is implemented but no test drives a replica into
  that state; every test restart proved its groups by ReadIndex directly.
- A single node that never formed a cluster cannot prove its groups and
  stays fenced; a group without quorum delays the fence the same way.
- A referenced packed-segment fragment that is not proven is dropped and
  counted, not rebuilt (W154 covers chunk stripes only); its two peers
  still serve it.
- ADMITTED tickets a live client never settles keep the post-fence tail
  waiting (shown as `tickets ... waiting=`); no promise depends on it.
- A stop during recovery waits for an in-flight metadata RPC inside the
  shutdown deadline.
- In step 2 a chunk written while node 3 died can read `EIO` with node 3
  down (acknowledged degraded with node 3's fragment counted): that is
  K34/F24, open in its own session; the test reads that file after recovery.

## Reproduce

```bash
git archive <commit> | ssh intel1 'mkdir -p ~/build/k36 && cd ~/build/k36 && tar xf -'
ssh intel1 'cd ~/build/k36 && export TMPDIR=$HOME/tmp-k36 && make -j8 EFS_GIT_ID=<commit> all && python3 tests/test_k36_recovery.py'
```

Prerequisites: `/dev/fuse`, `fusermount3`, Python 3; ports chosen by the
test (never 17xxx). `EFS_K36_KEEP=1` keeps the work directory; otherwise the
test removes it. The gate is `make -k EFS_GIT_ID=<commit> test-all` in the
same tree.
