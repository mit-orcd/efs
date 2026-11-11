# K106 (a): a request in flight to a member that halts (Task FR)

**Status (Nov 10):** source commit `66f71aa51` on branch `claude/loving-chebyshev-3e1e81` (devel `67fc3b4a1`), **unmerged, not pushed**.
Verdict: **PASS for the covered paths, K106 (a) not closed for the uncovered ones**: the first `mkdir` after SIGSTOP of the leader waits 6.06 to 6.11 s (5 runs) instead of 30.09 to 30.52 s (5 runs on devel), the slow-live-peer test shows no cut, the 8-job gate ran no exchange and cut no conn outside the four tests of this task. Remaining failures: none caused by this change. TSan lane: `k37_segment_rebuild` fails (`packer moved 0 of 4`), the test race recorded in Task FL's page (it fails on devel too); gate `NOTRUN` rows are RDMA/host limits. Left waiting by design: the paths in design note 1 and 3. Next action: the coordinator merges and changes row K106 and the counts (this task did
not touch them); a distinct-host run is owed (see "What is not verified"). Row [K106](../k/K106.md) stays OPEN until then.

Task FR, Nov 10 2026, intel1 (private clusters, ports 18700 to 18799, `~/efs-taskFR/`). Purpose: remove wait (a) of row K106, the
30 s a client request waits when the member it is in flight to halts (SIGSTOP, a hung host: the kernel accepts TCP, nothing
replies), and prove the fix does not cut a request to a member that is slow but alive. Design input: the section "(a)" of
[Task FL's page](k106-halted-member-waits-20261110.md).

## Design note

**1. Where the hook sits.** `efs_recv_all` (`src/common/network.c`) is shared with the servers and is unchanged. The hook is a client
function, `efs_client_conn_await_reply(node_id, conn)` in `src/client/node_cache.c`, called by the client immediately before it blocks
in `efs_conn_recv_msg`: it polls the conn's descriptor in slices of `EFS_NODE_PROBE_MS`, so the following receive finds the reply
already readable (or the conn already shut down). Receive paths that block on a reply in `src/client/` (`graphify explain` of the
callers of `efs_conn_recv_msg`, plus the typed variants):

- **Covered:** `rpc_send_recv_shard_once` (`inode_rpc.c`), the one loop every metadata request through `rpc_send_recv_shard`, `_owner` and
  `_admitted` goes through (CREATE/mkdir, UNLINK, LINK, RENAME, SETATTR, GETATTR, LOOKUP, FLOCK, XATTR, REPORT and the rest of
  the `INODE_*` types) and the first `mkdir` of the K106 text.
- **Left waiting as today (not hooked):** `rpc_send_recv_dual_once` and the specialised senders in `inode_rpc.c` (readdir, lookup-path, lease,
  lane and publication calls: about fifteen `efs_conn_recv_msg` sites), the PUT replies (`efs_conn_recv_put_reply`, `write.c`), the
  segment and chunk reads (`efs_conn_recv_msg_into`, `efs_conn_recv_segment_reply`, `read.c`), the data commit
  (`data_commit.c`), the declaration and view exchanges (`node_cache.c`, which have their own bounds), `efs_bench.c`. Reason: each has
  its own retry and placement logic (PUTs learn the halted member from the 5 s checkouts of Task FL, reads fall back to other
  fragments); one site is the proven K106 case, and every added site needs its own replay-safety argument. They are the follow-ups if
  the distinct-host run shows a wait there.
- **RDMA conns** (`conn->rc`) are left waiting (the poll is on a TCP descriptor).

**2. How a waiter learns, and one exchange per node.** A waiter registers its conn (`conn_wait[node][slot]`, under `conn_lock`) before it
polls. After `EFS_NODE_PROBE_MS` (1 s) without a readable byte it takes `node_exch_busy[node]`; the waiters that find it taken keep polling
their own conn (they re-check after the next slice; a waiter does not start an exchange when one ended less than
`EFS_NODE_PROBE_MS` before: `node_exch_end_ms`). The owner of the flag runs `node_exchange` (connect, protocol declaration, the existing
5 s bound, on its own thread). **Answered:** the member is alive, the request is slow, everyone keeps waiting; the next exchange is
at least `EFS_NODE_PROBE_MS` later, so a slow member sees at most one connect and declaration per second per node. **Silent for the
whole local bound** (the verdict the existing code already uses to mute a member; a reset, a refusal or the waiter's own deadline are not
silence): the same lock section sets the down mark (`EFS_NODE_DOWN_FAILS` streak, `now + EFS_NODE_DOWN_MS`, the 30 s that
exist) and `shutdown(SHUT_RDWR)`s every registered conn of that node, then logs `node-waiter-cut: node=N conns=K`. `shutdown` makes the
other waiters' `poll` return readable and their receive fail with `ECONNRESET` (EOF): that is how they learn, with no new channel. The
down mark is cleared only by the existing rules (an answered declaration), unchanged. Only conns registered by a covered waiter are
cut, not every checked-out conn: a PUT or a read on the same node is left to its own path.

**3. Which requests are safe to cut.** The cut produces exactly the failure the 30 s `recv` timeout produced, only earlier: `rpc_send_recv_shard_once`
already treats a failed receive as "drop the conn, `rpc_retry_peer` (note the failure, forget the leader), pick another voter, retry". So no
new retry path is introduced; the question is only which requests that path may be taken early for. The hook is called only for
`!rpc_type_mutates(type) || rpc_identified(type, req_len)`: every read and probe (the retry re-asks), and the four directory mutations that
carry an op-id (`INODE_CREATE`, `UNLINK`, `LINK`, `RENAME_AT` with `EFS_OPID_WIRE_LEN` suffix): `opid_retry_ack`
re-sends the identical request (`create_retry`) and the server's op-id receipt window answers a same-identity re-send with the recorded
verdict (`opid-receipt: replay`; `rpc_resolve_identified` documents the same guarantee for the case where the budget is spent). **Not cut,
waiting as today:** the mutations without an op-id suffix (SETATTR, APPEND, HOLD, REPORT_CHUNKS, CREATE_RETAINED non-probe, FLOCK non-GETLK, XATTR set/remove, and
any CREATE/UNLINK/LINK/RENAME sent without the suffix). They are not proven replay-safe by an op-id; they keep today's behaviour (the 30 s wait).
A cut that ends in an application error rather than a retry: an identified op whose retries then run out of budget returns
`EFS_ERR_BUSY`/`NET`, after which `rpc_resolve_identified` re-asks by identity as it does today; a cut inside the last seconds of the 30 s
operation budget leaves the same few-seconds retry window as any other transport failure.

**4. The member answers but the request never completes.** The wait keeps its existing bound: with an operation deadline the receive ends at it
(the hook returns when the thread's remaining time is 0); without one (LOOKUP and other deadline-less calls) the hook ends the wait at the
conn's recv bound (`recv_timeout_ms`, `EFS_IO_TIMEOUT_MS` = 30 s, the same figure the socket timeout gave) and shuts down its own conn so the receive
fails at once instead of waiting a second 30 s. Nothing is raised, shortened or added: the numbers 1 s and 5 s exist already.

## Tests

`tests/test_node_waiter_cut.py` (four scenarios in round 1, seven after round 2 below; each a Makefile and `tests/budgets.tsv` row, also in the TSan lane of
`tests/sanitizer_check.sh`) runs the production `efs_client_conn_await_reply` and `node_exchange` with a peer that accepts TCP, over real
socket pairs. No wall-clock bound is asserted: a missed cut hangs a waiter and the alarm fails the test. `EFS_NODE_PROBE_MS` is scaled to 40 ms
and `EFS_IO_TIMEOUT_MS` to 300 ms in the test.

| scenario | what it checks | devel `67fc3b4a1` | fixed |
|---|---|---|---|
| `halted` | the peer never answers the declaration: the waiter's conn is shut down (receive gets EOF), the member is down and mute, one `node-waiter-cut ... conns=1` line | fails (no hook) | PASS |
| `slow` | the declaration is answered, the original reply arrives later: the conn stays open, the reply is read, member not down, no `node-waiter-cut` line | PASS (it is only the receive) | PASS |
| `two` | two waiters on one node, a slow exchange: one declaration, both conns cut, one line with `conns=2` | fails | PASS |
| `noreply` | declarations always answered, the reply never comes, no deadline: the wait ends at the conn's recv bound, conn shut down, member not down, no cut line | fails | PASS |

Mutation checks (the fixed source edited, the test run): dropping the one-exchange-per-node guard fails `two` (two declarations); cutting on any non-failed verdict
instead of only on silence fails `slow`. Existing tests that splice `node_cache.c` or `inode_rpc.c` (`test_node_probe_exchange.py`,
`test_node_reprobe.py`, `test_metadata_retry_budget.py`, `test_w155_identified_resolve.py`) pass; the hook function sits after
`efs_client_conn_drop` so the spliced regions of the first two are unchanged; the last two got a no-op stub of the hook.

## Source, builds and environment

- Devel `67fc3b4a1` (before) and branch commit `66f71aa51` (after; the sources under `src/` are those of `e7e096c6f`, `66f71aa51` changes tests only), built from `git archive`
  on intel1 with `make -j8 all tests/faults/efsd`, gcc 14.3.1 `-O3`, TCP. Binary identity (SHA-256 prefix): devel `efsd` 208b48b6, `efs-fuse` faff7c27,
  `tests/faults/efsd` 503c5e5f; fixed `efsd` fd1f3cfc, `efs-fuse` a95f6c7c, `tests/faults/efsd` a920d6df. The (c) tree carries Task FL's finisher-off `sed`
  (`efsd` 287bd64c). The loaded-gate tree carries one measurement overlay that is **not in the commit**: after the `node-waiter-cut` line it appends one record (node, cut conns,
  pid, `TMPDIR`) to a file for every exchange a waiter ran, whether or not it cut, because a passing test deletes its own logs and the gate keeps none
  (`efs-fuse` 53f1e619). Full hashes in `binaries.txt` of the artifacts.
- intel1: 8 CPUs, Rocky/EL10 kernel 6.12.0-211.67.1. Three `efsd` of the `EFS_FAULTS` build (tcp), one `efs-fuse` through node 1, node 3 halted; ports 18700 to 18799 only
  (`scripts/evidence/ff/port_range.py`, slot per run, five runs in parallel on the 8 CPUs). `/run/screen` is not mode 777 on intel1: `SCREENDIR` under the task tree.
- Raw output stays outside Git: `~maik/efs-artifacts/taskFR-20261110/` on intel1 (run outputs `dev-a-*`, `fix-a-*`, `fix-b-1`, `fixc-c-1`, controls `*-b-nohalt-*`, the gate's `gate2-summary.txt`,
  `gate2-timings.tsv` and `gate2-cuts.log`, `make-test.txt`, `tf-*.txt`, `asan.txt`, `tsan.txt`, build logs, `binaries.txt`, `fixtures.tar.gz` with the daemon and client logs of the runs);
  its `SHA256SUMS` has SHA-256 `bf023bbe5b29772ca40a1166422d7bea811e978225f9e5a989e28b18dc392331`.

## Reproduction

```bash
git archive <commit> | ssh intel1 'mkdir -p ~/efs-taskFR/fix && tar -x -C ~/efs-taskFR/fix'     # and devel into .../dev
cd ~/efs-taskFR/fix && make -j8 all tests/faults/efsd
export TMPDIR=~/efs-taskFR/tmp SCREENDIR=~/efs-taskFR/screens   # /run/screen is not mode 777 on intel1
# one command per row, SLOT 0 to 4 picks the 12-port range (run in a screen):
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" a halt      # (a) ~50 s on devel
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" b halt      # (b); nohalt = control
python3 scripts/evidence/ff/port_range.py "$PWD" SLOT scripts/evidence/fl/halted_waits.py "$PWD" c halt 60   # (c), finisher-off tree
# loaded-cluster check (8-job load):  TMPDIR=~/efs-taskFR/gtmp EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
#   then: grep -rl node-waiter-cut $TMPDIR      (the gate's client logs)
```

For (c) the finisher-off tree carries the sed of Task FL's page (`h->fin_running = 0`) before the build. Stop by pid, remove the
trees with `~/bin/edelete --force --delete`; raw output stays outside Git.

## Results

### (a) The first `mkdir` after SIGSTOP of the metadata leader (both groups led by node 3, 5 warm `mkdir`, SIGSTOP, 3 timed `mkdir`)

| run | devel `67fc3b4a1` first / 2nd / 3rd `mkdir` | fixed first / 2nd / 3rd `mkdir` |
|---|---|---|
| 1 | 30.33 / 0.00 / 0.00 s | 6.06 / 0.00 / 0.00 s |
| 2 | 30.09 / 0.00 / 0.00 s | 6.06 / 0.00 / 0.00 s |
| 3 | 30.52 / 0.00 / 0.00 s | 6.06 / 0.00 / 0.00 s |
| 4 | 30.31 / 0.00 / 0.00 s | 6.11 / 0.00 / 0.00 s |
| 5 | 30.36 / 0.00 / 0.00 s | 6.11 / 0.00 / 0.00 s |

No `errno` in any run (a sixth fixed run while checking the hook: 6.06 s). The 6.06 s is the 1 s wait plus the 5 s exchange; the election added nothing measurable. In every fixed run the client log has one
`node-waiter-cut: node=3 conns=1 ...` line followed by `inode-rpc: retry type=47 attempt=0 why=recv rc=-6 nid=3` (the existing retry path).

### (b) and (c) unchanged (fixed tree)

| wait | before this change (Task FL, fixed `f8c12bdef`) | this change |
|---|---|---|
| (b) 30 writes, node 3 halted | 13.1 s (7.04, 5.03, then 0.03 to 0.04 s), no error | 12.8 s (7.03, 5.04, then 0.02 to 0.04 s), no error |
| (c) first `txn-recover ... ABORT` after the fault (finisher off, 60 s window) | 7.7 s (one run); 5.1 to 6.9 s earlier runs | 6.3 s, no error (longest unlink 6.92 s) |

Hot-path control, (b) with no halt, 30 writes: devel 1.4, 1.2, 1.4 s; fixed 1.3, 1.4, 1.5 s (the extra `poll` per metadata reply is not measurable in it).

### Loaded-cluster check (full gate, `EFS_TEST_JOBS=8`, fixed tree plus the overlay)

- First gate run: `FAIL 5`: my four scenarios of `test_node_waiter_cut.py` (gcc 14 `-Werror=misleading-indentation` in the test's stub, invisible to the Mac compiler) and
  `test_create_retry_identity.py` (it splices `inode_rpc.c` and needs a stub of the new hook). Both fixed in `66f71aa51`; no product code involved.
- Second gate run on the final tests: `TOTAL 611 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (743s) OVERBUDGET 11`. The twelve `NOTRUN` rows are three flagged host limits
  (no `/home/maik/git/cluster`, no valgrind header, RDMA hardware) and nine RDMA tests on a host without an IB device; the eleven `OVERBUDGET` rows are older tests (none of this task's).
- `node-waiter-cut` and exchanges: the overlay file holds 10 records, **all from this task's four scenarios** (`case-273` halted: 1 cut of 1 conn; `case-275` two waiters: 1 cut of 2 conns;
  `case-274` slow and `case-276` noreply: exchanges that ran and cut nothing). **Outside them: no waiter ran an exchange and no conn was cut** in any other test of the gate, the tests that SIGSTOP or kill a member on purpose included (the overlay recorded nothing for them; I did not trace which of them reach a covered reply wait, so they are not shown to exercise the hook).
  The grep of the leftover logs under the gate's `TMPDIR` is empty for the same reason and, alone, weak: a passing test deletes its logs.

### Other checks

| check | result |
|---|---|
| `make test` | rc 0 |
| `tests_for.py client/node_cache --run`, `client/inode_rpc --run` | rc 0 each |
| `bash tests/sanitizer_check.sh asan` | VERDICT PASS (62 run, 0 failed) |
| `bash tests/sanitizer_check.sh tsan` | VERDICT FAIL (36 run, 1 failed): `k37_segment_rebuild`, `AssertionError: refused files once node 3 is back: packer moved 0 of 4`, the test race recorded in [Task FL](k106-halted-member-waits-20261110.md) (fails on devel too). `waiter_cut_halted`, `_slow`, `_two`, `_noreply` PASS under TSan |
| test timings (gate, intel1) | the four scenarios run in 0.1 to 0.4 s; budget 5 s each (1.25 x slowest rounded up) |
| `python3 scripts/check_k_register.py`, `check_doc_links.py`, `docs/gen-architecture-full.py`, `make docs-check`, `rebuild-code-graph.sh`, `tests/test_code_graph.py` | PASS (K register 113 rows, 0 dangling links, architecture pages regenerated unchanged, `make docs-check` rc 0; graph rebuilt in the last commit, `tests/test_code_graph.py` OK) |

## Round 2 (Task FW)

An independent review of the change found nothing blocking and four things to repair. Branch `claude/sleepy-curie-5c8445` on the wave 77 base `20d58f67c`. Raw output stays on intel1 in `~maik/efs-artifacts/taskFW-20261110/` (`SHA256SUMS`).

| finding | what changed |
|---|---|
| F1 `tests/test_node_waiter_cut.py` had a fallback that built against a tree without the hook (`HAVE_AWAIT`), a `usleep` and a polling loop | The fallback is gone: the test builds only against a tree that has the hook. The stub declaration now blocks on a semaphore that the test releases, and the test waits on a second semaphore for the exchange to start: no sleep, no polling. |
| F2 the test forced the operation deadline to 0 and replaced `slot_for_conn`, `pool_size` and `node_index_for_id`; the gating at `inode_rpc.c` was untested | The real `pool_size`, `slot_for_conn`, `node_index_for_id`, reap and release are spliced from `node_cache.c`. The stub deadline is real when a scenario asks for it. The gating condition is a function, `rpc_reply_wait_cut` in `inode_rpc.c`, spliced into a new scenario. New scenarios `deadline`, `race`, `gating` (Makefile, `tests/budgets.tsv` 5 s, `tests/sanitizer_check.sh` rows). |
| F3 a cut could shut down a waiter whose reply was already readable; it then read the reply and released a shut-down conn to the pool | The cutter skips a waiting conn whose reply is readable (`poll` with timeout 0 under `conn_lock`). A conn the cut shut down is marked (`conn_wait` value 2, no new field); `efs_client_conn_release` destroys it and frees the slot instead of pooling it, `efs_client_conn_drop` clears the mark. |
| F4 `node_exch_busy` and `conn_wait` were never reset by the init function | `efs_client_conn_init` resets `conn_wait` (idle slots), `node_exch_busy` and `node_exch_end_ms`. |

The set of cut requests is unchanged and `EFS_NODE_PROBE_MS` and every other bound are unchanged.

Scenarios: `deadline` (deadline 20 s away, member halted: the wait ends at the exchange verdict, more than 10 s before the deadline); `race` (two waiters, one reply readable before the verdict: that waiter keeps its conn, reads its reply and keeps its pool slot on release; the other is cut and its conn is destroyed at release); `gating` (every request type 0 to 255 plus the body-dependent FLOCK, XATTR and CREATE_RETAINED cases: cut exactly for reads and the four directory mutations with an op-id; SETATTR, APPEND, HOLD, REPORT_CHUNKS, FLOCK set, XATTR set and remove, CREATE_RETAINED non-probe and op-id-less directory ops are not cut).

Mutation proof (intel1, outputs `mutation-*.txt`): removing the readable skip fails `race` (the readable waiter's conn was shut down); removing the cut mark fails `race` (the cut conn is not marked); adding SETATTR to the cut set fails `gating` (`type 63 cut=1`). Each reverted.

Results of the other commands of this round: all on intel1 (gcc 14, same tree as the commit plus the stub lines named here): `make` rc 0; the seven scenarios of `test_node_waiter_cut.py` rc 0 each; `test_create_retry_identity.py`, `test_metadata_retry_budget.py`, `test_w155_identified_resolve.py` (their hook stub also needs a stub of `rpc_reply_wait_cut`, added; they failed with an implicit declaration before), `scripts/tests_for.py client/node_cache --run`, `make test` rc 0, `test_make_test_recipe.py` rc 0; `sanitizer_check.sh asan` VERDICT PASS (65 run, 0 failed), `tsan` VERDICT PASS (41 run, 0 failed; `k37_segment_rebuild` did not fail this time). The first run of `make test` and ASan failed only on the three tests that splice `inode_rpc.c` before their stub was added. Live measurement (the page's (a), first `mkdir` after SIGSTOP of the leader, three runs): 6.07, 6.12, 6.07 s, the two following `mkdir` 0.00 to 0.01 s, no errno. Raw output (`live-*.log`, `mutation-*.txt`, `*.log`, `SHA256SUMS`): intel1 `~maik/efs-artifacts/taskFW-20261110/`, `SHA256SUMS` SHA-256 `d0a900a071e9979ef561c7736e3c5fd309703d22958abb4d3b36165c0f2b959b`; `~/efs-taskFW` removed. Reproduction: the commands of the Reproduction section, `python3 tests/test_node_waiter_cut.py SCENARIO` for the seven scenarios

### Known limits (review notes)

- One silent exchange marks the member down at once (the checkout path otherwise needs four failures). A live member that cannot accept a new connection for 5 s (descriptor exhaustion, a long process stall) is marked down until a probe is answered.
- The waiter that runs the exchange does not read its own connection during it, so its own reply can be delivered up to 5 s late.
- A powered-off or partitioned host is not covered: the connect times out, the verdict is "failed", and the wait stays 30 s. A request whose deadline is less than 5 s away is not covered either.
- Every hooked reply costs two lock acquisitions, a 64-slot scan and one `poll`; this was measured only with 30 writes.

## What is not verified

- **A distinct-host run is owed** (private TCP cluster on one machine only; no RDMA, no real hung host, three members).
- **K106 (a) is closed only for the covered paths.** Still waiting the 30 s on a halted member: mutations without an op-id (SETATTR, APPEND, HOLD, REPORT_CHUNKS, FLOCK, XATTR set/remove, CREATE_RETAINED,
  op-id-less directory ops), RDMA conns, `rpc_send_recv_dual_once` and the specialised `inode_rpc.c` senders, PUT/read/data-commit receives. Giving the non-identified mutations an op-id (so they can be cut) or
  hooking the next site is a follow-up for the coordinator's decision; I did not measure any of them.
- The accepted risk of the direction is only partly tested: a live member that stays silent to a new declaration for the whole 5 s (a very long stall) has its replay-safe requests cut and retried.
  The slow-peer test shows an answering member is left alone; the gate shows no cut under the 8-job load of one host (a cluster of three daemons per test), not under a real production load.
- The overlay measures exchanges and cuts per test; it does not show requests that waited less than 1 s.
- Not rerun: K34/K35/K36 as separate live runs (they ran inside the gate); the SIGCONT behaviour of mode `r`; (c) with the finisher enabled.
