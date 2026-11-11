# PUT lifecycle description and extraction measurement (Task V5, Nov 11, 2026)

**Current state (Nov 11):** the one description of how a PUT completes and what bounds it is [put-lifecycle.md](../../how-it-works/put-lifecycle.md), checked by `scripts/check_put_lifecycle.py` (wired into `make docs-check`) and linked from the PUT-window contract, its guide and the write-path map. Source tree: `devel` at `0bffb131d`; no code moved, one comment sentence added to `src/client/put_window.h`. Measured by the inventory method: the whole "PUT completion" candidate (25 functions, 1,367 lines) has 12 outside static callees and 6 outside callers, so it is not a clean owner; its fragment-PUT part (17 functions, 989 lines) has one entry point and 4 outside callees and is. Recommendation: extract the fragment PUT, not the pool and pipe. Verdict: PASS for the description and checker; NOTRUN for any run on a cluster (Mac only). Remaining: the reader trial the coordinator will run; items under "Not determined".

Linked item: structure review of Nov 11 (Task V5). Binary identity: none (nothing built).

## Method and reproduction

- Read the code named in the page, using `graphify explain "client/put_window"` and the [write-path map](../../how-it-works/write-path-map.md) to locate it. Line numbers below are at `0bffb131d`.
- Checker: `python3 scripts/check_put_lifecycle.py`.
- Inventory: `python3 scripts/evidence/v5/lifecycle_inventory.py src/client/write.c --functions` (a copy of the V2 script; only the struct names, the base-type rule and the test-pattern name changed; the candidate lists are `scripts/evidence/v5/candidate_*.txt`). Exit 1 if a listed name is not defined once. Raw output is not committed; the numbers are below.
- Environment: macOS, Python 3, no cluster, no build.

## Code read, per question

1. **Steps.** `efs_dcache_flush_ino` `src/client/write.c:7641`; `dcache_flush_ino_pass` 7588 (steal 7615, `flush_pipe_add` 7623, drain 7627; the reclaim thread uses the same pipe at 8943 and 8947); `dcache_steal_dirty_budgeted` 6941 (window open 7005, merge-base fetch 7029); in-line `dcache_flush_slot_inner_budgeted` 7286 (open 7342, PUT 7405, close 7414); `flush_pipe_add` 7198, `flush_pipe_drain` 7170 (`put_pool_run` 7176, close 7183/7190); `put_pool_run` 9157, `put_pool_thread` 9073, `chunk_put_worker` 9230; `dcache_put_now_budgeted` 4898 (PUT 4933, `putid_note` 5047, `efs_client_put_release(…,1)` 5085, mark dirty 5086); `efs_client_put_fragments_parallel` 4120; `client_write_run` 9379 calls it directly (9448), the pool is called only at 7176 and 9591.
2. **Deadlines.** `put_pool_run` 9164 (`previous ? previous : now + 30000`); job deadline copied 9184; worker sets it 9096; late job gets BUSY 9098; restore 9226. `efs_client_put_fragments_parallel` 4130-4133 (`EFS_IO_TIMEOUT_MS` = 30000, `include/efs/common.h:75`). Attempt budget `put_fragments_parallel_once` 3301-3304 (min of 30 s and the outer `D`). Connection checkout `src/client/node_cache.c:762-766` (`EFS_NODE_ADMISSION_MS` 5000, capped by `D`). `report_retry_pause` 1996-2006. Poll slice cap 3572, drain cap 3627. Origin of `D`: `efs_append_flush_report` `src/client/efs_fuse/append.c:111-120` (min of inherited and now + 30 s), called by `efs_fuse_fsync_ino` and `efs_fuse_flush_ino` (`efs_fuse/write.c:576, 625`); REPORT narrowing `report_dirty_ino_complete` write.c:2719-2727; window wait `put_window.c:94-100` (min of `max_ms` and `D`); flush wait 7656.
3. **Retries.** `put_fragments_parallel_run` 3934: loop 3958 (128), resend once 3993, BUSY resend 4018, re-probe 4028, hard failures 4100 (4) with pauses 4103; placement loop `efs_client_put_fragments_parallel` 4156-4228 (conditions 4166 and 4206, at most 3 placements); one more pass 7662.
4. **Member down.** Skip of down members `put_fragments_parallel_once` 3337; fewer than 2 live 4069-4090; quorum loop 3425 (`acks < 2`), drain 3617-3690, verdict 3705-3715 (STALE, QUOTA, OK, BUSY, NET, NO_QUORUM); degraded record 4039, `degraded_record` 3891; distinct-node refusal 4140-4150.
5. **Outstanding.** In-flight third reply dropped at write.c:3692-3700 (the last loop of `put_fragments_parallel_once`, after the drain). Ledger: `put_ticket_owner.c:280` (`efs_client_put_admit`; waits at 294 and 313: 50 × 20 ms), release 328, done 344, `efs_client_put_settle` 577 (deadline cleared 581, 64 rounds 583), `settle_main` 601 (250 ms wait). Unmount `node_cache.c:1158`; REPORT only kicks settle `write.c:2771`.
6. **Completion.** `put_recv_reply` 3005; stripe note 4200 (`data_commit.c:214`); `dcache_put_now_budgeted` 4898-5090 (cache entry recorded at 5060); `dcache_flush_keep` 7104 (re-dirty 7113-7120); `dcache_retire_gone` 7141; errno maps `efs_fuse/namespace.c:382`, `efs_fuse/support.c:1044`; stalled inode `write.c:9504`.
7. **Window.** Open 7005 / 7342; close 7183, 7190, 7213, 7414; wait 7656; reader wait `put_window.c:133-138` and `read.c:2280, 2384`.

## Checker mutation result

`chunk_put_worker` misspelled as `chunk_put_workr` in section 1: the checker exits 1 with `check_put_lifecycle: section 1: chunk_put_workr is not defined in src/client/write.c`. Restored: exit 0.

## Inventory (`src/client/write.c`, 9,695 lines, 275 functions with a body)

State of the candidate: structs `chunk_put_job` (23 fields), `put_batch` (4), `flush_pipe` (5); a field reference counts only when its base variable has a candidate struct type (353 of 562 references on shared names have an unknown base and are dropped).

| Measure | A: PUT completion | B: fragment PUT | C: pool and pipe |
|---|---|---|---|
| Functions / lines | 25 / 1,367 | 17 / 989 | 7 / 367 |
| File-scope `static` variables used (all only inside) | 6 (`g_put_sh` `g_put_pool` `g_put_init` `g_node_away_until_ms` `g_node_full_until_ms` `g_node_spare_probe_ms`) | 3 (the node marks) | 3 (`g_put_sh` `g_put_pool` `g_put_init`) |
| Struct fields used / only inside | 30 / 19 | 0 / 0 | 30 / 19 |
| References to that state from outside | 0 variable refs outside; 37 of 205 field refs outside (82% inside) | 0 | 0 variable refs; 37 field refs outside |
| Distinct outside callers | 6 | 3 | 3 |
| Outside static callees | 12 | 4 (`report_clock_ms` `report_retry_pause` `dgobj_note` `put_reply_fault`) | 10 |
| Non-static entry points named in other files | 1 (`efs_client_put_fragments_parallel`) | 1 | 0 |
| Test files naming a function / of them reading `write.c` as text | 13 / 11 | 6 / 4 | 5 / 5 |
| Functions named by no test | 0 | 0 | 0 |

A's outside callers: `write_chunks_no_replicate`, `dcache_flush_ino_pass`, `dcache_reclaim_main`, `client_write_run`, `dcache_put_now_budgeted`, `report_dirty_ino_run` (for `report_retry_pause`). A's 12 outside callees include four cache functions (`dcache_flush_keep`, `dcache_store`, `dcache_put_now`, `dcache_retire_gone`), two encode helpers, `dgobj_note` and `report_clock_ms` (15 calls).

Hot functions of A (per chunk or per fragment): `put_pool_thread` `chunk_put_worker` `flush_pipe_add` `efs_client_put_fragments_parallel` `put_fragments_parallel_run` `put_fragments_parallel_once` `put_recv_reply` `put_reply_pollfds` `put_drain_verdict` `reroute_untried_fragments` `node_is_full` `node_is_away` `node_note_back` (13). Per batch: `put_pool_run` `flush_pipe_drain`. Rare (once, per pass, retry or failure only): `put_pool_ensure` `flush_pipe_init` `degraded_why` `degraded_ack_log` `degraded_record` `put_quota_has_spares` `node_note_full` `node_note_away` `spare_reprobe` `report_retry_pause`. Classified by reading the call sites, not by a profile.

**Recommendation: extract B, not A.** A is cut where the code is coupled: `chunk_put_worker` and the pipe talk to the cache in 4 places and to the encode helpers, so C would need a cache interface wider than the one entry point it removes. B has one entry point (`efs_client_put_fragments_parallel`), 3 private variables, no shared structs and 4 outside callees, of which two are small helpers (`report_clock_ms`, `report_retry_pause`) shared with REPORT and one is a fault hook. The smallest coherent boundary is B plus nothing: its 17 functions in a new file behind that one declaration, with `dgobj_note`, `report_clock_ms`, `report_retry_pause` and `put_reply_fault` declared by the owner of the REPORT/degraded table. It holds the answer to "what bounds the PUT itself": the attempt deadline, retries, quorum and degraded record. The pool and pipe stay with the flush code that owns their error handling. Cost not measured here: the per-chunk cross-file call (one call per chunk against a network round trip; no profile was run) and the 4 test files that read `write.c` as text.

## Not determined

- Whether a blocked `efs_conn_send_msg_parts` can run past `D`: the sockets carry a 30 s send timeout (`node_cache.c:276, 611, 848`), `put_fragments_parallel_once` polls for `POLLOUT` first (3390) before the send (3410) but does not bound the write itself. A fault-injection run on a stalled receiver would decide it.
- Whether every wait inside `efs_client_put_admit` (`efs_client_rpc_put_ticket`, `efs_client_session_admit`) is capped by `D`: the RPC layer (`inode_rpc.c:935-960`) loops on `D` only when one is set; the call at `put_ticket_owner.c:307` was not followed into transport.
- What `dcache_store` does with the bytes of a failed `chunk_put_worker` (whether it marks them dirty): not read.
- The longest flush with no `D` for a caller other than the reclaim thread and `efs_dcache_flush_all` (`efs_fuse_truncate_ino` was not followed).
- Whether `ll_write` sets a deadline: `efs_client_write` was read, the FUSE write handler was not.
- How the reader path handles the window wait result beyond returning it (`read.c:2280, 2384` return a negative code).
- Real PUT times under a stopped member: no cluster run.

## Discrepancy found

`docs/how-it-works/write-path-map.md` ("PUT pool and write entry") says `client_write_run` runs chunk jobs through the PUT pool. The code does not: `client_write_run` PUTs each chunk in order on the calling thread; `put_pool_run` has two callers (`flush_pipe_drain`, `write_chunks_no_replicate`). Left unchanged (outside this task's edit scope); the new page states the code's behaviour.

## Limitations

Read by one agent; the coordinator reviews it against the source and runs a reader trial. Line numbers marked "area" are approximate to a few lines. The inventory classifies hot functions by reading, and resolves field references by variable names, which can miss a struct reached through a differently named variable.
