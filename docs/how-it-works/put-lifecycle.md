# PUT lifecycle: how a PUT completes and what bounds it

Checked by `scripts/check_put_lifecycle.py` (every function on a `Functions:` line is defined in the file named before it). Line numbers are in [the evidence page](../status/2026-11/put-lifecycle-description-20261111.md). `D` below is the thread deadline (`efs_client_rpc_deadline_ms`, a per-thread monotonic time; 0 means none).

## Summary

1. One dirty chunk is stolen out of the cache, copied into a flush job, PUT as three fragments to three members, recorded in the cache and the ticket table, and later published by a REPORT. The PUT window is open from the steal to the end of the batch drain.
2. The end-to-end deadline `D` belongs to the caller. Where there is none, `put_pool_run` (per batch) or `efs_client_put_fragments_parallel` (per PUT) sets now + 30 s. Waits below it are capped by `D`, except two untimed condition waits that end at another thread's `D` (section 2).
3. Two acknowledgements of three end an attempt early. The stripe is then complete only when the third fragment is acknowledged, or placed around, or recorded as degraded.
4. A failed PUT re-dirties its chunk and returns the first error of the batch. `efs_dcache_flush_ino` never retries a failed pass; the flush is run again only by its callers (section 3).
5. A window waiter waits for steal, merge-base fetch, batch PUT and drain, not for one PUT. That can exceed its 8 s bound, so it can get `EFS_ERR_BUSY` while nothing is wrong (the application sees `EIO` from close or fsync, `EBUSY` from read or truncate). That it happens on a healthy cluster follows from the bounds; it was not measured.

## 1. From a write to a durable chunk

- `efs_dcache_flush_ino` (close, fsync, truncate with an 8 s `D`, the held-unit retry, `fuse_unit_begin`, the stop drain `efs_dcache_drain_pending`): runs `dcache_flush_ino_pass`, waits on the PUT window, runs one more pass if it waited.
- `dcache_flush_ino_pass`: lists the inode's dirty chunks (`dcache_dirty_cis_of`), steals each, adds it to a `flush_pipe`.
- `dcache_steal_dirty_budgeted`: copies the body, marks the slot clean, opens the PUT window (`put_win_open`), fetches the merge base (`dcache_fetch_merge_base`) when the write is not a whole-chunk overwrite.
- `dcache_flush_slot_inner_budgeted`: the in-line variant (reclaim of a slot, `dcache_flush_all_slots`): same steps, but the PUT runs on the calling thread and the window closes right after it.
- `flush_pipe_add` / `flush_pipe_drain`: collect up to `EFS_WRITE_PIPELINE` (32) jobs; the drain runs them with `put_pool_run`, then re-dirties failures (`dcache_flush_keep`) and closes each window (`put_win_close`). The reclaim thread uses the same pipe.
- `put_pool_run`: queues the jobs on the PUT worker threads (`put_pool_thread`) and waits until all are done; `chunk_put_worker` runs one job (`dcache_put_now` for a flush job).
- `dcache_put_now_budgeted`: encodes, hashes, places, calls `efs_client_put_fragments_parallel` (again as a full image when the chunk's span cannot be added), then records the object in the chunk table (`efs_client_set_chunk`; `putid_note` keeps the PUT id) and the cache entry, and marks the chunk dirty for REPORT (`efs_client_mark_chunk_dirty`).
- `efs_client_put_fragments_parallel`: admits a ticket (`efs_client_put_admit`), runs `put_fragments_parallel_run`, places again if needed, notes the stripe (`efs_client_stripe_note`).
- `put_fragments_parallel_run` / `put_fragments_parallel_once`: retries, and one attempt (send three fragments, wait for replies, `put_recv_reply` per reply).
- REPORT (`report_dirty_ino_run`) publishes the recorded chunks; `efs_client_put_release(seq, 1)` marks the ticket landed; a later settle retires it.
- The synchronous `client_write_run` path does not use the pool: it calls `efs_client_put_fragments_parallel` once per chunk, in order, on the calling thread. The pool is used by the flush pipe and by `write_chunks_no_replicate`.

Functions: `src/client/write.c`: `efs_dcache_flush_ino` `dcache_flush_ino_pass` `dcache_dirty_cis_of` `dcache_steal_dirty_budgeted` `dcache_fetch_merge_base` `dcache_flush_slot_inner_budgeted` `dcache_flush_all_slots` `flush_pipe_add` `flush_pipe_drain` `dcache_flush_keep` `put_pool_run` `put_pool_thread` `chunk_put_worker` `dcache_put_now_budgeted` `efs_client_put_fragments_parallel` `put_fragments_parallel_run` `put_fragments_parallel_once` `put_recv_reply` `putid_note` `report_dirty_ino_run` `client_write_run` `write_chunks_no_replicate`; `src/client/put_ticket_owner.c`: `efs_client_put_admit` `efs_client_put_release`; `src/client/data_commit.c`: `efs_client_stripe_note`

## 2. Deadlines

- The caller owns `D`. The flush (`efs_fuse_flush_ino`, close), fsync (`efs_fuse_fsync_ino`) and the append flush set `D` = the smaller of the inherited `D` and now + 30 s in `efs_append_flush_report`; flush and REPORT both run under it. `report_dirty_ino_complete` narrows `D` to now + 8 s for the REPORT alone (`sync`).
- `put_pool_run`: `D` = the caller's, or now + 30 s when there is none. It stores it in every job and the worker (`put_pool_thread`) sets it as that thread's `D`. A job that starts after `D` gets `EFS_ERR_BUSY` without a PUT. It restores the old value on return, so a caller with no `D` gets a new 30 s for every batch.
- `efs_client_put_fragments_parallel`: when `D` is 0, sets now + `EFS_IO_TIMEOUT_MS` (30 s) and restores 0 at the end.
- `put_fragments_parallel_once`: each attempt gets now + 30 s, capped by `D`. Connection checkout is capped by `D` and 5 s (`efs_client_conn_get`). `report_retry_pause` never sleeps past `D`. Poll slices are at most 2 s.
- Longest single PUT: `D`, so 30 s when the caller has none. A send does not overshoot: with `D` set (always, inside a PUT) a TCP send is non-blocking with a poll until `D`, and an RDMA send takes the remaining time; a send that times out counts as a hard failure of that member.
- Waits are capped by `D` except two untimed condition waits that end at another thread's `D`: `put_pool_run` waiting for its jobs, and `admit_batched` (in `efs_client_put_admit`) waiting for another thread's ticket batch. The ticket RPC and the 20 ms admission waits check `D`.
- Longest flush of one inode (`efs_dcache_flush_ino`): both passes and the window wait run under the caller's `D`: 30 s from `efs_append_flush_report`. The window wait is at most 8 s and at most `D`. With no `D`, the reclaim thread drains batches of up to 32 chunks, each with a fresh 30 s, and stops after the first failed batch. `efs_dcache_flush_all` PUTs every dirty chunk in line, each with its own 30 s, and continues past failures (only QUOTA ends it); its total is not bounded.

Functions: `src/client/write.c`: `report_dirty_ino_complete` `report_retry_pause` `put_pool_run` `put_pool_thread` `efs_client_put_fragments_parallel` `put_fragments_parallel_once` `efs_dcache_flush_all`; `src/client/efs_fuse/append.c`: `efs_append_flush_report`; `src/client/efs_fuse/write.c`: `efs_fuse_flush_ino` `efs_fuse_fsync_ino`; `src/client/node_cache.c`: `efs_client_conn_get`; `src/client/inode_rpc.c`: `efs_client_rpc_set_deadline_ms` `efs_client_rpc_past_deadline`

## 3. Retries

All retries of one object use the same ticket, object name, placement and bytes (`put_fragments_parallel_run`).

- Fragment to the same member: after a hard failure (connect, send, reply) the whole attempt repeats; at most 4 hard failures (pauses about 100, 200, 400 ms plus jitter), then the error returns. Acknowledged fragments are not sent again. A missing slot whose member failed hard gets one resend on a fresh connection. A member only marked down gets one re-probe.
- BUSY or QUOTA replies: retried every 50 ms until `D`, at most 128 attempts in the loop.
- Fragment to another member: `efs_client_put_fragments_parallel` places the stripe again (`reroute_untried_fragments`) with a new ticket and object when a member refused as full or failed hard with two acknowledgements held. At most 3 placements in all; the given-up ticket is released as not landed.
- Whole stripe: the re-placement above is the whole stripe. No other layer repeats it.
- Whole flush pass: `efs_dcache_flush_ino` never retries a failed pass; its second pass runs only after a successful pass and a window wait that returned 1. A steal can also end a pass without any PUT (BUSY when a writer owns the chunk). The flush is run again by its callers: the held-unit loop in `report_dirty_ino_complete` (up to 16 rounds), `fuse_unit_begin` (up to 8 rounds) and the stop drain (`efs_control_drain` loops until its deadline). Otherwise a failed chunk stays dirty for the next close, fsync or reclaim.
- Ends: success, `D` passed (`EFS_ERR_BUSY`), a stale refusal (`EFS_ERR_STALE`), full with no spare (`EFS_ERR_QUOTA`), fewer than 2 live members (`EFS_ERR_NO_QUORUM`), or the attempt limits above.

Functions: `src/client/write.c`: `put_fragments_parallel_run` `efs_client_put_fragments_parallel` `reroute_untried_fragments` `put_quota_has_spares` `report_dirty_ino_complete` `efs_dcache_flush_ino`; `src/client/efs_fuse/release.c`: `efs_control_drain`

## 4. One member stopped or unreachable

- Skip: `put_fragments_parallel_once` does not send to a member marked down (`efs_client_node_is_down`) or to one with no connection; the slot counts as missing, not as an abort.
- If fewer than 2 members are live, `put_fragments_parallel_run` forces one re-probe (first attempt), then fails with `EFS_ERR_NO_QUORUM`.
- Quorum rule: an attempt ends early as soon as 2 of the 3 fragments are acknowledged, counting acknowledgements of earlier attempts. It then waits up to 2 s (and up to `D`) for the third reply. It returns OK with 2 only if no member refused the object as stale and fewer than 2 refused it as full.
- That OK is not final. For the missing slot, `put_fragments_parallel_run` resends once (hard failure), re-probes once (down mark), keeps retrying within `D` when the member answered BUSY (a deadline then fails the write with BUSY although 2 acknowledged), and on a full member with no spare returns QUOTA.
- A missing slot that was not sent, or not answered, for a local reason (its socket not writable within 100 ms, the attempt's budget or `D` used up) is neither resent nor placed again: the stripe is recorded degraded at once and the PUT returns OK.
- If the member is still unreachable, the caller places the stripe elsewhere when a fourth member exists. Otherwise `degraded_record` records the stripe as degraded (`dgobj_note`, so REPORT can flag it missing). If that record cannot be made the write fails with `EFS_ERR_NOMEM`.
- The caller gets OK, or one of: `EFS_ERR_BUSY`, `EFS_ERR_NO_QUORUM`, `EFS_ERR_NET`, `EFS_ERR_QUOTA`, `EFS_ERR_STALE`, `EFS_ERR_NOMEM`. A chunk whose three members are not distinct is refused with `EFS_ERR_NO_QUORUM`.

Functions: `src/client/write.c`: `put_fragments_parallel_once` `put_fragments_parallel_run` `put_drain_verdict` `degraded_record` `reroute_untried_fragments` `efs_client_put_fragments_parallel`; `src/client/node_cache.c`: `efs_client_node_is_down` `efs_client_node_reprobe`

## 5. Outstanding requests

- A request still in flight when `put_fragments_parallel_once` returns is not tracked: the third reply is awaited for at most 2 s, then its connection is dropped (`efs_client_conn_drop`).
- What is tracked is the ticket. `efs_client_put_admit` adds it to the ledger; `efs_client_put_release(seq, 1)` marks it landed, `(seq, 0)` marks it done; `efs_client_put_done` closes the ones an object superseded. `efs_client_put_settle` retires done tickets in batches; `settle_main` does this in the background every 250 ms or when kicked.
- Waiters: fsync and close never wait for settle (`report_dirty_ino_complete` only kicks it). Unmount waits: `efs_client_put_settle` (up to 64 rounds, with no deadline) after `efs_dcache_flush_all`. `efs_client_put_admit` itself waits (up to 50 × 20 ms) when the ledger is full.
- `put_pool_run` waits for every accepted job on a condition variable with no timeout. It ends because each job is bounded by `D`.

Functions: `src/client/put_ticket_owner.c`: `efs_client_put_admit` `efs_client_put_release` `efs_client_put_done` `efs_client_put_settle` `settle_main` `efs_client_put_settle_kick`; `src/client/write.c`: `report_dirty_ino_complete` `put_pool_run`; `src/client/node_cache.c`: `efs_client_shutdown`

## 6. Completion

- Fragment: acknowledged by the member (`put_recv_reply`; not yet durable, proven by the fsync COMMIT).
- Stripe: 2 or 3 acknowledgements, noted in `efs_client_stripe_note`, ticket admitted. Degraded stripes are in the `dgobj` table.
- `dcache_put_now_budgeted` complete: object recorded in the chunk table and in the cache entry, ticket released landed, chunk and inode marked dirty for REPORT. This is "PUT landed". It is not published.
- Published: REPORT acknowledged (`report_dirty_ino_run`); only then are the PUT ids dropped.
- Failure: `flush_pipe_drain` calls `dcache_flush_keep` with `put_ok` 0, which marks the entry dirty again; the first error of the batch is returned. A chunk of a removed inode is dropped instead (`dcache_retire_gone`) and a write-back error is recorded.
- Codes: the flusher gets the codes of section 4. What the application sees depends on the call: close and fsync return `ENOSPC` for QUOTA and `EIO` for every other code, BUSY and NOMEM included (`efs_fuse_flush_ino`, `efs_fuse_fsync_ino`). `efs_rc_to_errno` (read, truncate): BUSY → `EBUSY`, QUOTA → `ENOSPC`, NOMEM → `ENOMEM`, others `EIO`. `fuse_stat_errno` (the write's unit flush): BUSY → `EBUSY`, NOMEM → `ENOMEM`, others `EIO`. `efs_client_write` returns `EFS_ERR_IO` at once when the inode's write-back is stalled (`efs_wb_inode_stalled`).

Functions: `src/client/write.c`: `put_recv_reply` `dcache_put_now_budgeted` `flush_pipe_drain` `dcache_flush_keep` `dcache_retire_gone` `report_dirty_ino_run` `efs_client_write`; `src/client/data_commit.c`: `efs_client_stripe_note`; `src/client/efs_fuse/namespace.c`: `efs_rc_to_errno`; `src/client/efs_fuse/support.c`: `fuse_stat_errno`

## 7. The link to the PUT window

- The window of a chunk opens in `dcache_steal_dirty_budgeted` (or `dcache_flush_slot_inner_budgeted`) and closes in `flush_pipe_drain` (or after the in-line PUT). So it is open for: later steals of the same pass, each with a possible merge-base fetch; the batch `put_pool_run`; the drain up to this job.
- A waiter (`put_win_wait` from `efs_dcache_flush_ino`, `efs_dcache_put_win_wait` from the read path) waits for all of that, not for one PUT. It is released by `put_win_close` at once, and re-checks every 50 ms.
- The waiter's bound is the smaller of 8 s and `D`. The owner's work is bounded by its own `D`: up to 30 s for a batch with no caller deadline, after up to 31 more steals. So the bounds allow the window to outlast 8 s on a healthy cluster too (not measured), and on one with a stopped member; the waiter then gets `EFS_ERR_BUSY` (the flusher logs `dcache-flush-fail stage=put-window`).
- Nothing wakes the waiter other than a close or its own bound. The PUT's bound (section 2) is what finally closes the window.

Functions: `src/client/put_window.c`: `put_win_open` `put_win_close` `put_win_wait` `efs_dcache_put_win_wait`; `src/client/write.c`: `dcache_steal_dirty_budgeted` `dcache_flush_slot_inner_budgeted` `flush_pipe_drain` `flush_pipe_add` `efs_dcache_flush_ino`

## Question → where the answer lives

| Question | Function, file |
|---|---|
| Who sets the end-to-end deadline? | caller; `efs_append_flush_report` (efs_fuse/append.c), `put_pool_run`, `efs_client_put_fragments_parallel` (write.c) |
| What bounds a flush of one inode? | `efs_dcache_flush_ino` (write.c) under the caller's deadline |
| What bounds one fragment attempt? | `put_fragments_parallel_once` (write.c) |
| Who retries a fragment or a stripe? | `put_fragments_parallel_run`, `efs_client_put_fragments_parallel` (write.c) |
| How many acknowledgements end a PUT? | `put_fragments_parallel_once` (2 of 3), then `put_fragments_parallel_run` |
| What if a member is down? | `put_fragments_parallel_once` (skip), `degraded_record`, `reroute_untried_fragments` |
| Where are in-flight requests tracked? | `efs_client_put_admit`, `efs_client_put_settle` (put_ticket_owner.c) |
| What does a failure re-dirty? | `flush_pipe_drain`, `dcache_flush_keep` (write.c) |
| What does a window waiter wait for? | `flush_pipe_drain` (close), `dcache_steal_dirty_budgeted` (open) |
| What error does the application see? | close, fsync: `efs_fuse_flush_ino`, `efs_fuse_fsync_ino` (efs_fuse/write.c); read, truncate: `efs_rc_to_errno` (efs_fuse/namespace.c); write: `fuse_stat_errno` (efs_fuse/support.c) |
