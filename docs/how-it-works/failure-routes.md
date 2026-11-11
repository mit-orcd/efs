# Failure routes

What a recurring failure line proves, and what it does not. Routes were chosen mechanically: the 6 diagnostic keys most mentioned in `docs/status/k/*.md` (ties alphabetical; `efs-fuse` and `efs-mgmt` rank high because K rows name the programs, so each gets the one line an operator most needs), plus `dcache-flush-fail`, `txn-finish` and the clean-stop refusal reasons; the ranking is on the [evidence page](../status/2026-11/failure-routes-20261110.md).
Look up any other key in [the diagnostics index](diagnostics.md). A statement is made only where the function in parentheses shows it; "not determined" marks what the code read does not show.

## `dcache-flush-fail`

- Line: `dcache-flush-fail ino=%llu stage=put rc=%d` and `dcache-flush-fail ino=%llu stage=put-window rc=%d`.
- Emitted by: `efs_dcache_flush_ino` in `src/client/write.c` (component `client/write`), during the flush of one inode's dirty chunks before its REPORT (close, fsync, stop drain): `stage=put` after the PUT pass, `stage=put-window` after waiting for PUT windows.
- Establishes: with `stage=put` at least one chunk PUT of this call failed, or the dirty-set walk failed, or the RPC deadline passed between chunks (`dcache_flush_ino_pass`, `flush_pipe_drain`); `rc=` is an error recorded in this pass (the first one `flush_pipe_drain` or the walk recorded). With `stage=put-window` every PUT of this pass succeeded, and a PUT window of this inode (another flusher's) was still open after 8000 ms or the outer RPC deadline; `rc=` is always -13 (`put_win_wait`). `ino=` is the function's argument.
- Does not establish: that data is lost. A failed PUT re-dirties its entry for a later pass (comment in `efs_dcache_flush_ino`). The line names no chunk, member or object, and an inode removed on the server is dropped without this line (`dcache_retire_gone`).
- Next check: `grep -n "ino=<ino>" <client log>`. `dcache-inode-gone` means the inode was removed on the server and its bytes were dropped (`dcache_retire_gone`); `report-sync-fail ... stage=held-unit-flush` means this flush ran inside the REPORT retry loop (`report_dirty_ino_complete`); neither present: not determined from this log.
- Seen in: [K38](../status/k/K038.md), [K102](../status/k/K102.md).

## `flush-meta`

- Line: `flush-meta stage=%s ino=%llu report_rc=%d rc=%d stalled=%d`.
- Emitted by: `efs_fuse_flush_ino` in `src/client/efs_fuse/write.c` (component `client/efs_fuse.write`), during the FUSE flush of one open file (called from `ll_flush_run`, every close of a descriptor), after the flush-and-REPORT and the write-back error check.
- Establishes: the flush returns an error to the application: -ENOSPC if the final `rc` is QUOTA, otherwise -EIO (`efs_fuse_flush_ino`). `stage=report` means `report_rc` (the result of `efs_append_flush_report`) was not OK; `stage=writeback` means it was OK and `rc` was changed by `efs_file_sync_error` to an I/O error, because this description has a recorded write-back error or the inode is still stalled. `stalled=` is whether the inode was stalled before the flush.
- Does not establish: which stage of the REPORT failed or whether the data is on the members; for that the line sends the reader to `report-loop` and `report-resolve` (comment in `efs_fuse_flush_ino`). `rc` can differ from `report_rc` (`efs_file_sync_error`).
- Next check: look for `report-sync-fail ino=<ino>` and `report-resolve ... ino=<ino>` in the same log. `report-resolve outcome=unknown` means the server's answer was never learned (`efs_client_rpc_report_resolve`); neither present: with `stage=writeback` the cause is a recorded write-back error or stalled inode (`efs_file_sync_error`); which one is not determined.
- Seen in: [K17](../status/k/K017.md), [K38](../status/k/K038.md), [K39](../status/k/K039.md), [K43](../status/k/K043.md).

## `report-sync-fail`

- Line: `report-sync-fail ino=%llu stage=%s total_ms=%llu wait_ms=%llu flush_ms=%llu resolved_stale=%d rc=%d`.
- Emitted by: `report_dirty_ino_complete` in `src/client/write.c` (component `client/write`), during a synchronous REPORT (fsync, close, pressure) of one inode, after its retry loop.
- Establishes: only synchronous reports print it, and the final result `rc` was not OK (`report_dirty_ino_complete`). `stage` is the last step entered: `report` (first run), `held-unit-wait` (waiting for held units), `held-unit-flush` (flushing them), `report-retry`. The deadline for the whole call is 8 s plus the resolve budget, or the caller's earlier deadline. `ino=0` is the whole-table form (`only_ino` 0 flushes every inode).
- Does not establish: which member or record failed, or that the failing step is the one named: `stage` is the last entered, and a deadline can end the loop between steps (`past_deadline` break). `rc` AGAIN is converted before printing, so it never shows.
- Next check: find `report-resolve` and `dcache-flush-fail` for the same `ino`. `stage=held-unit-flush` with a `dcache-flush-fail` means the flush step caused it; `report-resolve outcome=partial` means a partial receipt (rc becomes STALE); none: not determined.
- Seen in: [K38](../status/k/K038.md), [K90](../status/k/K090.md), [K102](../status/k/K102.md).

## `report-resolve`

- Line: `report-resolve id=%s ino=%llu recs=%u rc=%d sent=%llx: outcome not known; same-identity lookup for %llu ms` then `report-resolve id=%s ino=%llu outcome=%s rc=%d first_rc=%d from=%u queries=%u resends=%u none=%d/%d ms=%llu%s%s`.
- Emitted by: `efs_client_rpc_report_resolve` in `src/client/inode_rpc.c` (component `client/inode_rpc`), after a REPORT failed with an error that does not say whether the server applied it (NET, BUSY, NO_QUORUM, NOT_PRIMARY, NOT_FOUND: `report_rc_transient`); called from `report_dirty_ino_run` in `src/client/write.c`.
- Establishes: the first line means a publication id exists and the client then asked the members that received it (`sent=` is a bit mask of members, bit n-1 = node n) and resent the same identity until the resolve bound. In the second line `outcome=applied` is a DONE receipt with applied outcome from member `from`; `partial` is a DONE receipt with a partial outcome (rc becomes STALE, `stuck=` names the first loser); `resent` and `answered` come from the resent REPORT itself; `unknown` ends as -EIO. `rc=` is the final result, `first_rc=` the error that started the lookup.
- Does not establish: that `none=a/b` (members holding a receipt saying nothing was proposed) proves nothing was applied: the code treats it as not proof (comment in `efs_client_rpc_report_resolve`). `ms=` includes pauses between rounds. `id=` is a receipt key string; its relation to a server log line is not determined here.
- Next check: `outcome=unknown` means the outcome was not learned within the bound and the client returns an I/O error (`efs_client_rpc_report_resolve`); whether the server applied it is not determined; for `outcome=partial` read `stuck=` and `why=` and then the K row. The members asked are those in `sent=`; the `report-split` line on a member carries `pub=`; whether it matches `id=` is not determined here.
- Seen in: [K38](../status/k/K038.md), [K40](../status/k/K040.md), [K43](../status/k/K043.md).

## `report-split`

- Line: `report-split nrec=%u pack_ms=%llu push_ms=%llu finish_ms=%llu skip=%u stale=%u inval=%u rc=%d fail=%s/%d ino=%llu pub=%llx:%llu`.
- Emitted by: `server_raft_host_report` in `src/server/raft_host.c` (component `server/raft_host`), on the member that received a REPORT of chunk records, after pack, push, finish, log sync, wait and unit publication; reached through `host_report_exec`.
- Establishes: this member processed a REPORT of `nrec` records. It prints only when `nrec` is at least 256, or some record was invalid, or pack+push+finish took at least 100 ms (`server_raft_host_report`), so it can print with `rc=0`. `fail=<phase>/<rc>` is the first failing phase (`act`, `log-hold`, `pack`, `push`, `finish`, `log-sync`, `wait`, `unit`, ...) and its rc, `-/0` if none. `ino=` is the first record's inode and `pub=` the publication id (`mint_ns` hex, `seq`).
- Does not establish: what the client saw. The log shows the first `rc`, while NOT_FOUND is turned into BUSY afterwards and a skipped stale record turns OK or BUSY into STALE (`host_report_stale_verdict`). Absence of the line proves nothing: small fast reports are silent.
- Next check: find the client's `report-resolve ... ino=<ino>` line; `outcome=applied` means the records were published despite a failed phase on this member, `outcome=partial` means a compare was lost, `unknown` means not determined.
- Seen in: [K38](../status/k/K038.md), [K43](../status/k/K043.md), [K81](../status/k/K081.md).

## `raft-host: txn-finish`

- Line: `raft-host: txn-finish coord=%u abort rc=%d (recovery)`, `... abort decided rc=%d (coordinator lost its group)`, `... no decision after %llu ms: abort decided`, `... resolve sh=%u dec=%d rc=%d (recovery)`.
- Emitted by: `host_fin_one` in `src/server/raft_host.c` (component `server/raft_host`), in the background finisher thread (`host_fin_thread`), while a distributed metadata transaction's decision is written and its per-shard RESOLVE entries are proposed after the coordinator part failed or finished.
- Establishes: `abort decided` and `no decision after N ms` mean this node's DECIDE ABORT was accepted (rc OK, or PROTO for the first form which is then read back) and `fin_aborted` was counted; the decision is abort for that `coord` (`host_fin_one`). The `(recovery)` forms mean this item was dropped (`fin_dropped`) on an error other than BUSY or NOT_PRIMARY and the leftover is left to the recovery scan.
- Does not establish: that the transaction is resolved: the first two forms precede the RESOLVE loop and a drop can still follow (a later deadline or `resolve ... (recovery)`). `coord=` is a shard number, not a node. How often the line may repeat is not determined here.
- Next check: for one `coord=`, look for a `resolve ... (recovery)` line: present means the RESOLVE was dropped and left to recovery; absent means the loop either finished (then it retires the decision) or was cut by shutdown or the age limit without a line (`host_fin_one`). Why a line repeats for one `coord` is not determined here.
- Seen in: [K42](../status/k/K042.md), [K112](../status/k/K112.md).

## `reason=` values `uncertified-busy`, `append-open-by-other-session`, `append-reap-owed` of a refused clean stop

- Line: `session cleanup incomplete: epoch=%u shard=%u rc=%d reason=%s; resume cleanup-leases and cleanup-appends for that epoch/shard` (operator tool) and `efs: session revoked at unmount: epoch=%u touched=%u rc=%d ms=%llu fence_ms=%llu cleanup_ms=%llu cleaned=%u probe_failed=%u%s%s%s` with ` phase=%s` and ` shard=%u reason=%s` filling the last three (client at unmount).
- Emitted by: `cmd_raft_session_fence` in `src/mgmt/efs_mgmt.c` (component `mgmt/efs_mgmt`), and the client's `efs_client_session_end` in `src/client/inode_rpc.c` (component `client/inode_rpc`), during the cleanup phase of a session revoke; the text of `reason` comes from `efs_session_cleanup_reason` in `include/efs/session_revoke.h`.
- Establishes: the revoke stopped in its cleanup phase on `shard` with BUSY, and `reason` is the salt of the last certified BUSY reply or of the reply that failed the stage (`efs_session_revoke`). `append-open-by-other-session` is salt 4: another session had an OPEN append (`efs_session_cleanup_reason`, `EFS_MD_SESSION_APPEND_WAIT_OPEN`). `uncertified-busy` is any other BUSY salt: it carries no server reason.
- Does not establish: for `uncertified-busy`, that nothing was submitted: only a certified BUSY with zero index and term is a no-op (`efs_session_cleanup_retry_ok`). `append-reap-owed` (salt 7) is mapped in the header, but this build's server source has no site that sets it (searched `src`); the header comment of `EFS_META_APPEND_WAIT_GONE` in `include/efs/meta_apply.h` says nothing produces it, so it is not expected in a log from this tree.
- Next check: `reason=append-open-by-other-session`: find which session holds an OPEN append on that shard's inode (not determined by this line). `uncertified-busy`: look in the member's log at the same time for `raft-host: txn-finish` lines; this is the pairing K112 reports, not something this page proves.
- Seen in: [K42](../status/k/K042.md), [K112](../status/k/K112.md).

## `efs-fuse`

- Line: `efs-fuse: UNMOUNT DATA LOSS: flush still failing (data rc=%d, metadata rc=%d) after %lds; writes that were never reported are gone` (the prefix `efs-fuse:` is shared by many other lines; this one is the unmount drain failure).
- Emitted by: `efs_fuse_destroy` in `src/client/efs_fuse/main.c` (component `client/efs_fuse.main`), the FUSE destroy callback, during unmount after `efs_wb_sync`, while draining the dirty set for up to 60 s.
- Establishes: the drain loop gave up with `data rc` (from `efs_dcache_drain_pending`) or `metadata rc` (from `efs_client_note_meta_change`) not OK, neither being QUOTA, and the unmount is recorded unclean. The comment in `efs_fuse_destroy` says writes not reported by this point are gone once shutdown frees the table.
- Does not establish: which files or how many bytes: the line names none. A QUOTA drain prints a different line (`ENOSPC` with `force-discard` lines naming the chunks), and a fenced session prints `mount session fenced`.
- Next check: `grep -n "force-discard" <client log>`: lines name the chunks dropped (`dcache_log_discard`); with none, look for `dcache-flush-fail` and `report-sync-fail` earlier in the log for the failing stage.
- Seen in: [K30](../status/k/K030.md), [K38](../status/k/K038.md), [K39](../status/k/K039.md), [K42](../status/k/K042.md), [K56](../status/k/K056.md), [K90](../status/k/K090.md).

## `efs-mgmt`

- Line: `efs-mgmt: session setup failed: %s` (the prefix `efs-mgmt:` is shared by other lines; this is the session setup failure of namespace commands).
- Emitted by: `mgmt_identify` in `src/mgmt/efs_mgmt.c` (component `mgmt/efs_mgmt`), before sending a namespace or SETATTR request from `cmd_raft_create`, `cmd_raft_unlink`, `cmd_raft_rmdir`, `cmd_raft_link`, `cmd_raft_setattr` or `cmd_raft_rename`.
- Establishes: the tool could not obtain a session or admit it to the shard, so the command was not sent (each caller returns 1 when `mgmt_identify` fails). `%s` is `efs_strerror` of the rc from `mgmt_session_get` (bad `EFS_MGMT_SESSION`, failed random read, or the CREATE) or `mgmt_session_admit` (REGISTER/ESTABLISH).
- Does not establish: which of those steps failed (the message gives only the code text), or whether the cluster is unhealthy: a malformed `EFS_MGMT_SESSION` fails the same way and prints its own line first.
- Next check: look for `EFS_MGMT_SESSION: ...` just before it (bad input); if absent, repeat the command with `EFS_MGMT_SESSION` unset: a second identical failure means the error is not random-read or input related, and the code text maps to [the error codes](diagnostics.md); which step failed is not determined.
- Seen in: [K5](../status/k/K005.md), [K6](../status/k/K006.md), [K22](../status/k/K022.md), [K35](../status/k/K035.md), [K37](../status/k/K037.md), [K95](../status/k/K095.md).
