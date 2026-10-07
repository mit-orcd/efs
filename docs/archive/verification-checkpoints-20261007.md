# Historical simulator and production rollout checkpoints

[Current verification guide](../how-it-works/verification.md) ·
[Current implementation limits](../status/spec-implementation.md)

Preserved Oct 7 from the active guide: Sep 2 harness state and Step 9–10.5c
rollout narrative. "Production uses the in-memory table", env-gated Raft,
"not hosted" and other stage-specific statements below describe their original
checkpoint, not current code. Recorded gates identify that stage only; they
do not establish current hardware or public-path acceptance.

**Harness (Sep 2).** `include/efs/sim.h` + `src/sim/` + `tests/test_sim`
drive a **fixed RF=3 Raft group** (servers 0..2) applying the §5 ordered-KV
SM (`src/meta/meta_apply.c`) against mem store and loop transport. Same seed
replays the same history hash. Metadata mutations are Raft log commands;
reads are leader + ReadIndex (no clock leases). Crash keeps KV + Raft persist
and restarts the SM from them. Faults that gate today: message drop,
partition (including minority leader / lost quorum), crash+restart with the
same disk, PUT-then-crash before publish (unpublished must not be readable),
publish without all k+f fragments, silent fragment corruption (skipped, never
decoded). Raft invariants gated in-sim: I1 (one leader per term), I2/I10
(acknowledged create survives f=1 replica crash), I3 (higher term fences the
old leader), I4 (partitioned leader cannot advance commitIndex). Op-IDs (I16)
persist in the CREATE batch. Production RPCs do not carry op-IDs yet, and
production `efsd` still uses the in-memory table. Sessions (I23) and
open-unlinked leases (I19) are driven: `efs_session` is a pure SM over one
KV, and the simulator runs the three-phase revocation barrier
(`ACTIVE(E) → FENCING → FENCE/ACK → ACTIVE(E+1)`) plus last-link keep /
reclaim. A fence-ACK withheld mid-barrier is a real event in `tests/test_sim`.
Data-target PUTs are not fenced. Production `efsd` still uses the in-memory
table. Cross-shard
transactions (I17) are driven: `efs_txn` is a pure SM over one KV per
participant, and the simulator runs MKDIR as a 2-shard PREPARE / DECISION /
RESOLVE (visibility at the coordinator's durable decision; L5 abort after
crash-during-prepare). Reconfiguration (I18) is driven: the simulator owns a control-plane
Raft group for **desired** placement, and each metadata group moves **actual**
membership through joint consensus (learners catch up before they vote). A
membership-transition interrupt (drop the new majority mid-joint) is a real
generator event in `tests/test_raft`. Production `efsd` still uses the
in-memory table.

**Step 9 in-sim (gated).** Fragments are FileID-scoped and named by a unique
candidate generation (never G+1). Publication is `CAS(expected = committed
base)` of that candidate plus a lane MAX in one KV batch; a CAS miss does
not apply the MAX (I21). A publication naming a superseded `content_epoch`
is rejected; data-target PUT is not (I22/I23). `test_sim` and
`test_meta_apply` gate I13, I14, I15, I20, I21, I22 (epoch fence on lane-0
files), and I25, plus rejection of duplicate-node / wrong-profile evidence.
Not in this step: multi-chunk I24, truncate range-delete, O_APPEND, degraded
`u` / profile-cutover barriers. Production `efsd` is unchanged.

**Step 10 in-sim (gated).** A directory moves LOCAL → SPLITTING(e) → HASHED(e).
During SPLITTING the hashed side wins: a tombstone is ENOENT and the
migrator never resurrects it (I8). CREATE co-locates the file inode with
the dentry shard. POSIX locks are a pure SM over KV: classic+OFD `fcntl`
share one conflict domain, `flock` is independent; conflict → AGAIN, wait
FIFO, same-inode cycle → DEADLK, cap → NOLCK. A session fence dequeues
waiters as STALE and never grants them. `test_sim`, `test_lock`, and
`test_meta_apply` gate I8 and those lock properties. Not in this step:
hashed RMDIR/`dentry_seq`, directory-rename ancestry, `utimens` dir fence,
pressure-based spread, partial-unlock split, production `efsd` wiring.

**Step 10.5a in-sim (gated): the simulator runs on the durable KV.** Set
`EFS_SIM_KV_DIR=<dir>` and every server's applied state lives in
`efs_kv_lsm_open` instead of `kv_mem`, with no test changes — each sim
instance gets its own store under that directory, because `kv_mem` hands
every instance an empty one and reusing a directory would carry state across
cases. The sim's memtable threshold is deliberately tiny (512 B) so the runs
cross it: a threshold the sim never reaches would check every invariant
against a memtable and never against a segment or a compaction result. The
gate is all five sim suites OK with segments actually produced (51 L0 + 81
L1 on the recorded run). This is the substitution that makes the durable
store's correctness a property the existing invariant checkers prove, rather
than a separate claim.

`test_kv_lsm` covers what the sim cannot: semantics parity with `kv_mem`
(including the short-buffer probe and batch atomicity), a **real** crash —
the child `_exit`s without unwinding after a batch returned, so only an
fsync that already happened can make the parent's reopen see the write — a
torn WAL tail that must be discarded while its prefix survives, tombstones
shadowing values across levels, and 4000 keys with a third deleted verified
through auto-compaction and a reopen. One bug it caught: the compaction merge
advanced the winning source before comparing the others against its key, so
older duplicates survived their own tombstone. A scan-only test cannot see
that — it needs the compacted output read back.

**Step 10.5b in-sim (gated): the simulator runs on the durable Raft log.**
Set `EFS_SIM_RAFT_DIR=<dir>` and every server's Raft persistent state lives
in `efs_raft_disk_open` instead of `raft_mem`, with no test changes; set both
variables and both backends are durable at once. Two things make this a real
substitution rather than a link-time one. First, **all three of a server's
groups share one log**, which is the production shape (thousands of groups
per node multiplexing one fsync stream) and puts that sharing under test —
a group whose records leaked into another's state would show up as a
cross-group hard-state or log failure. Second, **a simulated restart really
closes and reopens the log**, so every crash the simulator injects becomes a
replay of what was on disk, under all the existing fault injection, rather
than a store object that quietly kept its RAM.

The gate is all five sim suites OK on the durable Raft store alone and on
both durable stores together. It was verified **non-vacuous by negative
control**: making replay silently drop ENTRY records fails `test_raft_store`
(16 checks) and `test_sim` (3, in transaction recovery). A gate that cannot
fail is not a gate, and for a store whose whole job is "what survives a
restart" that is the only way to know the sim depends on it.

`test_raft_store` covers what the sim cannot: answer-for-answer parity with
`raft_mem` driven through one shared script (gap rejection, in-place slot
overwrite, the short-buffer required-length report, snapshot-index term
lookup, `NOT_FOUND` vs never-saved config), a **real** crash via
`fork`+`_exit`, a torn tail that must be discarded *without* losing the good
records behind it and must leave the file appendable, truncation surviving a
reopen (a follower that loses a truncation gets conflicting entries back and
diverges), and rotation preserving live state through a reopen. One bug it
caught: replaying a *rotated* log applies the snapshot record before any
entries exist, which the validation inherited from `raft_mem` rejected —
correct for a live caller, wrong for replay, so the prefix drop is clamped on
the replay path and strict on the callback.

**Step 10.5c-1 in-sim (gated): single-shard reads and SETATTR over the
applied KV.** READDIR walks LOCAL / SPLITTING / HASHED including the
lane-0 alias (a hashed name that lives on the inode's own shard is not a
second copy). SETATTR mode/owner is one Raft entry on the inode shard:
ctime MAX-clamps, mtime does not move, a stale `expect_gen` is STALE.
GETATTR is the §7.3 / §7.4 validated double collect — MAX over the inode
row and the active write lanes (a file) or the `used_shards` dir lanes (a
HASHED directory), including committed-but-unmaterialized reductions, then
a recheck of `lane_seq` / pending txids / `content_epoch` / `mtime_gen` /
`used_shards`; an unreachable coordinator is I9, not absence. A hashed
create or unlink stamps its dir lane on the dentry shard and does not
move the directory inode row's times. LOOKUP_PATH is a batched
ancestor walk (resume from the terminal, I9 on a dangling dentry,
INVAL through a non-directory). The simulator proposes SETATTR through
Raft, serves GETATTR/READDIR via ReadIndex (file lanes or dir lanes), and hops LOOKUP_PATH with a
ReadIndex per shard; a leader crash does not change a linearizable read.
Gate: `test_meta_apply`, `test_sim`.

**Step 10.5c-2 in-sim (gated): `utimens` inode fence.** Only utimens can
set a time backwards, so it is the only op that bumps `mtime_gen` and
pushes that generation onto every active write lane (file) or used dir
lane (HASHED/SPLITTING directory). getattr ignores lane mtimes stamped
under an older generation; a later write re-stamps at the new generation
and is visible again. atime-only does not fence. chmod after utimens
does not hide the explicit mtime (it does not bump `mtime_gen`). The
simulator proposes the fence through Raft; a leader crash / restart does
not resurrect a stale lane mtime. Gate: `test_meta_apply`, `test_sim`.

**Step 10.5c-3 in-sim (gated): truncate range-delete.** SETATTR(size) is
one apply batch over the inode row and active lanes: bump `content_epoch`,
set `base_size`, fence each lane, range-delete chunk-map keys whose
byte range starts at or past the new size, and optionally CAS-publish the
straddling tail chunk in the same batch. A publish carrying the old epoch
is STALE (I22). The simulator proposes truncate through Raft; crash /
restart does not restore deleted chunks. Gate: `test_meta_apply`,
`test_sim`. Not in this step: O_APPEND, production `efsd`.

**Step 10.5c-4 in-sim (gated): O_APPEND.** Reservation is against the real
EOF (`MAX` of `base_size`, active-lane ends, pending committed reductions,
and the live watermark) — a private counter that ignores a prior pwrite
is a correctness bug. An unresolved burst holds an append barrier:
publish with `new_size` past the watermark is BUSY; getattr reports the
contiguous resolved frontier, not lane `max_end`. Resolve is COMPLETED,
ABORTED_HOLE, or FENCED_HOLE; a retried reserve of the same op-ID
recovers the same offset (`opid.extra`, I16). The simulator proposes
reserve and resolve through Raft; crash / restart keeps the reservation.
Gate: `test_meta_apply`, `test_sim`. Not in this step: cross-shard
rename/link/rmdir, production `efsd`.

**Step 10.5c-5 in-sim (gated): LINK, UNLINK nlink>1, RMDIR, file RENAME.**
LINK is dest dentry + inode `nlink++`. UNLINK last-link is one shard iff
dentry and inode co-locate, else a 2-shard txn. RMDIR is a txn: LOCAL
emptiness is one range check; HASHED emptiness is `dentry_seq` guards on
used lanes (the participant cap stays 8). File RENAME is same-dir LOCAL
one shard / cross-dir a txn; replace is not in this step. Directory
rename is INVAL until `parent_version` is on the inode row. A dest dentry
PUT CASes the current txn version so a reused name is not stuck at
expected 0. The simulator proposes through Raft; I17 abort leaves no
half-apply. Gate: `test_meta_apply`, `test_sim` (mem and durable). Not
in this step: directory rename, production `efsd`.

**Step 10.5c-6 in-sim (gated): directory rename with `parent_version`.**
The inode row is 128 B and carries `parent` + `parent_version` (bumped on
reparent). Ancestry of `dst_parent` is a read-set of pver sidecar guards,
not exclusive on ancestor inodes. src in the chain is INVAL. Concurrent
`rename(a→b/a)` ‖ `rename(b→a/b)` BUSYs the second on the first's
exclusive pver PUT (I17). Cross-dir directory rename adjusts parent nlink;
file rename does not. The participant cap stays 8. Replace is not in this
step. Gate: `test_sim` (mem and durable). Not in this step: a new export,
production `efsd`.

**Step 10.5c-7 in-sim (gated): export ROOT is a Raft mkfs.** Boot no longer
seeds the root with a local KV write. It elects, proposes
`efs_meta_apply_init` (leader-stamped `now`), and apply is idempotent.
getattr of ROOT is ReadIndex. 100% drop cannot create the export.
Crash/restart keeps ROOT. Gate: `test_sim` (mem and durable). Not in
this step: production `efsd` (that is 10.5c-9).

**Step 10.5c-8 in-sim (gated): export salt at mkfs.** MKDIR scatter hashes
with the per-export salt chosen at mkfs (`hash(parent, name, salt) &
0xFFF`). Salt lives on the ROOT shard; a missing record reads as 0;
idempotent mkfs does not change it. Crash/restart keeps salt and later
mkdirs still scatter with it. Gate: `test_meta_apply`, `test_sim` (mem
and durable).

**Step 10.5c-9 (gated): production Raft host in `efsd`.** Env-gated
`EFS_MD_RAFT=1` (no-op when unset). Two groups (odd/even shard parity),
one `raft_disk` + one `kv_lsm` per node under `<storage>/mdraft/`, Raft
messages on existing peer TCP, a tick thread, idempotent `raft-mkfs` on
the ROOT group. Gate: `test_wire` (codec) and
`tests/stress/raft_host_smoke.sh` on a scratch 4-node cluster (port 19820,
`/tmp` storage; live cluster untouched) — elect, mkfs ROOT on every
voter, kill -9 follower then leader, restart catch-up keeps ROOT.
Not in this step: LOOKUP/GETATTR (that is 10.5c-10).

**The flag is gone (Step 11, Sep 11 2026).** Every "When `EFS_MD_RAFT=1`"
and "Flag off is a no-op" below records how that slice was gated while
the old in-memory table still existed. The Raft+KV host now starts
unconditionally and is the only metadata engine; the table those slices
bypassed is deleted. The 19820 scratch cluster is retired;
`tests/stress/raft_host_smoke.sh` starts and stops its own private
cluster.

**Step 10.5c-10 (gated): LOOKUP/GETATTR through ReadIndex + KV.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_LOOKUP` / `GETATTR` skip the in-memory
table: leader + ReadIndex, then `efs_meta_apply_getattr` /
`efs_meta_apply_lookup` on the applied KV. A follower replies
NOT_PRIMARY; a missing name is NOT_FOUND. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — getattr of ROOT (dir, nlink=2) on
the leader, NOT_PRIMARY on a follower, lookup miss, same after kill -9
catch-up. Not in this step: CREATE (that is 10.5c-11).

**Step 10.5c-11 (gated): file CREATE through Raft.** When `EFS_MD_RAFT=1`,
`EFS_MSG_INODE_CREATE` proposes `EFS_MD_CMD_CREATE` (same encoding as
the sim) on the dentry-shard group, waits `last_applied`, then LOOKUP +
GETATTR. Duplicate is EXIST; `S_IFDIR` is INVAL (MKDIR is a 2-shard
txn). `CREATE_SHARD` is INVAL (old fan-out). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — create a file under ROOT, lookup +
getattr, second create EXIST, name survives kill -9 catch-up.
Not in this step: MKDIR (that is 10.5c-12).

**Step 10.5c-12 (gated): MKDIR through Raft.** When `EFS_MD_RAFT=1`,
`S_IFDIR` CREATE is a 2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE` /
`RESOLVE`, same encoding as the sim) over parent dentry+row+dseq and
child inode+alloc. The receiving node must lead every participant
group; otherwise `NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — mkdir under ROOT (retry names until
accepted), lookup, ROOT nlink=3, name survives kill -9 catch-up.
Not in this step: last-link UNLINK (that is 10.5c-13).

**Step 10.5c-13 (gated): last-link file UNLINK through Raft.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_UNLINK` proposes `EFS_MD_CMD_UNLINK`
(same encoding as the sim) on the dentry-shard group, waits
`last_applied`. Missing name is NOT_FOUND; `S_ISDIR` and nlink>1 are
INVAL (RMDIR / hardlink unlink are 2-shard txns). `UNLINK_SHARD` is
INVAL (old fan-out). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — create+unlink a file under ROOT,
lookup miss, second unlink NOT_FOUND, name stays gone after kill -9
catch-up.
Not in this step: SETATTR (that is 10.5c-14).

**Step 10.5c-14 (gated): mode/owner SETATTR through Raft.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_SETATTR` proposes `EFS_MD_CMD_SETATTR`
(same encoding as the sim) on the inode-shard group, waits
`last_applied`. Missing ino is NOT_FOUND (no propose); SIZE/MTIME/ATIME
are INVAL (truncate / utimens later). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — setattr mode 0600 on the created
file, getattr confirms, miss ino NOT_FOUND, SIZE mask INVAL, mode
survives kill -9 catch-up.
Not in this step: RMDIR (that is 10.5c-15).

**Step 10.5c-15 (gated): empty LOCAL RMDIR through Raft.** When
`EFS_MD_RAFT=1`, directory UNLINK is a 2-shard txn (`EFS_MD_CMD_PREPARE`
/ `DECIDE` / `RESOLVE`, same encoding as MKDIR) over parent dentry +
parent nlink/dseq and child inode. HASHED/SPLITTING are INVAL/BUSY.
The receiving node must lead every participant group; otherwise
`NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — mkdir a dedicated name, rmdir,
lookup miss, second rmdir NOT_FOUND, rmdir of a file INVAL, name
stays gone after kill -9 catch-up.
Not in this step: LINK (that is 10.5c-16).

**Step 10.5c-16 (gated): LINK through Raft.** When `EFS_MD_RAFT=1`,
`EFS_MSG_INODE_LINK` is a 2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE`
/ `RESOLVE`) over dest dentry + dest parent mtime/dseq and source
inode `nlink++`. LOCAL dest only; HASHED/SPLITTING are INVAL/BUSY.
Directory src is INVAL. `LINK_SHARD` is INVAL (old fan-out). The
receiving node must lead every participant group; otherwise
`NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — link the created file under ROOT,
both names, nlink=2, duplicate EXIST, miss NOT_FOUND, directory INVAL,
extra name and nlink survive kill -9 catch-up.
Not in this step: nlink>1 UNLINK (that is 10.5c-17).

**Step 10.5c-17 (gated): nlink>1 UNLINK through Raft.** When
`EFS_MD_RAFT=1`, file UNLINK with `nlink>1` (or last-link with
dentry shard ≠ inode shard) is a 2-shard txn (`EFS_MD_CMD_PREPARE`
/ `DECIDE` / `RESOLVE`) over dest dentry DEL + dest parent mtime/dseq
and source inode `nlink--` (or inode DEL). LOCAL parent only;
HASHED/SPLITTING are INVAL/BUSY. Last-link on one shard stays
`EFS_MD_CMD_UNLINK`. The receiving node must lead every participant
group; otherwise `NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — extra link of the created file,
unlink that name, surviving nlink=2, second unlink NOT_FOUND, extra
name stays gone after kill -9 catch-up.

**Step 10.5c-18 (gated): utimens inode fence through Raft.** When
`EFS_MD_RAFT=1`, SETATTR with only MTIME/ATIME is `EFS_MD_CMD_UTIMENS`
on the inode shard: bump `mtime_gen`, assign mtime/atime. Mixed
mode+time is INVAL; SIZE is INVAL. A fenced lane whose Raft group is
not the inode group is INVAL this slice. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — setattr mtime=1000000000 on the
created file, getattr, mtime survives kill -9 catch-up.

**Step 10.5c-19 (gated): same-dir LOCAL file RENAME through Raft.**
When `EFS_MD_RAFT=1`, `RENAME_AT` of a file in a LOCAL directory is a
2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE` / `RESOLVE`) over src
dentry DEL + dest dentry PUT + inode parent/ctime + parent mtime/dseq.
Cross-dir, directories, HASHED/SPLITTING are INVAL/BUSY; dest exists
is EXIST; `RENAME` (by ino) is INVAL. The receiving node must lead
every participant group; otherwise `NOT_PRIMARY`. Restart restores
`last_applied` without compacting (`efs_raft_restore_applied`) so
CREATE is not replayed onto a KV that already renamed the name.
Flag off is a no-op. Gate: `tests/stress/raft_host_smoke.sh` —
`raft-smoke-n` → `raft-smoke-m`, old NOT_FOUND, new OK, old stays
gone after kill -9 catch-up.

**Step 10.5c-20 (gated): READDIR and LOOKUP_PATH through ReadIndex +
KV.** When `EFS_MD_RAFT=1`, `INODE_READDIR` and `INODE_LOOKUP_PATH`
are served from the applied KV (not the in-memory table). READDIR
ReadIndexes the directory (and HASHED used dir-lane groups) then
scans; SPLITTING is BUSY. LOOKUP_PATH walks hop-by-hop with a
ReadIndex on each dentry shard. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — ROOT listing contains the
created/renamed/link/mkdir names and not the unlinked ones;
`/raft-smoke-f` and `/raft-smoke-m` resolve; listing survives
kill -9 catch-up.
Remaining: cross-group propose, then the rest of the mutations.
Not in this step: cutting over the live `efs-test` table, step 11.

**Step 10.5c-21 (gated): SETATTR SIZE / chunk-aligned truncate through
Raft.** When `EFS_MD_RAFT=1`, `SETATTR` with only the SIZE bit proposes
`EFS_MD_CMD_TRUNCATE` (`content_epoch++`, `base_size = S`). Chunk-
aligned or zero only; unaligned sizes (need a tail candidate) and
mixed SIZE+mode/time stay INVAL. Same-group lanes only — a fenced
lane on another Raft group is INVAL this slice. Flag off is a no-op.
Gate: `tests/stress/raft_host_smoke.sh` — empty `raft-smoke-f` to
size 131072, getattr, size survives kill -9 catch-up (utimens after
truncate still keeps mtime=1000000000).

**10.5c-22 — chunk publish + GETCHUNKS from KV (same-group / lane 0).**
`REPORT_CHUNKS` packs one `EFS_MD_CMD_PUBLISH` per rec (CAS + lane
MAX). `GETCHUNKS` is ReadIndex + `efs_meta_apply_get_chunk`. First-use
of a lane whose Raft group ≠ inode group is INVAL this slice (that is
a 2-shard txn). Flag off is a no-op. Gate: same smoke — create
`raft-smoke-p`, empty GETCHUNKS, publish chunk 0 size=131072,
GETCHUNKS count=1, mapping and size survive kill -9 catch-up.

**10.5c-23 — unaligned truncate tail CAS (same-group / lane 0).**
SETATTR SIZE that is not chunk-aligned mints a tail candidate and
CAS-publishes it in the same `EFS_MD_CMD_TRUNCATE` entry. A later
size=0 range-deletes the map. Same-group lanes only. Flag off is a
no-op. Gate: same smoke — after publish, size=1000 keeps ci=0,
size=0 clears GETCHUNKS, crash keeps that.

**10.5c-24 — cross-group propose (no new opcode).** A node that does
not lead a participant group submits via non-empty `EFS_MSG_RAFT_MKFS`
(`group` + cmd) or ReadIndex (`group` only). Followers wait apply.
Unhosted CREATE/LOOKUP/GETATTR/UNLINK/LINK bounce to a dual-host
(never self). RMDIR looks up the dentry first so an even-shard child
bounces before resolve. READDIR emits a dentry stub for unhosted
children. Directory RENAME is INVAL from `dent.type`. Flag off is a
no-op. Gate: same smoke — mkdir of even-shard `raft-smoke-xg*` from
raft_id 0, lookup/rmdir/link-dir/rename-dir/readdir, crash keeps
that dir (ROOT nlink=4).

**10.5c-25 — O_APPEND reserve (resolve on REPORT).** `INODE_APPEND`
proposes `EFS_MD_CMD_APPEND_RSV` on the inode group (bounce if
unhosted). Zero UUID skips the op-id window. Reply size is the
watermark; getattr stays the frontier while nopen>0. After a
covering `REPORT_CHUNKS` publish, OPEN reservations resolve
COMPLETED (`EFS_MD_CMD_APPEND_RES`) so the next write past the
watermark is not BUSY. Flag off is a no-op. Gate: same smoke —
create `raft-smoke-a`, append 131072, getattr size=0, publish,
getattr size=131072, crash keeps that size.
Not in this step: cutting over the live `efs-test` table, step 11.

**10.5c-26 — SYMLINK as CREATE + publish.** FUSE already creates
`S_IFLNK` and stores the target as ordinary chunk bytes. Host
CREATE packed that mode; publish/GETCHUNKS wrongly required
`S_ISREG`. Both now accept files and symlinks (directories stay
INVAL). No new opcode, no target column. Flag off is a no-op.
Gate: same smoke — create `raft-smoke-s` mode=0120777, publish
size=11, getattr+GETCHUNKS, duplicate EXIST, crash keeps mode
and size.

**10.5c-27 — same-dir LOCAL directory rename.** Host RENAME of a
directory is no longer INVAL. Bounce from the dentry type before
resolve (a scattered MKDIR dest on another group would otherwise
be I9). pver sidecar GUARDs on dst_parent ancestry and an exclusive
pver PUT on the renamed dir. Cross-dir and HASHED stay INVAL.
LOOKUP_PATH bounces unhosted child-inode hops like LOOKUP. Flag
off is a no-op. Gate: same smoke — mkdir `raft-smoke-rd`, rename
to `raft-smoke-re`, old gone / new present, miss NOT_FOUND, exist
EXIST, cross-dir INVAL, READDIR+LOOKUP_PATH, crash keeps the new
name.

**10.5c-28 — HASHED dest CREATE.** Empty LOCAL dir split via DIR
begin / migrate / finish (`raft-dir` over existing `RAFT_MKFS`).
First use of a hashed dentry shard on another group is a 2-shard
txn (parent `used_shards` bit; dest dentry + co-located file inode
+ dir-lane). SPLITTING dest is BUSY. Unhosted HASHED lanes bounce
on LOOKUP/GETATTR/LOOKUP_PATH; ROOT READDIR stubs those children.
Flag off is a no-op. Gate: same smoke — dedicated `raft-smoke-hd`,
hashed-dentry file whose inode shard differs from the parent,
LOOKUP_PATH, ROOT READDIR, crash keeps the dir and files.

**10.5c-29 — HOLD open-unlinked leases.** `EFS_MSG_INODE_HOLD`
proposes SESSION LEASE_OPEN/CLOSE on the inode shard (no new
opcode; owner is the session stand-in). Last-link UNLINK with a
lease keeps nlink=0 (I19); last close reclaims. Directories INVAL.
Sessions/fencing are not hosted (FLOCK is 10.5c-30). Flag off is a no-op. Gate:
same smoke — dedicated `raft-smoke-k`, open, unlink, getattr
nlink=0, crash keeps the inode, close reclaims.

**10.5c-30 — non-blocking FLOCK grant/release.** `EFS_MSG_INODE_FLOCK`
proposes LOCK GRANT/RELEASE on the inode shard (no new opcode;
whole-file FLOCK domain; owner is the session stand-in). Conflict
is BUSY. Blocking wait queues, ranges, and F_GETLK are not hosted.
Flag off is a no-op. Gate: same smoke — dedicated `raft-smoke-w`, EX
owner=1, EX owner=2 BUSY, UN, EX owner=2, crash keeps the lock.

**10.5c-31 — non-blocking whole-file fcntl.** Same
`EFS_MSG_INODE_FLOCK` opcode with `EFS_FLOCK_FCNTL` (no new opcode;
record-lock domain; classic process owner kind). Same-domain
conflict is BUSY; flock on the same file is the other domain and
does not conflict. Ranges, F_GETLK, and blocking waits are not
hosted. Flag off is a no-op. Gate: same smoke — dedicated
`raft-smoke-c`, EX owner=1, EX owner=2 BUSY, flock EX owner=2 OK,
UN, EX owner=1 held through crash.

**10.5c-32 — non-blocking fcntl byte ranges.** Same
`EFS_MSG_INODE_FLOCK` opcode; optional `EFS_FLOCK_RANGE_LEN`
suffix (two native uint64_t start,end, half-open). Absent suffix
is whole-file. Struct size is unchanged. FLOCK domain rejects a
non-whole-file range (INVAL). Adjacent ranges grant; overlap is
BUSY. Blocking waits are not hosted. Flag off is a
no-op. Gate: same smoke — dedicated `raft-smoke-t`, EX `[0,100)`
owner=1, EX `[100,200)` owner=2 OK, EX `[50,150)` BUSY, inverted
INVAL, flock-domain range INVAL, owner=2 `[100,200)` held through
crash.

**10.5c-33 — F_GETLK leader read.** Same `EFS_MSG_INODE_FLOCK`
opcode with `EFS_FLOCK_GETLK` (no new opcode; no Raft entry).
Reply packs the first conflicting record or F_UNLCK. Same-owner
does not conflict. Flag off is INVAL. Gate: same smoke — on
`raft-smoke-t`, GETLK `[50,150)` reports owner=1 `[0,100)`,
own-range and free-range UNLCK, after crash GETLK `[100,200)`
still reports owner=2.

**10.5c-34 — blocking lock waits.** Same opcode with
`EFS_FLOCK_WAIT`: a conflicting grant queues FIFO at the leader
(leader memory, not Raft state) and the held RPC's reply is the
grant. A queued waiter blocks a later conflicting request (no
barging). Leader loss replies NOT_PRIMARY and the client
re-issues. WAIT with UN/GETLK is INVAL; flag off is INVAL. Gate:
same smoke — dedicated `raft-smoke-q`, a waiter pends behind a
held EX, a queued SH does not barge past the EX waiter, release
grants in FIFO order, and a waiter pending across the leader
kill re-issues on the new leader and grants after the surviving
holder's release.

**10.5c-35a — session record + register + establish.** Production
host applies `EFS_MD_SESS_CREATE` / `REGISTER` / `ESTABLISH`
(same bytes as the sim). GET is a ReadIndex (sub=0, not a log
command). No new opcode: mgmt `raft-session` reuses
`EFS_MSG_RAFT_MKFS` submit. CREATE is idempotent. Gate: same
smoke — create a uuid, register its session shard, establish,
GET reports ACTIVE + touched, and the record survives crash.

**10.5c-35b — real session identity on HOLD/FLOCK.** Optional
`EFS_SESS_WIRE_LEN` suffix carries `(uuid, epoch)` on
`efs_msg_inode_hold` / `efs_msg_inode_flock`; absent keeps the
stand-in. The host runs `efs_session_accept` on the inode shard
before proposing (wrong/not-established epoch → BUSY). mgmt
`raft-hold` / `raft-flock` / `raft-fcntl` take `[uuid-hex epoch]`.
Gate: same smoke — established uuid accepted, wrong epoch BUSY,
stand-in path intact.

**10.5c-35c — revocation barrier.** Production host applies
`EFS_MD_SESS_BEGIN` / `FENCE_LOC` / `ACK` / `FINISH` /
`LEASE_DROP`. mgmt `raft-session fence` is the coordinator (GET
shard≥4096 reads a `touched_shards` word). FENCE_LOC dequeues
waiters of that uuid/epoch (never granted). Gate: same smoke —
waiter of epoch 1 BUSY after fence, epoch 1 rejected, epoch 2
accepted after establish, ACTIVE epoch 2 after crash.

**10.5c-35d — append-reservation reclaim on fence.** `LEASE_DROP`
of epoch E resolves that session's OPEN reservations on the
shard as `FENCED_HOLE`. Optional `(uuid, epoch)` suffix on
`raft-append` tags the rsv; absent keeps the stand-in. Gate:
same smoke — reserve 128 KiB under epoch 1, getattr stays 0,
fence, getattr is 131072, old-epoch append BUSY.
