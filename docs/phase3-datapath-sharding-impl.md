# Phase 3 data-path sharding (bits>0) — implementation guide

Audience: an implementing agent. This is a staged, validate-as-you-go plan.
Do NOT skip the validation gates. Each stage must leave the cluster correct
(mc_stress VERIFY-OK, posix 144/144) even if the perf win only shows at the
last stage.

Read first, in order:
1. `docs/scaling-roadmap.md` — the "Performance next" section (why this exists).
2. `docs/phase3-sharding.md` — the metadata-sharding design this builds on.
3. This file end-to-end before writing any code.

---

## 1. The problem we are solving

On a `bits=0` export (today's `efs-test`), the multi-client sequential-write
ceiling is ~9.3 GB/s aggregate (9 clients). The cause:

- Every client sends ALL its dirty chunk mappings in one `REPORT_CHUNKS` RPC
  to the metadata primary (`src/client/write.c:795` →
  `efs_client_rpc_report_dirty` → `rpc_send_recv_owner(EFS_ROOT_INO, …)`,
  `src/client/inode_rpc.c:428`).
- The primary applies every record under the single global `g_server->lock`
  (`src/server/handler.c:1655` `EFS_MSG_REPORT_CHUNKS`), and the primary alone
  runs the metadata flush (`src/server/meta_server.c:1031`
  `server_flush_fragmented_meta`, `if (primary)` at line 1078).

So one server's lock + one flush thread bound the whole cluster's write
throughput, no matter how many servers we add.

The fix (this Phase): route each file's chunk/size records to the server that
owns the file's shard, so the apply + flush work spreads across all 4 servers.

---

## 2. CRITICAL current-state finding (verified empirically, Aug 23)

**`bits>0` today does NOT distribute write metadata across servers.** Two
reasons:

1. **Reports route only to the primary.** `efs_client_rpc_report_dirty` sends
   one report to `EFS_ROOT_INO`'s owner (the primary). The primary's REPORT
   handler applies only records for shards it owns and **silently drops the
   rest** (`src/server/handler.c:1730-1735` and `1747-1752`: `continue`).

2. **Creates concentrate ownership on the parent's owner.** `create_sharded`
   picks the target shard as
   `target = (psh + create_rr++ * create_stride) % shard_count`
   (`src/common/metadata.c:1690-1691`), and the server sets
   `create_stride = nlive` (`src/server/handler.c:1497`). With 4 live nodes and
   `shard_count = 8`, `(psh + rr*4) % 8` only ever yields `{psh, psh+4}` — both
   owned by the SAME node (`efs_shard_owner_of` = `sorted_live[shard % nlive]`,
   `src/common/metadata.c:3518`, and `shard` vs `shard+4` differ by a multiple
   of `nlive`). Since root is pinned to shard 0, **every file under root lands
   in shards {0,4}, both owned by the primary.**

   Verified on a live cluster: created 8 files on a `bits=3` export, all inos
   were multiples of 4 (shards {0,4} only). So the Aug-21/23 "sharded
   validation" exercised the per-shard *machinery* (extra tables, extras
   commit, failover) but never actually placed data-path ownership on a
   non-primary server.

**Consequence:** you cannot see a perf signal until BOTH are fixed. Fixing only
the report routing (leaving creates concentrated) still sends everything to the
primary. Fixing only the create spread (leaving reports primary-only) makes the
primary drop records for shards it doesn't own → **silent data loss**. They
must land together, gated by the Stage-0 test that proves the spread.

---

## 3. Goal architecture

- **Inos spread across all shards.** A create in a directory round-robins the
  child inode across ALL `shard_count` shards (not just the parent's owner's
  shards), so different files land on different owners.
- **Cross-server create.** Because the create RPC still goes to the parent's
  owner (keeps the dentry authoritative + single decision point), but the child
  inode row must live on the *target shard's* owner, the parent owner fans out
  a nested create to the target owner.
- **Per-shard REPORT.** The client groups its dirty records by shard and sends
  one report per non-empty shard to that shard's owner.
- **Per-shard fsync.** `fsync` sends `sync=1` to each shard that has dirty
  data; each owner commits its own shard before replying.
- **Reads already fan out** (mostly): `lookup` routes by parent
  (`inode_rpc.c:127`), `getattr`/`setattr`/`append`/`getchunks` route by ino
  (`inode_rpc.c:173,342,364,245`). The one gap: after cross-server create, the
  parent owner holds only the dentry, so `lookup` returns a stub and the client
  must `getattr` the child owner for the full row (Stage 2c).
- **Failover/rebuild per shard** already exists (extras commit, descriptor
  max-merge, `efs_export_root_maxmerge_extras`). Do not regress it.

---

## 4. Stage 0 — test scaffold (build this FIRST; it is your compass)

Write `tests/stress/shard_spread_probe.sh` that:

1. On a FRESH cluster (see "Deploy + fresh cluster" below), create a throwaway
   export and shard it:
   `./efs-mgmt mkfs 172.16.223.57:19810 efs-s3`
   `./efs-mgmt upgrade 172.16.223.57:19810 efs-s3 3`   # shard_bits=3 → 8 shards
2. Mount it on two clients (007 and 008) at `/tmp/efs/mnt-s3`.
3. On 007, create N=32 empty files, then for each `stat -c %i` and compute
   `shard = ino & 7`. Count DISTINCT shards.
4. Write 512 KiB of random data to each file from 007, `sync`.
5. On 008 (cold), `md5sum` every file and compare to 007's values.

**Pass criteria:**
- distinct shard count == 8 (all shards used), AND
- at least one file is in a shard whose owner != primary, AND
- all 32 files verify on 008.

**Today this FAILS** the distinct-shard assertion (you will see only 2 shards).
That failure is the point: it proves the concentration. After Stage 2 it must
pass. Keep this script; run it after every stage.

### Deploy + fresh cluster (standing permission to wipe)
Use the `efs-test-ssh` wrapper: `~/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh <host> '<cmd>'`.
The cluster is disposable. To reset: tear down clients/servers, wipe with
`edelete` (NEVER `rm -rf`), re-`mkfs`. See the `efs-fcstor-deploy` rule for the
exact restart commands. **Always measure perf on a fresh mkfs** — accumulated
chunk-table state silently degrades throughput 15-40% and makes A/B meaningless.

---

## 5. Stage 1 — per-shard REPORT routing + a loud NOT_OWNER (no perf change yet)

Goal: build the routing machinery and make mis-routed records LOUD instead of
silently dropped. With creates still concentrated, all reports still land on
the primary, so this stage is behavior-preserving on perf but must stay
correct.

### 5a. Server: stop silently dropping non-owned records
In `src/server/handler.c` `EFS_MSG_REPORT_CHUNKS` (case at line 1655):
- Replace both silent `continue` drops (lines 1730-1735 for chunk recs,
  1747-1752 for ino recs) with: count the drops, and if any record was dropped
  because this server is not the shard owner, set the reply status to
  `EFS_ERR_NOT_PRIMARY` (reuse it — the client already retries it by
  re-resolving the owner) and reply immediately.
- Keep applying the records this server DOES own before replying (partial
  apply is fine; the retry re-sends the full set and the apply is idempotent —
  `efs_export_set_chunk` overwrites, size is grow-only, mtime is newer-only).

Why: today a mis-routed record vanishes with `status = OK`. Under a membership
flap the client and server can disagree on ownership; silent drop = data loss.
A loud error makes the client re-resolve and retry. This is a correctness
hardening that must exist BEFORE reports route per-shard.

### 5b. Client: group the dirty report by shard
In `src/client/write.c` `efs_client_report_dirty` (line 727), after building
`crecs[cn]` and `irecs[in]`:
- Read `bits = g_client.export.root.shard_bits` and
  `sc = g_client.export.root.shard_count`.
- If `bits == 0 || sc <= 1`: keep the single existing call (unchanged path).
- Else: partition record indices by `efs_export_shard_of(rec.ino, bits)`
  (`src/common/metadata.c:3477`). For each non-empty shard `s`, gather that
  shard's chunk recs and ino recs and send one report routed by any ino in the
  group (they all share shard `s`).

### 5c. Client: route the report by a chosen ino
`efs_client_rpc_report_dirty` (`src/client/inode_rpc.c:401`) currently
hard-codes `rpc_send_recv_owner(EFS_ROOT_INO, …)`. Add a `route_ino` parameter
and pass it to `rpc_send_recv_owner(route_ino, …)`. The single-shard caller
passes `EFS_ROOT_INO`; the per-shard path passes a member ino.
`rpc_send_recv_owner` (`inode_rpc.c:71`) already retries `NOT_PRIMARY` by
dropping the conn and re-resolving — that is exactly the 5a error.

### 5d. Error handling: conservative merge-back
If ANY per-shard report fails after the existing 4-attempt retry, merge the
WHOLE snapshot back (`dirty_snap_merge_back_locked`, as the current code does
on failure at write.c:808-812). Re-applying already-sent records is safe
(idempotent: grow-only size, newer-only mtime, `set_chunk` overwrite). Do not
try to merge back only the failed shard — keep it simple and correct.

### Stage 1 gate
Fresh cluster, `bits=0` export: posix 144/144, posix2 12/12, perf quick
(single sw-1m ≈ 5600, multi-9 sw-1m agg ≈ 9300). Then `bits=3` export: the
Stage-0 probe still shows concentration (expected) but mc_stress VERIFY-OK on
both clients and no silent-drop log lines. Perf will NOT improve yet — that is
expected. Commit.

---

## 6. Stage 2 — spread creates + cross-server create + read fan-out (the win)

This is the bulk of the work. Take it in the three sub-steps below and re-run
the Stage-0 probe after each.

### 6a. Spread the create target across all shards
In `create_sharded` (`src/common/metadata.c:1690-1691`), change the target for
non-dirs from `(psh + create_rr++ * create_stride) % sc` to `create_rr++ % sc`
(round-robin over ALL shards). Keep dirs on the parent's shard
(`target = psh`) so a directory's dentries stay local to it. Remove the now
-unused `create_stride` assignment at `handler.c:1497` (or leave it; it becomes
unused).

After this, creates will compute target shards the local server may NOT own.
The next two sub-steps make that correct.

### 6b. Cross-server create (server-side fan-out)
The create RPC still arrives at the parent's owner. Today `create_sharded`
writes the child row into a LOCAL shard table even when that shard is owned by
a peer — that row would never be flushed (the flush skips non-owned shards,
`meta_server.c:1058-1067`) and would be fenced on remap. Change it so the child
row is created on the shard's OWNER:

- In the `EFS_MSG_INODE_CREATE` handler (`handler.c:1493`), before calling
  `efs_export_create`, determine the target shard (factor the target
  computation out of `create_sharded` into a helper, e.g.
  `efs_export_create_target(ex, parent, mode)` returning the shard).
- If the target shard is owned locally (or `sc <= 1`): call
  `efs_export_create` exactly as today.
- If remote: the parent owner must
  1. Send a nested create to the target owner carrying
     `(parent, name, mode, uid, gid, target_shard)`. Add a msg type (e.g.
     `EFS_MSG_INODE_CREATE_SHARD`) or a flag on the existing create msg. The
     target owner allocates an ino from the target shard's congruence class
     (`efs_export_alloc_ino(ctab, target)`), creates the full row in its own
     `ctab`, marks `ctab->shard_dirty = 1`, and replies with the full row.
  2. On success, the parent owner writes ONLY the dentry into its local parent
     shard table (`efs_export_create_with_ino(ptab, ino, parent, …)`, the same
     call `create_sharded` already uses for the parent copy at
     `metadata.c:1726`), marks `ptab->shard_dirty = 1`, and relays the full row
     to the client.
  3. Ordering: create the child row FIRST, then the dentry. If the parent
     owner crashes between, the child has an unreachable orphan row (harmless,
     GC-able). Never write the dentry first.
  4. If the target owner is down/unreachable: fail the create with a retryable
     error (`EFS_ERR_NO_QUORUM`/`EFS_ERR_NET`); the client retries after
     failover. Do NOT fall back to writing the row into a non-owned local
     shard table.
- The server already has peer connections for PUT_META/catchup — reuse that
  machinery for the nested call. Keep the nested call synchronous and bounded
  (a create adds one RTT; creates are not the write-path bottleneck).

Client dual-apply is unchanged: the client's local table holds all shards, so
its local `create_sharded` still works regardless of server ownership.

### 6c. Read fan-out for the full row
After 6b, the parent owner holds only the dentry for a remotely-sharded file,
so its `lookup` returns a dentry-stub (size 0, no chunks). The client must
fetch the full row from the child owner. In `efs_client_lookup`
(`src/client/ops.c:304`): after `efs_client_rpc_lookup(parent, part, &child)`
returns the dentry, if the export is sharded and `child` is a regular file,
issue `efs_client_rpc_getattr(child.ino, …)` (routes to the child owner,
`inode_rpc.c:173`) and use THAT row for `adopt_rpc_inode` + `pull_file_layout`
(both need the real size to pull chunk mappings). Keep the existing
"prefer the local row" merge (ops.c:341-349) so a writer's newer local size
wins.

Verify the server `GETATTR` handler returns the full row from the child owner's
shard table (it routes by ino via `table_for_ino`, `handler.c:39`). If the
child owner rebuilt its shard table from pages, the full row is there because
the owner flushed it (6b marked it dirty → owner flushes its own shard).

### Stage 2 gate
Fresh cluster, `bits=3`: the Stage-0 probe now PASSES (8 distinct shards, at
least one non-primary-owned, all files verify cross-client). mc_stress
VERIFY-OK on both clients, including kill -9 of a NON-primary shard owner →
failover reads OK, restart rejoin OK, cold mount OK. Then `bits=0` regression:
posix 144/144, posix2 12/12. Then perf on a FRESH `bits=3` cluster: multi-9
sw-1m agg should exceed the ~9.3 GB/s bits=0 ceiling. Commit.

---

## 7. Stage 3 — per-shard fsync

`fsync` currently calls `efs_client_report_dirty(1)` (one sync report to the
primary). With per-shard reports (Stage 1), `sync=1` must reach every shard
that has dirty data so each owner commits its own shard before replying.

In `efs_client_report_dirty`, when `sync=1` and sharded: send `sync=1` to EVERY
non-empty shard group (not just one). Return OK only if all succeed. This is
per-file correct (the fsynced file's shard is non-empty because its chunks were
just marked dirty by the flush path) and over-commits other dirty files in
those shards exactly as today's whole-export flush over-commits. The periodic
`meta_flush_thread` still flushes all dirty shards regardless.

Check the server side: a `sync=1` report on a non-primary owner must flush +
commit THAT shard (the `do_flush` path in the REPORT handler calls
`server_flush_fragmented_meta`, which for a non-primary flushes its owned extra
shards and commits extras, `meta_server.c:1053-1084`). Verify this path works
for an owner that is not the primary — that is the durability barrier for
sharded fsync.

### Stage 3 gate
`bits=3`: fsync-heavy workload (e.g. create + fdatasync loop, and the
mc_stress appfile which uses O_APPEND + fsync semantics) survives kill -9 of a
shard owner mid-flush and remounts clean (no torn data). posix fsync tests
pass on both `bits=0` and `bits=3`. Commit.

---

## 8. Stage 4 — full validation + perf acceptance

On a FRESH cluster:
1. `bits=0` (`efs-test`): posix 144/144, posix2 12/12.
2. `bits=3` (`efs-s3`): Stage-0 probe passes; mc_stress VERIFY-OK both clients;
   posix suite 0 EFS bugs; posix2 12/12; kill -9 shard owner failover OK.
3. Perf (fresh mkfs, both bit settings):
   - `bits=0` baseline: single sw-1m ≈ 5600, multi-9 sw-1m agg ≈ 9300.
   - `bits=3` target: **multi-9 sw-1m agg ≥ 3× single-client (~17 GB/s)** and
     99th-percentile 1 MiB write latency < 20 ms (the roadmap milestone).

If `bits=3` multi-write does not beat `bits=0`, profile the shard owners with
`perf record` — the residual is likely the per-shard flush serialization or a
re-centralized lock. Do not declare victory on a grown table; always fresh mkfs.

---

## 9. Traps & footguns (read before each stage)

- **Silent drop is the enemy.** Any place that skips a record for a non-owned
  shard MUST be loud (NOT_PRIMARY) so the client re-resolves. Grep for
  `continue` next to `efs_shard_owner_of` after your changes.
- **Ownership is recomputed from the live list on every op.** During a
  heartbeat flap a server may transiently think it owns more/fewer shards.
  The loud-error + client retry (Stage 1) is what makes this safe. Never cache
  an ownership decision across retries.
- **`table_for_ino` (`handler.c:39`) routes by `ino & (shard_count-1)`.** Root
  is pinned to shard 0. Do not special-case root away.
- **The append barrier (`append_rsv`, `handler.c:1610-1627`) lives on the shard
  table and is released only when that table's size grows via a REPORT.** So
  size irecs MUST reach the shard owner (they do, once reports route per-shard
  — the irec's ino is in the same shard as its chunks). If appends stall with
  `BUSY`, check that irecs are landing on the owner.
- **fsync durability = the owner's commit, not the primary's.** A sync report
  to the wrong server is a durability bug, not just a perf bug.
- **Fresh mkfs for every perf measurement.** Grown tables degrade throughput
  and will make you chase phantom regressions.
- **Build-ID gate:** after a new commit, restart ALL servers together (mixed
  build IDs are rejected). Use `edelete`, never `rm -rf`, to wipe.
- **Do not break `bits=0`.** Every sharded code path must be gated on
  `bits && sc > 1` so the `bits=0` fast path is byte-for-byte unchanged. The
  `bits=0` regression suite (posix 144/144) is the guardrail.

---

## 10. Suggested commit boundaries

1. Stage 0 probe script (test-only, no behavior change).
2. Stage 1 (loud NOT_OWNER + per-shard report routing) — behavior-preserving.
3. Stage 2 (spread creates + cross-server create + read fan-out) — the win.
4. Stage 3 (per-shard fsync).
5. Stage 4 (validation results recorded in the project-state rule).

Each commit must leave `bits=0` correct (posix 144/144). If a stage cannot be
made correct, revert it and keep the prior stages — they are independently
safe.
