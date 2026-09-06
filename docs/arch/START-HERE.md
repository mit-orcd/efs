# Start here — what to work on, and what to read first

[Architecture](../architecture.md) · [Roadmap](../scaling-roadmap.md) ·
[Development](development.md) · [Verification](verification.md)

This page exists because of the bar in [development.md](development.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---

## 1. The task right now

> **Architecture migration §10, step 10.5c:** applied SM is gated in-sim
> (10.5c-1..8), the production Raft host is gated in `efsd` (10.5c-9),
> and LOOKUP/GETATTR go through ReadIndex + KV behind `EFS_MD_RAFT`
> (10.5c-10), file CREATE is a single Raft entry on the dentry
> shard (10.5c-11), MKDIR is a 2-shard txn (10.5c-12), last-link
> file UNLINK is one Raft entry (10.5c-13), mode/owner SETATTR is
> one Raft entry (10.5c-14), empty LOCAL RMDIR is a 2-shard txn
> (10.5c-15), LINK is a 2-shard txn (10.5c-16), nlink>1 UNLINK
> is a 2-shard txn (10.5c-17), utimens is the inode fence
> (10.5c-18), same-dir LOCAL file RENAME is a txn
> (10.5c-19), READDIR/LOOKUP_PATH are ReadIndex + KV
> (10.5c-20), and SETATTR SIZE (chunk-aligned truncate, no tail)
> is one Raft entry (10.5c-21). Next: cross-group propose
> (a node that is not leader of a participant group) and the rest of
> the mutations behind the same flag.
> [architecture.md §10](../architecture.md)
>
> **10.5c-1 is in (gated):** the first single-shard op batch over the
> applied KV — READDIR (LOCAL / SPLITTING / HASHED, including lane-0
> aliasing), SETATTR mode/owner (ctime not mtime, MAX-clamped, stale-handle
> reject), GETATTR as the validated double collect (file write lanes **and**
> HASHED-dir `used_shards` dir lanes; pending committed reductions; epoch /
> `mtime_gen` / `lane_seq` recheck; I9 on an unreachable coordinator),
> LOOKUP_PATH as a batched ancestor walk (resume from the terminal, I9,
> intermediate not-a-directory). The simulator proposes SETATTR through
> Raft, serves GETATTR/READDIR via ReadIndex, and walks LOOKUP_PATH
> hop-by-hop with a ReadIndex per shard; a leader crash mid-read does not
> change the answer. Gate: `test_meta_apply`, `test_sim`
> (`test_single_shard_ops`, `test_hashed_dir_stat`).
>
> **10.5c-2 is in (gated):** the `utimens` inode fence. Only utimens can
> set a time backwards, so it is the only op that bumps `mtime_gen` and
> pushes that generation onto every active write lane (file) or used dir
> lane (HASHED/SPLITTING directory). getattr then ignores older lane
> mtimes; a later write re-stamps at the new generation. atime-only does
> not fence. chmod after utimens does not hide the explicit mtime. The
> simulator proposes the fence through Raft; a leader crash does not
> resurrect a stale lane mtime. Gate: `test_meta_apply`
> (`test_utimens_fence`, `test_stat_fence_and_gen`, `test_stat_dir_hashed`),
> `test_sim` (`test_utimens_fence`, `test_hashed_dir_stat`).
>
> **10.5c-3 is in (gated):** truncate range-delete. SETATTR(size)
> is one apply batch: `content_epoch++`, `base_size = S`, per-active-lane
> epoch fence, prefix scan of each lane's chunk-map with DEL of keys at or
> past S, optional CAS of the straddling tail chunk in the same batch.
> A later publish at the old epoch is STALE (I22); a sub-chunk RMW cannot
> resurrect deleted keys. The simulator proposes truncate through Raft;
> crash/restart keeps size 0 and the deleted chunks gone. Gate:
> `test_meta_apply` (`test_truncate_range_del`), `test_sim`
> (`test_truncate_range_del`).
>
> **10.5c-4 is in (gated):** O_APPEND in-sim. Reserve against
> the real EOF (MAX of `base_size`, active-lane ends, pending reductions,
> watermark) — a private counter is a correctness bug. While unresolved,
> an append barrier rejects publish with `new_size > watermark`; getattr
> reports the contiguous resolved frontier, not lane `max_end`. Resolve is
> COMPLETED / ABORTED_HOLE / FENCED_HOLE; a retried reserve recovers the
> same offset (I16, `opid.extra`). The simulator proposes reserve/resolve
> through Raft; crash/restart keeps the reservation. Gate:
> `test_meta_apply` (`test_append_reserve`), `test_sim`
> (`test_append_reserve`).
>
> **10.5c-5 is in (gated):** in-sim LINK, UNLINK nlink>1, RMDIR, and
> file RENAME. LINK is dest dentry + inode `nlink++`. UNLINK last-link is
> one shard iff `dentry_shard == inode_shard`, else a 2-shard txn (the
> hardlink corner). RMDIR is a txn; LOCAL emptiness is one range check,
> HASHED emptiness is `dentry_seq` guards on used lanes (≤64; never raise
> `EFS_TXN_MAX_PART`). File RENAME is same-dir LOCAL one shard / cross-dir
> a txn; replace is not in this slice. Directory rename is INVAL until
> `parent_version` is on the inode row. Dest uniqueness CASes the current
> txn version (a reused name is not ver 0). Gate: `test_meta_apply`
> (`test_link_nlink`, `test_rmdir_rename`), `test_sim` (`test_link_i17`,
> `test_rmdir_rename`) — mem and `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-6 is in (gated):** directory rename with `parent_version`. The
> inode row is 128 B and carries `parent` + `parent_version` (bumped on
> reparent). Ancestry of `dst_parent` is a read-set of **pver sidecar**
> guards (`EFS_KV_KIND_PVER`), not exclusive on ancestor inodes — chmod /
> mkdir in an ancestor does not abort. src in the chain is INVAL (cannot
> move a dir under itself). Concurrent `rename(a→b/a)` ‖ `rename(b→a/b)`:
> the second BUSYs on the first's exclusive pver PUT (I17, no half-apply).
> Cross-dir directory rename adjusts parent nlink; file rename does not.
> `EFS_TXN_MAX_PART` stays 8 (deep ancestry that would exceed it is BUSY).
> Replace is still not in this slice. Gate: `test_sim` (`test_dir_rename`,
> `test_rmdir_rename`) — mem and `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-7 is in (gated):** the in-sim export is created by a Raft mkfs,
> not a local KV seed. Boot elects, proposes `efs_meta_apply_init` with a
> leader-stamped `now`, and apply is idempotent. getattr of ROOT is
> ReadIndex. 100% message drop cannot create the export (there is no
> locally-seeded ROOT to hide behind). Crash/restart keeps ROOT and files
> written after mkfs. Gate: `test_sim` (`test_export_mkfs`) — mem and
> `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-8 is in (gated):** per-export `salt` is chosen at mkfs and
> stored on the ROOT shard (`EFS_KV_KIND_EXPORT`). MKDIR scatter is
> `hash(parent, name, salt) & 0xFFF`. Idempotent mkfs does not rewrite
> salt. Crash/restart keeps it. Gate: `test_meta_apply`
> (`test_export_salt`), `test_sim` (`test_export_salt`) — mem and
> `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> Both durable backends are in (10.5a KV, 10.5b Raft log). The export
> question is **decided** (Sep 4, architecture.md §1): one export per
> cluster, hardcoded name `efs`; no create-export operation; multi-export,
> if ever, is one engine per export, never `export_id` in keys.
>
> **10.5c-9 is in (gated):** production Raft host in `efsd`, env-gated
> `EFS_MD_RAFT=1` (inert when off). Two groups (odd/even shard parity, same
> mapping as the sim), `raft_disk` + one `kv_lsm` per node, Raft messages
> on peer TCP (`EFS_MSG_RAFT`), a tick thread, idempotent `raft-mkfs`.
> Gate: `test_wire` (codec) + `tests/stress/raft_host_smoke.sh` on a
> scratch 4-node cluster (port 19820, `/tmp` storage — live cluster
> untouched): elect, mkfs ROOT on every voter, kill -9 follower then
> leader, restart catch-up keeps ROOT.
>
> **10.5c-10 is in (gated):** LOOKUP and GETATTR on the production host
> are leader + ReadIndex + applied KV when `EFS_MD_RAFT=1` (the in-memory
> table path is unchanged when the flag is off). Follower replies
> NOT_PRIMARY; a missing name is NOT_FOUND. Same scratch smoke, plus
> getattr of ROOT (mode 040755, nlink=2) and a miss lookup, including
> after crash catch-up.
>
> **10.5c-11 is in (gated):** file CREATE on the production host is one
> Raft entry on the dentry shard (`efs_meta_apply_create_file`; LOCAL
> parent = co-located with the parent). Duplicate name is EXIST; a
> directory mode is INVAL (MKDIR is a 2-shard txn, not this helper).
> Same scratch smoke, plus create/lookup/getattr, crash catch-up keeps
> the name.
>
> **10.5c-12 is in (gated):** MKDIR is the in-sim 2-shard txn
> (PREPARE/DECIDE/RESOLVE, same command bytes) on the production host.
> Directory mode on `EFS_MSG_INODE_CREATE` takes that path. The receiving
> node must lead every participant group (`NOT_PRIMARY` otherwise). Same
> scratch smoke: mkdir under ROOT (retry names), lookup, ROOT nlink=3,
> crash catch-up keeps the directory.
>
> **10.5c-13 is in (gated):** last-link file UNLINK on the production
> host is one Raft entry on the dentry shard (`efs_meta_apply_unlink`).
> Missing name is NOT_FOUND; a directory is INVAL (RMDIR is a txn);
> nlink>1 is INVAL (2-shard). `UNLINK_SHARD` is INVAL (old fan-out).
> Same scratch smoke: create+unlink a dedicated name, lookup miss,
> crash catch-up keeps the name gone.
>
> **10.5c-14 is in (gated):** mode/owner SETATTR on the production
> host is one Raft entry on the inode shard (`efs_meta_apply_setattr`).
> Missing ino is NOT_FOUND without proposing; SIZE/MTIME/ATIME are
> INVAL (truncate / utimens later). Same scratch smoke: chmod 0600 on
> the created file, getattr confirms, crash catch-up keeps
> `mode=0100600`.
>
> **10.5c-15 is in (gated):** empty LOCAL RMDIR is the same 2-shard
> txn as MKDIR (PREPARE/DECIDE/RESOLVE) on the production host.
> HASHED/SPLITTING are INVAL/BUSY (dseq-on-used-lanes later). The
> receiving node must lead every participant group. Same scratch
> smoke: mkdir a dedicated name, rmdir, lookup miss, rmdir of a file
> is INVAL, crash catch-up keeps the name gone.
>
> **10.5c-16 is in (gated):** LINK is dest dentry + inode `nlink++` as
> the same 2-shard txn as MKDIR (PREPARE/DECIDE/RESOLVE) on the
> production host. LOCAL dest only; HASHED/SPLITTING are INVAL/BUSY.
> Directory src is INVAL; `LINK_SHARD` is INVAL (old fan-out). Same
> scratch smoke: link the created file, both names, nlink=2, duplicate
> EXIST, miss NOT_FOUND, crash catch-up keeps the extra name and
> nlink=2.
>
> **10.5c-17 is in (gated):** nlink>1 file UNLINK is dest dentry DEL +
> inode `nlink--` as the same 2-shard txn as LINK (PREPARE/DECIDE/
> RESOLVE) on the production host. Last-link with dentry shard ≠
> inode shard uses the same path; last-link on one shard stays a
> single Raft entry. LOCAL parent only; HASHED/SPLITTING are
> INVAL/BUSY. Same scratch smoke: extra link name, unlink it,
> surviving nlink=2, second unlink NOT_FOUND, crash catch-up keeps
> the extra name gone and the remaining link.
>
> **10.5c-18 is in (gated):** utimens is the inode fence on the
> production host (`EFS_MD_CMD_UTIMENS`). SETATTR with only MTIME/ATIME
> bumps `mtime_gen` and assigns the times; mixed mode+time is INVAL;
> SIZE was INVAL until 10.5c-21. Same-group lanes only — a fenced
> lane on another Raft group is INVAL this slice. Same scratch smoke:
> setattr mtime=1000000000 on the created file, getattr, crash catch-up
> keeps that mtime.
>
> **10.5c-19 is in (gated):** same-dir LOCAL file RENAME is src dentry
> DEL + dest dentry PUT + inode parent/ctime as the same 2-shard txn
> as LINK (PREPARE/DECIDE/RESOLVE). Cross-dir, directories, and
> HASHED/SPLITTING are INVAL/BUSY; dest exists is EXIST; `RENAME`
> (by ino) is INVAL (`RENAME_AT` is hosted). Same scratch smoke:
> `raft-smoke-n` → `raft-smoke-m`, old gone, new stays through crash.
> Restart persists `last_applied` without compacting the log
> (`efs_raft_restore_applied`) so CREATE is not replayed onto a KV
> that already renamed the name.
>
> **10.5c-20 is in (gated):** READDIR and LOOKUP_PATH on the production
> host go through ReadIndex + applied KV (same as 10.5c-10 LOOKUP/
> GETATTR). HASHED dir lanes are ReadIndexed via
> `host_read_inode_lanes`; SPLITTING READDIR is BUSY this slice.
> LOOKUP_PATH is hop-by-hop (dentry shard ReadIndex if it differs,
> then child lanes). Same scratch smoke: ROOT listing contains
> `f`/`m`/`l`/mkdir and not `n`/`u`/`h`; `/raft-smoke-f` and
> `/raft-smoke-m` resolve; crash keeps that.
>
> **10.5c-21 is in (gated):** SETATTR SIZE on the production host is
> `EFS_MD_CMD_TRUNCATE` (content_epoch fence + `base_size`). Chunk-
> aligned or zero only — unaligned sizes need a tail candidate and
> stay INVAL this slice. Mixed SIZE+mode/time is INVAL. Same-group
> lanes only. Same scratch smoke: setattr size=131072 on the empty
> created file, getattr, crash catch-up keeps that size (utimens
> after truncate still keeps mtime=1000000000). Remaining: cross-group
> propose, then the rest of the
> mutations (not a cutover of `efs-test`, not step 11).
>
> **The one rule 10.5c owes 10.5b** (`include/efs/raft_disk.h`): the Raft log is
> the durability boundary and the applied KV is a replayable view, so never
> call `efs_raft_snapshot()` until the KV is durable through `last_applied`
> (`efs_kv_lsm_flush()`). The snapshot drops the log prefix that would
> otherwise replay those commands. `efs_raft_new()` starts at
> `last_applied = snap_idx`. The host persists applied without
> compacting and restores it on restart (`efs_raft_restore_applied`)
> so a durable KV is not re-applied. Apply must still stay idempotent
> in the window before that persist.
>
> Production `efsd` still uses the in-memory table and the snapshot /
> root-2PC flush; only after 10.5c does step 11 delete that flush. If a
> decision is missing, stop and ask.

**Rule for picking the next one after that:** the order is
[architecture.md](../architecture.md) §10, step by step. If a step looks like
it needs a design decision that is not already in the spec, that is a signal
to stop and ask — not to invent one.

---

## 2. Routing: "I am changing X"

Read the row's **Read** column and nothing else first. The **Governs** column
is what your change must not break; the **Gate** column is what proves it.

| You are changing | Read | Governs | Gate |
| --- | --- | --- | --- |
| Any wire message | [architecture.md §7](../architecture.md) op matrix, `include/efs/protocol.h` | build-ID compat; restart all servers together | `make test`, solo posix |
| Path lookup / dentries | [protocols/directory.md](protocols/directory.md) | I5–I8 | posix, posix2 |
| create / unlink / rename / link | [protocols/directory.md](protocols/directory.md), [protocols/transactions.md](protocols/transactions.md) | I5–I9, I16, I17 | posix, posix2, posixstress |
| Anything about file size, mtime, ctime | [protocols/data.md](protocols/data.md) "lanes"/"times" | I21, I22 | posix (size-visibility tests), posix2 |
| Chunk write / publish / truncate / append | [protocols/data.md](protocols/data.md) | I11–I15, I20–I22, I24, I25 | posix, posixpersist, fio honest matrix |
| The read path, or read prefetch/caching | [protocols/data.md](protocols/data.md) "validated collect" | I24, I13 | posix, posix2 (cross-client visibility) |
| Client reconnect, leases, locks, open-unlinked | [protocols/sessions.md](protocols/sessions.md) | I19, I23, I16 | posix2, posixstress |
| Cross-shard anything | [protocols/transactions.md](protocols/transactions.md) | I16, I17, I9 | posix2, posixstress |
| Raft, KV, replication, membership | [architecture.md §7.1/§7.8](../architecture.md), [failure-tolerance.md](failure-tolerance.md) | I1–I4, I10, I18 | `tests/test_sim`, leaks |
| Production Raft host (`EFS_MD_RAFT`) | `src/server/raft_host.c`, [architecture.md §10](../architecture.md) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied | `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
| Simulator / applied KV SM | [verification.md](verification.md), `include/efs/sim.h`, `include/efs/meta_apply.h`, `include/efs/raft.h` | I1–I4, I9, I10, I13–I16, I20–I23, I25 | `tests/test_sim`, `tests/test_meta_apply`, `tests/test_raft` |
| Op-ID / idempotency window | [architecture.md §7.9](../architecture.md), `include/efs/opid.h` | I16 | `tests/test_sim` |
| A hot path, for speed | [performance.md](performance.md) | P1–P4, §8 contract | fio honest matrix — **never** the stock `perf` write column |
| FUSE client behavior | [architecture.md §7.7](../architecture.md) | I24, kernel-cache rules | posix, posix2 |
| Module structure / file layout | [development.md](development.md) | ~1000-line file cap; header-only deps | `make test` + the suite for whatever moved |
| The spec itself | [development.md](development.md) "machine gate" | one home per normative table | regenerate `architecture-full.md`; links + `I1..I25` resolve |

Invariant texts live in [architecture.md](../architecture.md) §4. Where state
lives and which shards an operation touches live in §5 and §6 — those two
tables are the single source of truth; satellites explain them and never
restate them.

---

## 3. Done means

A change is finished when all of these hold. Do not stop early and do not
substitute one for another.

1. **It builds on a node** — never in the NFS home
   (see the fcstor deploy rule; a local `make` produces AVX-512 objects that
   SIGILL on the AMD test nodes).
2. **Unit tests pass:** `make test`, plus `test_meta_v6`, `test_dir_stats`,
   `test_ino_path`, `test_meta_slot` where metadata is involved.
3. **The gate from your routing row passes**, run with
   `tests/run_tests.sh <suite>`, and the result directory is recorded.
4. **A failure is a failure.** A timeout is not a skip; an empty TSV is not a
   pass; a suite that ran against a dead mount (`findmnt` not
   `fuse.efs-fuse`) did not run at all.
5. **The measurement is honest.** If you claim a speedup, it came from the
   documented method in [performance.md](performance.md), not from a cache.

---

## 4. Never, without asking

- Invent a design decision the spec does not contain (see §1).
- Restate a normative table in a satellite — link to its one home instead.
- Add a component as new monolith code; it lands inside the carved
  boundaries ([development.md](development.md)).
- Weaken an invariant to make a test pass.
- Widen a timeout instead of removing the work that made it slow.

---

## 5. If you are an AI agent — or briefing one

This page exists so that a **less advanced model can produce a correct
change**. That works only if the task arrives pre-chewed: the model's job is
execution inside hard edges, not exploration. The briefer (human or
orchestrator) owns: picking the step (§1), decomposing it until every
instruction is mechanical, naming the exact files to read (§2 — the Read
column and nothing else), and reviewing the diff. The agent owns: staying
inside the named files, and the checklist in §3 — all of it.

A briefing that works, paste-able:

```text
You are making ONE behavior-preserving change to the efs repo.
Read first, in order, and read nothing else:
  docs/arch/START-HERE.md, then only the files your routing row names.
Task: <one step, decomposed until mechanical: what moves, what does
not, what is forbidden>
Hard rules: no logic changes outside the task; no renames; no new
dependencies; no files outside the ones named; if anything seems to
need a design decision the spec does not contain, STOP and report —
do not decide.
Done means: builds on a test node (never in $HOME), make test passes,
the routing row's gate passes via tests/run_tests.sh, and you record
the result directory. A timeout is a failure. Verify the mount with
findmnt before trusting any suite result.
```

Two traps a pasted briefing must name because an outside agent cannot
rediscover them (the other recurring ones are already in §3):

- **EEXIST on a unique, never-used name is a bug, never benign.** Do not
  swallow it as a race and move on.
- **`pgrep -x`, never `pgrep -f`** on this project's processes — the `-f`
  pattern matches your own ssh command line and kills your own session.
