# K97: `rpc_resolve_ms` lazy init race (Task DR)

Verdict: **PASS** (K97 FIXED, blocks alpha: no). Linked item: K97 in
[the checklist](../alpha-1.0-2plus1-checklist.md); found by Task DQ while trying
the K96 live test under TSan ([K96](k96-status-proof-slots-20261106.md), Checks).

## Race and fix

`rpc_resolve_ms()` (`src/client/inode_rpc.c`, W155) cached `EFS_RESOLVE_MS` in a
function-static `long` (-1 = unset) that any RPC thread could set first. Under
`tests/test_k96_status_slots_live.py` the 24 `efs-bench` writers create their
files at once, and TSan reported the read (`:142`) in one worker against the write
(`:145`) in another, via `rpc_resolve_identified` <- `rpc_send_recv_dual` <-
`efs_client_rpc_create`. Fix (`c55723ce0`): the K83/W143 idiom, an acquire-load of
the static, compute on -1, a release-store, then use the local copy. Two first
threads may both call `efs_setting_u64`, which is idempotent under its own mutex.
No timeout, budget or default changed (8000 ms).

The other lazy env statics in `inode_rpc.c` (`st_on_s`, every
`env_opid_verbose_s`/`env_opid_trace_s`, `efs_rpc_prof_on`) already use the
idiom; the fault hooks (`create retry`, `append_reply_fault`) keep their state
under a mutex.

`k96_status_slots_live|K96_ROUNDS=1 tests/test_k96_status_slots_live.py` now joins
`default_list_tsan` in `tests/sanitizer_check.sh` next to `k88_chunk_hints_live`.

## Tested

- Source: branch `claude/exciting-tu-27838c`, tested `c55723ce0` (on Task DQ's
  `a537e769d`, not merged into devel). Before: the same tree with the
  `inode_rpc.c` hunk reversed. Uploaded with `git archive`, so the build id reads
  `unknown`.
- Host node9900 (192 CPUs, gcc 11.5.0, libtsan), private libfuse 3.16.2
  (`EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `PKG_CONFIG_PATH` and
  `LD_LIBRARY_PATH` to its `lib64`), TCP loopback, buffered I/O, harness-chosen
  ports (none in 17432-17435), `TMPDIR=/tmp/efs-k96tsan/tmp`. Three production
  `efsd`, 24 library writers, one round.
- Commands (from the tree root):
  - `EFS_SAN_ONLY="k96_status_slots_live k88_chunk_hints_live" bash tests/sanitizer_check.sh tsan`
  - `EFS_SAN_ONLY=k96_status_slots_live bash tests/sanitizer_check.sh tsan` on the before tree
  - `make -j32 test`
  - Cleanup: remove the tree (`/tmp/efs-k96tsan`); the gate deletes its log dir on PASS.

## Results

| Tree | Entry | Verdict | TSan reports |
| --- | --- | --- | --- |
| before (hunk reversed) | `k96_status_slots_live` | FAIL (12 s) | 8: 6 `rpc_resolve_ms` (`inode_rpc.c:142`), 2 `host_export_salt` (`src/server/raft_host.c:7486/7487`) |
| after `c55723ce0` | `k96_status_slots_live` | PASS (12 s) | 0 |
| after `c55723ce0` | `k88_chunk_hints_live` | PASS (7 s) | 0 |

`VERDICT: PASS (2 run, 0 failed, 19s)`. Fast suite `make test` on the fixed tree:
rc 0 (the FAIL lines in its output are expected negative controls).

## Limitations and follow-ups

- One TSan run each side. Task DQ's trial run of the same entry (2 reports, both
  this race) is the earlier before-data.
- Not this row: the before run also reported a server race in `host_export_salt`
  (`raft_host.c:7486/7487`), absent from the after run. It is intermittent and
  can fail the new TSan entry until traced; no K row yet.
- Same unsynchronised lazy shape outside this file, not reported by TSan here:
  `src/client/write.c` `lim` (about line 9089, hot write path, commented as a
  benign race) and the fault-build `ms` (about line 3744).
- Raw output: node9900 `~/efs-artifacts/taskK97-20261106/node9900-tsan.tar.gz`
  (gate outputs, the before run's TSan reports, `make test` log), sha256
  `23f25de6d9311b6eca8611b72b0ee9959e196008ccf750ecee6bcb5262bea10f`.
