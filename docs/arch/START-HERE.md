# Start here — what to work on, and what to read first

[Architecture](../architecture.md) · [Roadmap](../scaling-roadmap.md) ·
[Development](development.md) · [Verification](verification.md) ·
[Product gaps](../product-gaps.md)

This page exists because of the bar in [development.md](development.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---

## 1. The task right now

**Where the project is (Sep 20 2026).** [architecture.md §10](../architecture.md)
steps 0–12 are **all landed and gated**: simulator, KV, Raft, cross-shard
txns, sessions, directory spread (including populated leftover migrate as a
2-shard txn), delete-2PC, and FUSE A–D. LOCAL dirs auto-begin SPLITTING when
`nents > EFS_DIR_SPREAD_MIN` and the background migrator drains leftovers to
HASHED. Production `raft-change` wires `efs_raft_change` in `raft_host`
(operator desired file + learner attach with C_old, then joint/COLD).
InstallSnapshot is in the Raft SM, and `raft_host` snapshots after
`efs_kv_lsm_flush` when `applied - snap_idx ≥ 256` using the existing WAL
item payload (`kv_snap.c`), not a new dump format.

**So there is no next §10 step.** What is left is the work queue in
[§1a](#1a-the-work-queue) — measured gaps, in the order they should be taken.
**Every open measurement/bug-chasing item has a runbook + script in
[runbooks.md](runbooks.md) (`tests/measure/*.sh`, pinned to build
`b2184a5c7faf-dirty`); start there.** `tests/preflight.sh` is the deploy
rule's pre-flight as one command — run it before anything else.
**W1–W5 done; W6 correctness gate met Sep 20** (its three open sub-items
are performance and each needs a user decision — see W6). Take the
lowest-numbered item that is not marked done; correctness items come before
every performance item. Each item names what to change, how to measure it,
what proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4); several items below are blocked on
exactly that and say so.

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, TCP), clients fcstor007–015 at `/tmp/efs-mount`.
19820 is retired. Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs`
without being asked — several items below run on the existing data. Before
touching the cluster, run the **pre-flight** in the fcstor deploy rule
(`.cursor/rules/efs-fcstor-deploy.mdc`).

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page.

**Txn finisher (Sep 26, `src/server/raft_host.c` `host_txn_commit`).**
A compaction stall longer than the 400 ms apply wait turns an in-flight
DECIDE/RESOLVE into BUSY and leaves the EXCL intent until the 5 s
recovery scan; that intent makes every later create on the shard BUSY
and the client burns its 16 attempts into EIO. `host_txn_commit` hands
that txn to a finisher thread, which reads the decision record and
proposes RESOLVE (retrying BUSY until 10 s). It does not remove the
stall — that is still W13, not ratified. Measured: first 9-host suite
on the build **200/201 all nine, 42–44 s**
(`results/posix/20260926-0345-fin`, `fin_q=0`, no stall that run).
Three immediate repeats (`-fin2` `-fin3` `-fin4`, uncited) slipped to
196–200 as the table grew: `dir_deep_nesting*` / `names_crazy_dirs`
timeouts plus a few EIO, with `fin_done` 4–11 and `apply_max` still
~1.6 s. Do not treat 200/201 as the steady score until a suite run
after the cluster has been busy still holds it. Do not start W13.

**Mkdir hammer (Sep 26, `68dfebb`, raft path still that commit at
`e8f3dc1`).** Propose was fsyncing the Raft log while holding `h->mu`,
then sending AppendEntries. 9×16 own-directory mkdir
(`results/measure/20260926-042515-mkdir-hammer`): idle p50 8.4 ms,
144-way p50 **258 ms** / 470 mkdir/s. The fsync now runs outside the
lock and overlaps the send; the leader does not vote for an entry
until that fsync finishes (`efs_raft_submit` / `efs_raft_durable`).
Same-morning rehammer (`20260926-044248-mkdir-hammer`): idle p50
**3.7 ms**, 144-way p50 **144 ms** / 735 mkdir/s. Still one round
trip per mkdir (one AppendEntries in flight). Sending every newer
suffix immediately (`0e81e49`, reverted by `e8f3dc1`) let a follower
apply 12 120 entries under the host lock
(`20260926-050319-mkdir-hammer`, pump hold 5.3 s). The kept code,
remeasured after that load (`20260926-050609-mkdir-hammer`): idle
p50 10.9 ms, 144-way p50 **189 ms** / 706 mkdir/s, apply_max 67 ms,
`fin_q=0`. Do not pipeline past the one in-flight batch. Do not
start W13.

**9-host suite on that build** (`results/posix/20260926-050825`,
timeline `results/measure/20260926-010808-w8-stall-timeline`):
194–199/201, 40–44 s, 0 NOTRUN. Group 0 changed leader once
(term 6294→6296, ~12 s); the EIO rows sit on that window. A single
mkdir probed during the suite was p50 14 ms. `names_crazy_dirs`
still times out on the slower hosts. That is the 144-way queue,
not a new lost update.



**I16 landed (Sep 23 02:47, `43bdf6a41f7d`) — gate runs owed.** Every
directory RPC (CREATE/MKDIR, UNLINK/RMDIR, LINK, RENAME_AT) now carries
an optional 36-byte op-id suffix `(client uuid, session epoch, seq,
contiguous ack)` (`EFS_OPID_WIRE_LEN`, `handler.c:dirop_opid`). The
window for an op lives on the **dentry shard of the (destination) name**
— deterministic from the request, always a participant, always in the
group that owns the shard (the coordinator is randomized, the parent
shard may be in the other group: both rejected). The leader probes it
(`host_opid_replay`) *before* the EEXIST/ENOENT pre-checks and answers a
replay from the recorded verdict (`host_opid_reply_ino`: current row if
the ino still exists, the stored ino/nlink otherwise). The verdict is
recorded **atomically with the op**: cross-group txns add a
`EFS_TXN_REDUCE_OPID` part (`host_prep_opid`; `fold_reduce` on
`EFS_KV_KIND_OPID` runs `efs_opid_fold` = ack + complete on whatever the
window holds at RESOLVE, so it commutes with the log path's
read-modify-write); the single-group log path appends a trailer to the
`CREATE` (flag bit `HOST_CREATE_F_OPID` in `cmd[1]`) / `UNLINK` /
`RMDIR` (by length) commands and `efs_meta_apply_*_op` writes the window
in the same `efs_kv_batch` (`opid_item`). The apply keeps
returning OK for a replay it sees (idempotent); the client-visible
EEXIST/ENOENT for a *genuine* second attempt still comes from the
leader's pre-check, which now runs after the probe. Client
(`inode_rpc.c`): one random uuid per mount (epoch 1), one seq space,
512-slot in-flight table; `ack = lowest in-flight − 1`, so a shard's
window advances past seqs it never served; the slot is held across every
BUSY/STALE/NOT_PRIMARY retry inside `rpc_send_recv_*`, so the retry is
byte-identical. Table full = op goes out with no identity (pre-I16
behaviour). `efs_opid_ack` now moves the watermark and shifts the
bitmap; `efs_txn_prepare_opid` rejects any key that is not exactly the
window key. Unit: `test_txn:test_opid_reduce` (commit records, abort does
not, commutes with a log-path fold, wire form), `test_meta_apply` I16
block (mkdir/create/unlink/rmdir replay = recorded verdict; the same op
without an id = EEXIST/ENOENT). `raft-obs` line gains `opid_replay=`.
**Bounded by design, and the two limits to know:** the reply cache is
16 entries per `(client, epoch, shard)` window (`EFS_OPID_REPLY_CACHE`;
`EFS_OPID_VAL_MAX` 512 → `txn.c VAL_MAX`); a 17th un-acked op on one
shard applies **unrecorded** (`efs_opid_fold` NOMEM → skip), i.e. falls
back to pre-I16. One client with > 16 concurrent dir ops on one shard
whose oldest is stalled hits that. If the 9-host gate still shows
EEXIST-on-fresh-name, check this first (count `opid_replay` and look for
>16 in-flight per shard) before suspecting the mechanism; raising the
cache is an implementation matter. Second limit: the dir op-id identity
is a per-mount random uuid (epoch 1), **not** the §7.5 session's
`(uuid, epoch)`, so a dead client's windows are never dropped by the
fence barrier §7.9 relies on ("a fenced session's records are dropped
wholesale") — a remount leaves its old windows behind. Follow-up, no
decision needed: seed `opid_uuid/epoch` from the client session and drop
`EFS_KV_KIND_OPID` keys for a fenced epoch in the barrier.
**Gate results on `43bdf6a` (Sep 23 02:52–03:07, rolled 02:49):**
`i17_leader_freeze.sh` run 1
(`results/measure/20260923-025259-i17-leader-freeze`): parent clean,
`arc_term_miss` 0→3, `opid_replay` 0→3, **0 worker errors** (the
`46d54e6` runs had 3 EEXIST + 1 ENOENT). Run 2 (`-025903-`): parent
clean, `opid_replay` 3→7, but **3 workers got `mkdir ENOENT`** at round 7
during the g0 freeze (fcstor007 p0, fcstor008 p3, fcstor015 p3); the
following `rmdir` of the same name succeeded, so the MKDIR committed and
only the *reply path* failed. Not an I16 miss: the replay path returns
OK, and `raft-host` logs no mkdir `rc=-3`. Root cause found in the
client: `rpc_send_recv_shard/_dual` returned `EFS_ERR_NOT_PRIMARY`
immediately on a **hintless** NOT_PRIMARY (`primary_id == 0` — a stale
leader that just stepped down and has not heard the new one), and
`efs_client_stat_ino` maps every RPC failure to NOT_FOUND, so
`ll_mkdir`'s post-mkdir `lookup_fill(new_ino)` (GETATTR) turned into
ENOENT for a dir that exists. Fix in tree (uncommitted, see §1b tail):
hintless NOT_PRIMARY backs off like BUSY (50 ms ×2^n, same 16-attempt
budget, EBUSY at the end) in both send paths; `ll_mkdir` logs
`fuse: mkdir ... ok ino=N but getattr rc=` when it happens. Server side:
`host_opid_reply_ino` logs `opid-replay ... (stub)` / `row gone`.
posix jobs=1: **200/201 + mmap SKIP** (`results/posix/20260923-030005`).
9-host: **189–191/201 per host, 0 not-run**
(`results/posix/20260923-030056`); fails = the six many-op timeouts
(item 4) + one-offs `names_crazy_roundtrip` EIO (014), `symlink_absolute`
EIO (012), `unlink_open_then_recreate` `b''` (012) — the same hintless-
NOT_PRIMARY class is the first suspect for the EIOs. `md_latency.py` 30 s
after the suite: mkdir 8.4 / create+close 19.3 / append+close 65.9 /
stat 0.5 / unlink 2.0 / rmdir 8.3 ms — post-suite churn, **not** a valid
comparison; remeasure on an idle cluster (commit flat 30 s).
**That client fix is `bb634d9`, rolled, and it closed the getattr
case.** On `ad292b9` (getattr reads a COMMITted-but-unresolved inode
row; `efs_meta_apply_get_inode_tx`) the freeze
(`results/measure/20260923-144129-i17-leader-freeze` and `-144243-`)
had no new `fuse: mkdir ... but getattr` line. Run 2 was clean
(opid_replay 1→5, parent removable). Run 1 left `d-fcstor011-1-22`:
three clients got `mkdir EBUSY` after the 10.3 s retry budget while
recovery of shard 1504 took 16.5 s, then `rmdir` returned ENOENT for a
name that existed. The dentry was a COMMITted EXCL intent, and lookup
used a bare kv get. Handler lookups and resolves now go through
`efs_meta_apply_lookup_tx` / `resolve_tx` (same rule as the inode row;
the apply path stays plain). `rmdir`/`unlink` map `EFS_ERR_BUSY` to
EBUSY — they were returning EIO. Do not widen the 16-attempt budget to
outrun recovery.
**Gated on `7e29943f28ef-dirty` (rolled Sep 23 18:21 UTC, servers and
clients 007–015):** `i17_leader_freeze.sh` ×2
(`results/measure/20260923-182559-i17-leader-freeze`, `-182709-`) — both
PASS, 0 worker errors on all 9 clients, parent removable, `opid_replay`
0→1→3, `arc_term_miss` 0→3→15, every `fuse.log` `getattr=0 exhausted=0`.
Idle `md_latency.py` before the freeze: mkdir 9.8 / create 4.4 / append
7.1 / stat 0.4 / unlink 2.0 / rmdir 10.5 ms (reference 6.1/4.0/6.2/0.3/
1.5/5.2; mkdir/rmdir high — remeasure once `commit` is flat 30 s; the
post-freeze samples were taken with commit still moving and are
invalid). Known limit, by design: a client's 16-attempt budget (~10.3 s)
is shorter than recovery of a stranded txn (16.5 s on shard 1504) → the
app sees EBUSY, never a wrong answer. Do not widen it.

**Gates on `7e29943f28ef-dirty` (Sep 23 20:26–20:46 UTC):**
9-host posix **193–196/201, 0 not-run, 79–94 s**
(`results/posix/20260923-202626`, timeline
`results/measure/20260923-162609-w8-stall-timeline`). Fails are the six
many-op timeouts (item 4; mkdir p50 46 ms / p90 230 ms under the suite)
plus one-offs at the one group-2 election in the run (term 535→537,
leader raft-id 2→3, one root stat of 1.6 s): `concurrent_create_unlink_two_proc`
EIO on 009 and 010, `content_random_roundtrip` EIO on 014,
`concurrent_write_and_readdir` EIO on 007, `unlink_open_then_recreate`
reading `b''` on 009, `concurrent_appends` timeout on 010. Group 0 did
not change term during the suite (5498; `leader=0` in the status line is
raft id 0 = node 1, not "no leader"). No 2.4 s apply stall in this run.
`same_parent_storm.sh` 9×4×100 on Sep 23: parent ended `children=0
nlink=2` and `rmdir` succeeded, but fcstor014 logged two `mkdir ENOENT`
at round 63 (`results/stress/same-parent-20260923-202846`). Reply path:
`ll_mkdir` committed, then `lookup_fill` (no open fh) refreshed, and
`stat_refresh` maps every GETATTR failure to NOT_FOUND. Fixed: a
committed mkdir/symlink/link replies the dual-applied local row when
that refresh fails (`lookup_fill_committed`). Re-gate on the new
clients (servers still `7e29943f28ef-dirty`): PASS 9×4×100, parent
`children=0 nlink=2`, `RMDIR_OK`
(`results/stress/same-parent-20260924-170945`). The fallback line did
not fire, so this run did not exercise the failed refresh; the Sep 23
failure is the case the reply now covers.
Idle `md_latency.py` twice, 20 min apart, term stable and
`commit == applied`, commit still +30 in 30 s (the 0–2/s reaper band,
not catch-up): mkdir 8.5 / create+close **56.8** / append+close **59.5**
/ stat 0.5 / unlink 2.2 / rmdir 7.8 ms
(`results/measure/20260923-162535-idle-mdlat/mdlat-idle2.txt`). The 50 ms
mode is on the two ops that write a byte and close; mkdir is near the
6.1 ms reference. Not the post-roll election (the roll was 18:21, this
sample is 20:46). A 4 MiB raft-log tail still shows the suite, not the
idle 1/s — do not blame a command from it.
**Strace (Sep 24, `results/measure/20260924-021832-close-strace`,
`20260924-053648-leader-strace`): the 50 ms is a `recvfrom`, not fsync.**
On the client every slow mkdir/create/append is one `recvfrom` of a
441-byte reply (0x1B9). On both leaders, at the same instant as the slow
creates, a thread blocks 60–70 ms in `recvfrom` of a 2-byte frame; the
group-2 leader also blocks 77–114 ms reading a ~64 KiB frame (0x10049).
A different pair of threads on each leader sits ~52 ms in `recvfrom` of
an 81-byte frame for the whole trace — the 50 ms heartbeat cadence,
present with no creates running. The follower's long calls were a
2.000 s `clock_nanosleep` and 2.000 s reads, not this op. One create in
the leader trace was 10 ms; the other three were 66 / 73 / 118 ms.
Do not treat this as the Sep 20 wrong-condvar bug until a send-side
trace shows the 81-byte frame leaving a follower late.

**9-host on the mkdir-reply clients (Sep 24 20:33 UTC,
`results/posix/20260924-203211`, servers still `7e29943f28ef-dirty`):**
193–196/201, skip=1 (`mmap_write_read`), 0 not-run, 74–80 s. The Sep 23
election one-offs (`content_random_roundtrip`, `unlink_open_then_recreate`
`b''`, `concurrent_create_unlink_two_proc`) did not recur. New one-offs:
fcstor008 `names_near_path_max` and `content_random_overwrite_append`
EEXIST, `rename_file_over_symlink` EIO; fcstor010 `dir_many_files` EIO
at `f0149`; fcstor015 `dir_deep_nesting` ENOENT. The rest are the six
many-op timeouts. The XFS baseline on node9901 did not run
(`/data1/efs` is not a mount there); score the `efs-*.tsv` summaries,
not the compare files.
**9-host after the ghost + close-unlock clients (Sep 25 00:02 UTC,
`results/posix/20260925-0002-ghost`, servers still
`abc6e913e760-dirty`):** 191–197/201, skip=1, 0 not-run, 66–74 s.
`unlink_open_then_recreate` and `flock_unlock_on_close` passed on
every host. The empty read was the sharded `keep_last` path returning
only when the dentry table differed from the parent, so the common
case deleted the open-fd ghost and getattr adopted size 0. Last close
now also sends `LOCK_UN` for that fd's owner. Non-timeout leftovers:
fcstor007 and 009 `dir_many_files` EIO (`f0200`, `f0253`), fcstor010
`trunc_grow_sparse` FileExistsError on the test directory, fcstor011
`names_dash_prefix_terminal` EBUSY on the test directory. The rest
are the many-op timeouts.
**9-host after NET-retry + visible-dir mkdir (Sep 25 00:54 UTC,
`results/posix/20260925-0048-net`):** 194–197/201, skip=1, 0 not-run,
77–101 s. `dir_many_files`, `trunc_grow_sparse`, and
`names_dash_prefix_terminal` passed on every host, as did
`unlink_open_then_recreate` and `flock_unlock_on_close`. A dropped
conn or recv used to return `EFS_ERR_NET` on the first attempt
(create → EIO). It now stays inside the existing 16-attempt loop.
MKDIR `EEXIST`/`EBUSY` returns success only when a lookup sees a
directory, and no longer reports EEXIST when the name is not visible
(`fuse_create_errno` used to short-circuit on `last_err`).
**9-host after local directory lookup (Sep 25 02:50 UTC,
`results/posix/20260925-0130-lookup`):** 195–198/201, skip=1, 0
not-run, 63–69 s. `dir_deep_nesting_beyond_64` passed on all nine
hosts. With `entry_timeout=0` a depth-100 mkdir was one LOOKUP RPC
per ancestor per level. A directory this client already has is
answered from the local table; files still go to the server, so a
peer unlink stays visible. `dir_deep_nesting` (the same walk plus
`rmtree`) still timed out on 7 of 9 hosts. `names_crazy_dirs` timed
out on every host. `names_crazy_roundtrip`,
`concurrent_creates_same_dir`, and `concurrent_write_and_readdir`
timed out on some hosts only.
**9-host after write() stopped reporting inline (Sep 25 04:30 UTC,
`results/posix/20260925-0412-write`, servers+clients
`d2e593244a10-dirty`):** 194–198/201, skip=1, 0 not-run, 60–66 s.
`write()` used to `report_dirty(0)` the whole set before returning;
flush/close already does `report_dirty_ino(ino, 1)`, so the write
report was a second round trip on every small file. Best hosts
(007, 008, 011) fail only `dir_deep_nesting` and `names_crazy_dirs`.
Those two still time out on every host. `dir_deep_nesting_beyond_64`
slipped back to a timeout on some hosts after the roll.
Alone on fcstor007 (Sep 25 04:34 UTC, same build, jobs=16) both
pairs pass: `dir_deep_nesting` + `beyond_64` in 3.6 s,
`names_crazy_dirs` + `names_crazy_roundtrip` in 3.2 s. The 15 s
failure is the 9-host queue, not a bug in those tests.
**9-host after an immutable-segment block cache (Sep 25 12:35 UTC,
`results/posix/20260925-0445-segcache`, `6ba3592b03c2-dirty`):**
198–199/201, skip=1, 0 not-run, 48–56 s. `dir_deep_nesting` and
`dir_deep_nesting_beyond_64` passed on every host checked.
`names_crazy_dirs` still timed out on every host;
`names_crazy_roundtrip` on some. Each KV get was malloc + pread of
the block under the LSM lock, including a miss that walks every
segment. The segment is immutable, so the last block stays cached.
A miss used to install that block and evict the hot one. Misses
leave the cache alone, and a key past the segment's last key does
not read the last block again.
**9-host after that, plus no open-lease and no empty close REPORT
(Sep 25 16:49 UTC, `results/posix/20260925-1240-misscache`,
`59b312f5904b-dirty`):** 199–200/201, skip=1 (`mmap_write_read`),
0 not-run, 44–49 s. Seven hosts failed nothing. fcstor008 and
fcstor013 failed only `names_crazy_dirs` (15 s). `dir_deep_nesting`,
`dir_deep_nesting_beyond_64`, and `names_crazy_roundtrip` passed.
**Owed now:** `names_crazy_dirs` on the slow hosts. Then W13, W11,
W9, W10, which stop until ratified.
The idle 50 ms create+close median did not hold: Sep 24 15:03 UTC on
the live mount, term stable, `md_latency.py` was mkdir 5.9 /
create+close 4.1 / append+close 6.1 / stat 0.3 / unlink 1.6 / rmdir
6.2 (`results/measure/20260924-150154-rpcprof-shapes` agrees). One
50–110 ms sample per 20-op batch remains. Not a code change until
that median is back.
The I16 follow-up's server half is in tree and unit-tested
(`efs_session_fence_local` deletes that shard's op-id window for the
fenced epoch; `test_session` OK on node9901) and is **not rolled**: the
FUSE client never creates an efs session, so nothing fences it. Seeding
the op-id from the session waits on that client session, which is the
product-gap item, not a one-line change.

The I17 story (index-only ring match; `46d54e6`; two gate runs
`results/measure/20260922-122517-i17-leader-freeze`, `-122629-`) is in
`docs/project-history.md` (Sep 22). posix jobs=1 on `46d54e6`:
**200/0/1 SKIP in 92 s** (`results/posix/20260922-133653`); 9-host
**185–197 / 201, 0 NOTRUN, 46–66 s** (`results/posix/20260922-134008`,
a leader change 12 s in explains the low end). The six old `posix-*`
leftovers on 19810 stay half-applied (pre-fix rows, no repair tool) —
ignore or wipe at the next agreed `raft-mkfs`.

Cluster runs `46d54e679e4f-dirty` → rolling to **`43bdf6a41f7d`** with
`EFS_RAFT_OBS=1` (5 s `raft-obs:` lines in
`efsd.log`; keep it on until W8 closes). W8's gate chain passes except
the 9-host row: `tests/measure/w8_root_lat.sh` root mkdir med 10 ms max
0.063, no 1 s mode; `w8_root_mkdir.sh` 9/9 root + 9/9 sub; posix jobs=1
**200/201 + mmap SKIP in 37 s** (`results/posix/20260922-015106`);
`same_parent_storm.sh` PASS 9×4×100 (`results/stress/same-parent-20260922-015426`);
9-host suite **191–195 / 201 on every host, 0 NOTRUN, all nine done in
~62 s** (`results/posix/20260922-020950`, timeline
`results/measure/20260921-220933-w8-stall-timeline`). Idle
`md_latency.py` reference is now mkdir 6.1 / create 4.0 / append 6.2 /
stat 0.3 / unlink 1.5 / rmdir 5.2 ms (a run 20 s after a roll shows a
50 ms mode — that is the post-roll RDMA election churn, wait 2 min).

What is left in the 9-host row, and what to do with each:
1. **Half-applied cross-shard txn (I17)** — fixed `46d54e6` (ring match
   by `(index, term)`), gated Sep 22 (two freeze runs).
2. **Retry of a committed non-idempotent op (I16)** — `link_of_symlink`
   EEXIST on a never-used name, `concurrent_create_unlink_two_proc` EIO,
   `unlink_open_then_recreate` reading `b''`: a client got BUSY from the
   400 ms apply-wait deadline during a stall, retried LINK/UNLINK
   (`stale_retryable`), and met its own result. **Landed `43bdf6a`**
   (block above); gated on `7e29943` (the 20:26 9-host run above). The
   client still retries BUSY and the 400 ms deadline is unchanged. The
   `b''` read and the EIO one-offs recurred once, at the group-2
   election in that run.
3. **Compaction stall → leader loss (needs a decision, §4).** One pump
   cycle held `h->mu` for **2.4 s** on both g0 replicas at once
   (`obs-fcstor004/005.txt`: `apply_max=2464250us applies_in_worst=54`),
   the leader missed its heartbeats and g0 went 5299→5302→5303; the
   client saw a 2 s `stat` and two failed mkdirs in that window. The KV
   is 1 GiB per node in 64 MiB L1 segments; an L0 segment spans every
   shard, so `kv_compact_locked` merges **all** of L1 every 4 flushes
   (≈ 16 MiB of writes) under `l->mu` (every read on the node stalls
   too) and `h->mu`. It is deterministic, so every replica does it at
   the same moment. Options the spec does not choose between: bounded
   (leveled / per-key-range) compaction; compaction off the apply path
   (background thread, readers merge an immutable memtable); a larger
   memtable as a stopgap (fewer, not shorter, stalls). **Now queue item
   W13 (§1a) with a recommendation — background compactor — and steps;
   awaiting ratification.** Do not raise the election timeout.
4. **Throughput at 144 concurrent jobs** — the six many-op tests
   (`dir_deep_nesting*`, `names_crazy_*`, `concurrent_write_and_readdir`,
   `concurrent_creates_same_dir`, `mtime_monotonic_many_writes`) time out
   on most hosts: mkdir p50 21 ms / p90 88 ms under the suite, so a
   200-op test needs > 15 s. Steady-state apply is still ~0.3 ms per
   entry with no fsync in it (`apply_max=10657us applies_in_worst=36`):
   that is the KV reads in the apply path — one `pread` per segment per
   lookup/scan, 17 segments, no block cache. Measure before choosing
   (perf on the pump thread under the suite); a block cache is an
   implementation matter, a different segment layout is a decision.
Do not raise the per-test 15 s budget or the suite cap.

**Progress log (newest first — read this before the state below):**

- **22:15 (Sep 21)** — **9-host posix: 191–195/201 on all nine in 62 s
  (was 66–95 with `[None]` at the 385 s cap). Three causes, three fixes.**
  (1) *Harness:* the parallel path stamped the per-test clock at
  **submit** — 196 tests into 16 workers, so anything still queued after
  15 s was recorded "timeout" without running (3-op tests "timing out",
  the same first timeout on every host, ~100/201). The worker now stamps
  its own start (`a683def`); the budget is unchanged.
  (2) *`h->mu` contention:* with `read_mu` gone, 30+ handlers per pump
  cycle took the pump's own mutex to read commit/applied/leader and to
  cond_wait; the pump lost its heartbeat cadence and both groups
  re-elected 4× in 50 s under load
  (`results/measure/20260921-211829-w8-stall-timeline`). Handlers now
  read a lock-free view the pump publishes (`host_publish_view`,
  atomics) and sleep on `applied_cv` under a separate `cv_mu`; a handler
  takes `h->mu` only to propose or begin a read round (`4eb1419`).
  (3) *KV WAL fsync per applied entry:* 0.5 ms per apply on every
  replica under `h->mu` (`EFS_RAFT_OBS`). One `efs_kv_lsm_sync_hold` per
  pump cycle, released before `persist_applied` (`84a2a55`); spec
  §"Raft log is the durability boundary" says the KV owes nothing on
  the critical path. Under the suite: stat p50 3 ms (was 9), mkdir p50
  21 ms (was 63); idle mkdir 6.1 / create 4.0 / append 6.2 ms. What
  remains (§1b): retry-of-committed-op (I16 dedup), the 2.4 s
  full-L1 compaction stall that still costs a term, and per-op cost at
  144 jobs. New tools: `tests/measure/w8_stall_timeline.sh` (1 Hz
  raft term/commit + client op latency around the 9-host suite),
  `EFS_RAFT_OBS=1` on the deployed efsd.

- **17:37 (Sep 21)** — **The sweep itself was a 100 ms regression; fixed
  in `3291c6d`.** posix jobs=1 on `9534e53` passed 200/201 but took 219 s
  (56 s in the morning); jobs=16 put 77 tests over the 15 s budget;
  `md_latency.py`: mkdir med 99.5, create+close 152, append+close 202 ms
  (reference 7.2/6.7/9.0). The GC thread on each leader ran 512 shards ×
  3 prefix scans per second; strace: one scan = 9 `pread64` (one block
  per segment) ≈ 0.3 ms under the LSM mutex the apply path needs; perf:
  38 % `rep_movs` + 21 % memcmp, process CPU only 3 % — lock hold, not
  CPU. Now every PREPARE apply marks its shard, a 5 s-old mark gets one
  scan (64 per pass, 1 ms yield), clean clears it, a fresh process marks
  all 4096 once. After the roll: create 6.1–6.7, append 8.5–9.3, stat
  0.4, unlink 2.1, rmdir 7–10 ms medians; mkdir 9.9 (bimodal 2 ms log
  path / ~50 ms cross-group txn, so its median is the mix).
- **17:15 (Sep 21)** — **Recovery gates 0–3 pass on `9534e53`.** Within
  ~1 min of the roll the two leaders logged 47 `txn-recover` lines —
  every one `COMMIT (resolved)`: the coordinators HAD written COMMIT,
  only their RESOLVEs were lost, so the recovered ops (dentries, inodes,
  ALLOC bumps) are now visible ~90 min after their callers were told
  EBUSY. One forwarded RESOLVE came back BUSY once and succeeded the
  next pass. `kv_intents` on a fresh copy: **0** INTENT/GUARD/REDUCE
  (`results/measure/20260921-w8-orphans/after-recovery.txt`). Parent
  burst 12 rounds × 9 hosts: **108/108, 0 BUSY lines, max 0.31 s**
  (`results/measure/20260921-210917-w8-parent-burst`). Root lat
  (`…-211258-w8-root-lat`): root mkdir 16 ms (log path) or 50–57 ms
  (cross-group txn: 5 PREPARE + DECIDE + 2 RESOLVE ≈ 8 commits), max
  0.119, **no 1 s mode**; the "≤ 15 ms" I wrote for this gate was the
  log-path number, the txn path is ~50 ms by construction. Concurrent
  root mkdtemp (`…-211320-w8-root`): root 9/9 ≤ 0.137 s, fresh parent
  9/9 ≤ 0.311 s, every rmdir OK, no ENOENT.
- **17:10 (Sep 21)** — **Stranded transactions were the 10.4 s EBUSY.**
  `tests/measure/w8_parent_burst.sh` (9 hosts × 6 rounds of fresh-parent
  `mkdtemp`, `results/measure/20260921-204358-w8-parent-burst`): 53 OK,
  1 EBUSY after 10.39 s, and the new server line shows 16 identical
  `mkdir … rc=-13 (BUSY)` for it — one shard, every retry. A copy of
  node 2's `mdraft/kv` through `tests/tools/kv_intents`
  (`results/measure/20260921-w8-orphans`): **105 INTENT, 1 GUARD, 28
  REDUCE records, all 4 500–5 000 s old**, 45 of the intents on ALLOC
  keys of 45 even shards — a log-path create/mkdir landing on one of
  those shards is BUSY on every attempt. They date from the 1 s-scan era:
  a DECIDE or RESOLVE whose `host_wait_applied` hit 400 ms made the
  coordinator skip the rest, and nothing ever came back for the records
  (spec L5 says recovery must; there was none). `9534e53`:
  `efs_txn_scan_pending` + `host_txn_recover_pass` on the GC thread.

- **16:30 (Sep 21)** — **Root cause of the 1 s root mkdir: full-shard
  scan in RESOLVE.** Server strace during 12 root pairs
  (`results/measure/20260921-202253-w8-root-srv`): on the dual host 3–5
  threads sit in one futex wait and release together 0.70 s / 1.05 s
  later; on fcstor005 a futex wait ends `ETIMEDOUT` at exactly 0.400 s
  (the `host_wait_applied` deadline → BUSY); no disk syscall ≥ 0.15 s.
  perf on fcstor003+004 (`results/measure/20260921-202715-w8-root-perf`):
  **85–90 % of efsd CPU** is `host_pump → apply_committed → host_apply →
  efs_txn_resolve → lsm_scan_prefix → merge_scan → memcmp`.
  `efs_txn_resolve` and `efs_txn_drop` used a 2-byte prefix = every key
  of the shard, on every replica, per participant shard, under the KV
  lock and `h->mu`. Root's shard 1 carries the most history, so a
  cross-group child (txn path) cost ~1 s while a same-group child (log
  path, no RESOLVE) cost 2 ms — the bimodality. The 400 ms BUSY then
  drove the 10.4 s client backoff, and a retried UNLINK whose first
  attempt had committed came back NOT_FOUND = the rmdir ENOENT. Fix
  `165e779`: scan `[shard][INTENT|GUARD|REDUCE]` (3-byte prefixes).
  Unit tests `test_txn`/`test_meta_apply`/`test_sim` OK on fcstor003.
- **20:02 (Sep 21)** — **Root mkdir is 2 ms or 1.05 s; BUSY burns 10.4 s.**
  Root has **410** names, listed in 10 ms
  (`results/measure/20260921-195541-w8-root-trace`). Not the 65536
  spread. A strace of `efs-fuse` shows the 1.12 s call is one
  `recvfrom`; there is no client sleep on that path. Twenty sequential
  root mkdir+rmdir pairs
  (`results/measure/20260921-195829-w8-root-lat`): 13 at 2 ms, 7 at
  1.03–1.09 s, and the rmdir matches the mkdir. Interleaved with a
  fresh directory
  (`results/measure/20260921-200108-w8-root-interleave`): 6/16 root
  calls still ~1.03 s while the fresh-directory call in the same
  second is 2–15 ms. Two fresh-directory creates returned `EBUSY`
  after 10.38 s, which is all 16 BUSY retries. Raft tail, last 64 KiB
  (`results/measure/20260921-200251-w8-root-log`): group 0 PREPARE 58
  vs DECIDE 10. The 9-host warmup dies inside that backoff.
- **19:44 (Sep 21)** — **W8 root burst vs a fresh parent.**
  `results/measure/20260921-194332-w8-root`. One root `mkdtemp` 1.192 s.
  Nine at once: six `MKDIR_OK` in 1.19–5.96 s, three still out at 8 s
  (one python in `request_wait_answer`), and fcstor012/014 then `rmdir`
  ENOENT on the directory that same call had just created. Nine at once
  in a fresh subdirectory: **9/9 in 0.008–0.197 s**, every `rmdir` OK.
  Group 0 `commit == applied`, +55 entries in ~9 s. Root `stat` right
  after: nlink=406, `.stats` rollups all zero
  (`results/measure/20260921-194815-w8-root-stats`). Not a 65536-entry
  spread. The mount root is what the 9-host suite cannot enter.
- **19:40 (Sep 21)** — **W7 closed. W8 harness proven, 9-host gate not run.**
  Isolated walks are 1.1–6.1 s (`results/measure/20260921-133437-posix-isolated`);
  suite 1 jobs=1 stays 200/201. A 20 s cut of suite 1 writes 201 TSV rows
  (`results/measure/w8-cut.tsv`): 1 PASS, 200 `NOTRUN` ("suite cut by
  signal 15"), `NONE=0`. `compare.py` reports `EFS BUGS : 0` and
  `not run : 200` (`results/measure/w8-compare.txt`). Immediately after,
  9 clients each doing one `mkdtemp` in the mount root: **0 MKDIR_OK**.
  Five ssh timed out at 20 s (D-state `request_wait_answer` ignores the
  inner timeout); four were killed at 12 s (`rc=124`) with no success
  line. A same-binary remount cleared them. One mkdir on one client
  still returns. Do not start `run_tests.sh posix --parallel` until the
  root burst in `w8_root_mkdir.sh` returns.
- **18:23 (Sep 21)** — **W11 measured, still unspecified.**
  `results/measure/20260921-182308-raft-snap-state`: raft logs
  **1.83 / 3.56 / 4.36 / 1.89 GB** on fcstor003–006, still growing
  388–801 B/s on an idle cluster. KV 0.54–1.08 GB. `snapshot skipped`
  is latched (group 0 applied≈4521152, group 2 ≈3256233). Both groups
  `commit == applied` on every voter — fcstor005 is not behind on this
  build. A follower restart is still a full replay. Chunked
  InstallSnapshot stays unspecified: ask, do not design.
- **16:30 (Sep 21)** — **dd wall, 1 and 4 clients. 9-client number does not exist.**
  `results/measure/20260921-163033-dd-wall`, 8 GiB `dd conv=fsync` of
  non-zero `/tmp/src8g`, FUSE_OK, file 8589934592. **1 client 499 MiB/s**
  (wall 16.4 s; Sep 18 was 639). That REPORT: pack 3554 + push 4848 +
  finish 1013 ms, `rc=0`. **4 clients 176 MiB/s** aggregate (slowest
  185.8 s; Sep 18 was 251). 21 `report-split` lines, all `nrec=65536`
  `rc=0`, one push 149 s. 9 clients: every ssh hit 400 s; mkdir EIO on
  007/009/010 (the 30.2 s WALL is a failed open) and fsync EIO on
  011–015. Row is `INVALID`. Do not quote a 9-client rate from this run.
- **12:30 (Sep 21)** — **IOR-hard scaling, full runbook**
  (`tests/measure/ior_hard_scaling.sh`,
  `results/measure/20260921-162514-ior-hard-scaling`). Write MiB/s
  **372 / 33 / 69 / 82 at NP 1 / 4 / 9 / 36** (47008 B × 3000 segs, one
  file). 1-rank matches the own-file dd wall. From 4 ranks, ~half of
  `report-split` lines are `rc=-14` STALE and `finish_ms` (apply wait)
  is the large phase (37 s summed vs a 59 s IOR wall at 36 ranks).
  Not a monotone CAS cliff. Next: `dd_wall.sh` at 1/4/9 with `PERF=1`.
- **12:25 (Sep 21)** — **W6 same-directory rate, full runbook**
  (`PERF=1 tests/measure/samedir_rate.sh`,
  `results/measure/20260921-161931-samedir-rate`). Storm PASS at 1×1,
  1×9, 4×9. Aggregate **138 / 134 / 159 ops/s** — flat, so adding procs
  does not add throughput. `busy_n=0` rules out the BUSY backoff.
  `checkout_us` ~15 ms/client rules out the conn pool. Fuse `recv_us`
  ≈ storm wall rules in server+wire wait. Leader log tail is the storm's
  own PREPARE/CREATE/UNLINK/RESOLVE/LEASE_CLOSE/REAP_DONE (GC_ACK 0.4 %,
  so the earlier 52 % GC_ACK was an idle-tail artifact of the small
  smoke). On-CPU profile: `memcmp` in `lsm_scan_prefix` under
  `reduces_pending`, `guards_conflict`, `efs_txn_resolve`. Next runbook
  in order: `ior_hard_scaling.sh` at `NPS="1 4 9 36" SEGS=3000`
  (open-cost §2 is already answered by the smoke).
- **11:30 (Sep 21)** — **Runbooks for every open measurement item**
  ([runbooks.md](runbooks.md), `tests/measure/*.sh`, `tests/preflight.sh`),
  all pinned to the running build `b2184a5c7faf-dirty`, each smoke-run
  once (dirs under `results/measure/20260921-*`). Two findings from the
  smokes that change the questions: **(1) 1 GiB cold open is 0.23 s idle,
  not 20 s** — exactly 128 GETCHUNKS × 1.6 ms; with 32 concurrent openers
  GETCHUNKS is **14.6 ms** (9.5×) and open 1.6–2.5 s, one map fetch per
  host — so the IOR 20 s is server-side GETCHUNKS serialization under 36
  ranks, not per-open cost. **(2) Same-parent storm is 171 ops/s at
  1 proc AND at 4 procs with `busy_n=0`** — a flat aggregate ceiling that
  is not the BUSY backoff (leader raft log tail: 52 % `GC_ACK`). Also:
  `EFS_RPC_PROF=1` counters only dump on an RPC, ≤ every 2 s — read them
  after a trivial RPC (`rpc_prof_last` in `tests/measure/lib.sh`), and they
  do **not** count REPORT (`rpc_send_recv_dual` unprofiled) — the REPORT
  number is the server's `report-split` line, which the write runbooks now
  collect. **(3)** 1-client 8 GiB dd+fsync today 380–418 MiB/s (Sep 18:
  639); its single REPORT was `pack 6.9 s + push 5.9 s + finish 1.0 s` =
  **13.9 of the 21.5 s wall** — pack+push, not the apply wait, is the tail.
  Next: run the runbooks at their default (full) settings, one at a time —
  order in runbooks.md §1–7; then bring the tables to the user.
- **08:45 (Sep 21)** — **Step E DONE as option (b), §7.2 end state:** the
  parent inode row, the dseq emptiness witness and the HASHED dir-lane
  stamp are **commutative reductions**, not EXCL full-image CASes. New txn
  kinds `EFS_TXN_REDUCE_INO` (signed `nlink`/`nents` delta, times MAX,
  `used_shards` OR, `parent` SET, `parent_version` delta) and
  `EFS_TXN_REDUCE_ADD` (u64 +1); `fold_reduce` in `txn.c` dispatches on
  the DATA key's kind (LANE / INODE / DSEQ) and bumps the folded key's
  version so a stale EXCL lands STALE. Soundness rules: a pending reduce
  and an EXCL/GUARD on one key are mutually BUSY (`reduces_pending`);
  the log path probes before an unversioned DEL (`dir_txn_busy` in
  `efs_meta_apply_rmdir`, `efs_txn_key_busy` in `efs_meta_apply_unlink`)
  and returns BUSY; dseq GUARDs compare the observed **value**
  (`efs_txn_dseq_observe`) — a log-path bump is unversioned but changes
  the value, so `RMDIR`'s guard goes STALE exactly when a child appeared.
  One shared PREPARE decoder `efs_txn_apply_prepare` (server + sim).
  All of `host_hashed_create_txn`, mkdir, rmdir, `host_unlink_txn`, link,
  `rename_at` converted (`host_prep_ino_delta` / `host_prep_dseq_bump` /
  `host_prep_lane_stamp`); the simulator's `dseq_prep`/`dseq_guard` too.
  Client retries STALE for UNLINK/LINK/RENAME_AT as well as CREATE
  (`stale_retryable`). Unit gates: `test_txn` +4
  (`test_ino_delta_commutes` is the Sep 20 lost update: log-path PUT
  between PREPARE and RESOLVE, fold lands on the log-path result, ver+1;
  vs-EXCL/GUARD BUSY both ways; lane 56-byte tail kept; wire decode) and
  `test_meta_apply::test_log_delete_busy_under_intent`; `test_sim`,
  `test_kv`, `test_raft` green. Deployed stop-all/start-all (new
  `tests/roll_efsd.sh --all`) as `b2184a5c7faf-dirty` + all 9 clients
  (`fuse-deploy`). **posix jobs=1 `results/posix/20260921-123904`:
  200/201 + mmap SKIP in 56 s** (unchanged signature). **Repro
  `tests/stress/same_parent_storm.sh` PASS
  `results/stress/same-parent-20260921-124141/`:** 9 hosts × 4 procs ×
  100 rounds of mkdir/create/rmdir/unlink in ONE parent = 14 400 ops,
  0 errors, parent ends `children=0 nlink=2`, `rmdir` OK. Measured while
  there: **~178 ms per op per proc under 36-way same-parent contention
  (~200 ops/s aggregate)** vs 7 ms idle — the same ceiling as
  mdtest-easy-write 0.238 kIOPS. Not a correctness item; it goes in the
  W6 residuals as "same-directory op rate" (BUSY/STALE retries are not
  logged at default verbosity, so first instrument, then decide).
  **Step 5 (9×4 IO-500 debug) PASS on this build:
  `results/io500/20260921-debug-9x4-reduce/`** — both `-R` reads 0
  errors, **0 `Unable to remove directory`**, the run tree is gone
  afterwards (the previous run left 3 undeletable dirs). Rates within
  noise of the previous build (mdtest-easy-write 0.189 vs 0.238 kIOPS,
  hard-write 0.240 vs 0.201, ior-easy-read 1.13, hard-read 4.06 GiB/s;
  NOTE.txt has the table). Step 6 = this commit.
  `efs-bg.sh` now runs jobs in a detached **GNU screen `efs-<name>`** on
  node9901 (user's suggestion): `screen -r efs-<name>` there shows the
  live job; log/rc bookkeeping unchanged.
- **23:10 (Sep 20)** — **Step D (9×4 IO-500 debug) DONE and correct:**
  `results/io500/20260921-debug-9x4-outbox/` (run id 2026.09.20-22.45.42,
  cluster on the outbox-fix build). Both `-R` reads **0 errors**, every
  ior file unlinked. vs the morning gate (`20260920-debug-9x4`):
  mdtest-easy-write **0.238 kIOPS (was 0.050, 4.7×)**, mdtest-hard-write
  0.201 (was 0.018, 11×), mdtest-easy-stat 0.88 (0.24), mdtest-hard-stat
  2.76 (0.79), mdtest-easy-delete 0.29, mdtest-hard-delete 0.61;
  ior-easy-write 0.776 GiB/s (0.814, unchanged — W4 wall), ior-hard-write
  0.046 (0.044, unchanged — 36-way sub-chunk CAS), ior-easy-read 1.16
  (1.80; 25 s of the 37 s is the 1 GiB `open`, see residuals),
  ior-hard-read 3.89 (3.55).
  **New correctness finding — wedged directory, needs a user decision
  (§4):** 7 mdtest `WARNING: Unable to remove directory
  …/mdtest-easy/test-dir.0-0/mdtest_tree.N.0`. Afterwards 4 of them rmdir
  fine (transient STALE), **3 return EIO forever**: `raft-rmdir` →
  `status=3` (ERROR) because `server_raft_host_rmdir` /
  `efs_meta_apply_rmdir` hit `prow.nlink < 3 → EFS_ERR_PROTO`. The parent
  `test-dir.0-0` (ino 824) has **nlink=2 with 3 live subdirectories**
  (true value 5): 36 ranks did `mkdir` then `rmdir` of one child each in
  the same parent; children with an even ino go the same-group **log
  path** (`efs_meta_apply_rmdir`: PUT of the parent row, no intent probe,
  no version bump) and odd-ino children go the **txn path** (EXCL on the
  parent row at `pver`, PUT of a full row image with `nlink-1` from its
  read snapshot). A txn that read the row before a log-path apply still
  PREPAREs at the old version, wins, and overwrites the log-path
  decrement/increment — the exact class `alloc_key_claim` fixed for the
  ALLOC key on Sep 20 (`meta_apply.c` comment above it), now on the parent
  inode row. Every log-path parent-row PUT is exposed: `create_file_batch`,
  `mkdir_batch`, `efs_meta_apply_unlink/link/rename/rmdir`. Consequence:
  parent `nlink` drifts low → the last children can never be rmdir'ed
  (EIO), `rm -rf` of an mdtest tree fails; drifts high → a directory
  claims children it does not have. This is the "transient mdtest rmdir
  ENOTEMPTY" residual — it is not transient.
  Leftovers on 19810: `/io500/2026.09.20-22.45.42/mdtest-easy/test-dir.0-0/
  mdtest_tree.{3,18,27}.0` (parent 824 nlink=2). Ignore or wipe.
  Options for the user: **(a)** generalize `alloc_key_claim` to the parent
  row: log-path PUT is BUSY under a pending intent and bumps the row's
  version so the txn's EXCL goes STALE — same rule as ALLOC, mechanical,
  plus the client must retry STALE for MKDIR/RMDIR/UNLINK/LINK/RENAME (today
  only `INODE_CREATE`, `inode_rpc.c:299`), and the 50 ms × 2ⁿ BUSY backoff
  becomes the same-parent latency; **(b)** the spec'd §7.2 end state:
  parent nlink / dseq / mtime as commutative REDUCE parts (the existing
  `efs_txn_reduce` carries only max_end/max_mtime/max_ctime; needs a
  signed nlink delta and a REDUCE resolve onto an inode row), which makes
  same-parent ops conflict-free instead of retried. Not started either.
- **22:45 (Sep 20)** — **The 100 ms metadata floor is gone: mkdir med
  103 → 7.2 ms, create+1B+close 60–107 → 6.7, append+close 160–180 → 9.0**
  (`results/perf/20260921-md-latency.txt`). Step C below is DONE; the
  reaper was only half of it. The other half was a wakeup bug in the
  raft_host outbox: ONE shared `outbox_cv` for all per-peer sender threads
  + `pthread_cond_signal`, so queuing a message for peer A usually woke
  peer B's sender (empty queue, back to sleep) and A's message left at the
  next signal for anyone — the next 50 ms heartbeat. Each AE and each AE
  reply lost 0–50 ms per hop → ~100 ms per commit while every fsync was
  0.3 ms. Found by strace on a FOLLOWER (003): it acked only every other
  heartbeat, two 85-byte `writev` back to back; on the leader the AE to
  one peer left 47 ms after the propose. Fix: per-peer `tx[].cv`
  (`struct host_outbox`), `host_send` signals exactly that one, shutdown
  broadcasts all. Rolled all four (`roll3`, 90 s, TCP peers,
  `EFSD_ENV=EFS_TRANSPORT=tcp`); posix jobs=1 running as `posix3`. Not
  committed yet (step A below now includes this).
- **21:55 (Sep 20)** — Steps 1–4 DONE again on the full in-flight tree
  (now also the `concurrent_appends` dcache fixes, the dir-rename dual-host
  bounce, and a reaper fix found tonight); step 5 (9×4 IO-500) NOT run;
  nothing committed yet. Everything long-running now runs **from node9901
  via `efs-bg.sh`** (see the `efs-test-ssh` skill; logs in `~/efs-runs/`)
  because the login-node shell died four times today — the fifth time was
  mid-doc-edit at 21:50, which is why this entry exists.
  - Posix jobs=1 twice: `results/posix/20260921-011518` (190 + mmap SKIP,
    but 8 clients were being rebuilt/remounted during it) and
    `results/posix/20260921-012432` (**191 both-pass + mmap SKIP**, fails
    = `dir_deep_nesting`, `dir_deep_nesting_beyond_64`, `names_crazy_dirs`
    15 s walks + `mtime_monotonic_many_writes` 15 s). `concurrent_appends`,
    `dir_rename_dir_with_contents`, `trunc_zero_then_high_pwrite` PASS.
    `mtime_monotonic_many_writes` is not a flake: 80 × (open O_APPEND,
    write 1 B, close) at **~200 ms each** > 15 s. Isolated timing on 007
    (idle cluster): mkdir med **103 ms** (min 5.5), create+1B+close 60–107,
    append+close 160–180, `report_ms=104` on every 1-rec REPORT,
    server `report-split ... finish_ms=103`.
  - **Root cause of the ~100 ms floor (fixed in tree, deployed, gate
    pending):** the reaper. `tests/tools/raft_log_tail.py` on 004's
    `raft.log` showed **92 % of both groups' entries were `LANE_SWEEP`**
    for the same 64 inodes, 362× each — ~33 entries/s per group with no
    client. `host_gc_propose` was leader-only, but a lane's shard is
    `ish + lane × odd stride`, so every ODD lane of an inode lives in the
    OTHER group: the anchor-group leader swept lane 0 (landed), got
    `NOT_PRIMARY` on lane 1, and retried the whole marker next second,
    forever — unless one node happened to lead both groups. Fix:
    `host_gc_propose` forwards a foreign-group command to that group's
    leader (`host_remote_cmd`, same path as a client op) and waits for the
    local replica if hosted. No unit test covers `raft_host`; the gate is
    the log histogram going quiet + the latency numbers above dropping.
  - Rolled all four (`tests/roll_efsd.sh 1 2 3 4` from node9901, 01:48–
    01:50 UTC, each caught up in < 10 s, all `d0fd0448adb6-dirty`). After
    the roll `REAP_DONE` finally lands (0.3 % → 40 % of entries): the
    reaper is **draining a backlog of dead inodes at ~15/s per group**
    (thousands from IO-500/mdtest/posix). Until it is idle every client
    commit queues behind it (mkdir med still 150 ms at 21:55). **Wait for
    both groups' `commit` to be flat for 30 s before measuring anything**,
    then redo the isolated latency probe — the min of 5.5 ms says what a
    raft commit costs; if the median stays > 20 ms on an idle cluster that
    is the next serialization point (each apply fsyncs the KV WAL inside
    the single pump thread, `raft_host.c:4245 EFS_KV_LSM_SYNC`) and needs
    a user decision, not a tweak.
  - Rolling restarts cause a **group-2 election storm** (term 199 → 264)
    lasting ~5 min after the last restart: the server peer pool is RDMA
    (`peer_pool.c` upgrades every peer conn; raft AE rides it) and a
    restarted node's QPs die with `transport retry counter exceeded` /
    `*** SOCKET CLOSED/REUSED BEHIND THIS CONN ***` on its peers, each
    blocking a sender for seconds → missed heartbeats. Pre-existing (same
    lines in every `efsd.log.prev`), converges by itself. Same class as
    the client pool identity bug (`test_conn_fd`) — not chased tonight.
  - New tooling, all committed with this batch: `efs-bg.sh` (detached
    runner on node9901), `tests/deploy_fuse_clients.sh` +
    `tests/fuse_client_remount.sh` (parallel client rebuild+remount, one
    status line per host), `tests/roll_efsd.sh` (rolling restart with
    build-ID check and per-group catch-up wait), `tests/tools/raft_log_tail.py`
    (what is the log made of). The deploy rule's kill command is now
    `pkill -9 -x efsd` only.
- **12:05** — Steps 1–4 DONE, step 5 not started; the agent's shell died
  again while probing the one new posix failure. Results:
  - Churn (step 1) = the GC reaper draining the fragment backlog the
    `lane-sweep rc=-5` bug left (no client process; `df` on every
    `/data1/0N` falls ~150 KB/s). Benign; let it run.
  - Rolling restart (step 2) done 11:33–11:45 in the order 004, 005, 006,
    003; each caught up within 9 s; all four run `d0fd0448adb6-dirty`.
  - **Step 3 PASSED:** raw IOR hard 36×3000 write **96.78 MiB/s** (was
    ~45), cold `-r -R` verify **0 errors**, `/tmp/hardcheck.py` from
    fcstor009 **bad records 0 of 108000** (was 6707). W1 `n1_shared_pwrite`
    **lost=0**. All in `results/io500/20260920-hardv-pubbatch/gate.txt`.
  - Step 4 posix jobs=1 `results/posix/20260920-155107`: **192 both-pass +
    mmap SKIP**, 3 EFS fails. Isolated re-run: `dir_deep_nesting` PASS,
    `mtime_monotonic_many_writes` PASS (load flakes, known), but
    **`dir_rename_dir_with_contents` FAILS ISOLATED** (deterministic
    `EIO` on `os.rename(d/src, d/dst)` where `src/sub/f` exists; 1.6 s).
    It was PASS in the Sep 17 `results/posix/20260917-190719` gate, so
    this is a regression in the in-flight tree or a state issue on this
    populated cluster. `ino_dup=0`, `lane-sweep rc=-5`=0 on all four
    servers. **Do this before step 5:** reproduce by hand on fcstor007
    (`mkdir -p $d/src/sub; echo x >$d/src/sub/f; mv $d/src $d/dst`), vary
    it (empty dir; dir with a file only; dir with an empty subdir) to see
    which shape EIOs, and read the RENAME path in
    `src/server/raft_host.c` (`server_raft_host_rename_at`, LOCAL same-dir
    directory rename GUARDs dest ancestry `parent_version`) plus the
    efsd.log of the parent's group leader for the txn status. Candidate:
    the `alloc_key_claim` version bump or `dentry_seq` GUARD racing the
    child's `nents` update — the test creates `sub` and `f` immediately
    before the rename.

**State as of Sep 20 10:40 (verified by probe, not memory; the agent's
shell died mid-rolling-restart, which is why this block exists):**

- **The bug being fixed: a REPORT that spans several Raft entries returned
  OK when only its LAST entry applied OK.** `host_pub_batch_wait`
  (`src/server/raft_host.c`) read the apply-verdict ring for `last_idx`
  only; a report larger than `HOST_PUB_BATCH_N` (256) pubs is proposed as
  several entries, and a pub that loses its N-1 CAS at apply time is
  `STALE` on the ring for *its* entry only. The client took the OK, marked
  those dirty ranges clean, and the CAS winner's generation (merged on an
  older base) became the file. Measured: raw IOR hard, 36 ranks × 3000
  segments of 47008 B (`SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write
  3000`, then cold-remount `ior-hard-verify 3000`) lost **6707 of 108000
  records**, every one a client's whole sub-range of a record straddling a
  128 KiB chunk boundary, zeros in a cold verify (classifier:
  `/tmp/hardcheck.py <file> 36` on fcstor007 — recreate from the comment
  in that file if it is gone). The 9×4 debug run
  `results/io500/20260920-debug-9x4/` saw the same thing as 2 `-R` errors
  on ior-hard-read (easy-read 0). This is a **correctness** bug and blocks
  every other W6 item.
- Uncommitted code in the working tree (on top of `cc828d8` + the Sep 20
  docs/rules commit), all unit-gated on fcstor003 (`test_raft` /
  `test_meta_apply` / `test_kv_lsm` OK):
  - `src/server/raft_host.c` — `struct host_pub_batch` keeps every
    proposed index (`idxs[]`, `overflow`); `host_pub_batch_wait` waits for
    the last index then reads **every** entry's verdict; any `STALE`, any
    ring miss (`obs_arc_miss`), or `overflow` → the report is `STALE`
    (client re-pulls the map and replays only chunks whose generation
    moved). `host_pub_batch_reset` frees both arrays. No unit test covers
    `raft_host` (live-only); the gate is step 3 below.
  - `src/meta/meta_apply.c` — `alloc_key_claim` (log-path CREATE/MKDIR
    alloc is `BUSY` under a txn `EXCL` intent on the shard ALLOC key and
    bumps its version; closes the `ino_dup` 4746 race; gate
    `test_alloc_vs_txn_intent`); `sweep_cb` / `rsv_purge_cb` initialise
    `val=NULL vlen=0` on `EFS_KV_DEL` items (uninitialised `vlen` made
    `wal_encode` return `INVAL` → reaper `apply lane-sweep rc=-5` forever,
    124 inos re-swept ~9 entries/s).
  - `src/kv/kv_wal.c` — `wal_encode` validates `vlen` only for `PUT`;
    regression in `tests/test_kv_lsm.c` (DEL with garbage `val/vlen`).
  - `src/client/read.c`, `src/client/ops.c`, `src/client/client_internal.h`
    — a read whose layout pull fails **fails** instead of zero-filling;
    `pull_chunks_range` returns the first RPC error; `pull_layout_miss` has
    a 200 ms range cache, not a 1/s rate limit. Built into `efs-fuse` on
    fcstor007–015 (all nine were remounted from this tree before the last
    IO-500 run).
- **Servers:** `efsd` built from this tree at `/tmp/efs` on fcstor003–006
  (`d0fd0448adb6`-dirty, 4704656 B, 10:29). **fcstor004 (node 2) was
  restarted on it at ~10:33 and caught up** (its `applied` equalled the
  leaders' `commit` on both groups at every 3 s poll for 2 min). The HELLO
  gate accepted it, so the build ID is unchanged and a **rolling** restart
  is fine. **fcstor003 (node 1, g0 leader), fcstor005 (node 3), fcstor006
  (node 4, g2 leader) still run the PREVIOUS process** (started ~09:20,
  without the `host_pub_batch` fix). Nothing was wiped; no `--join`.
- **Unexplained, check first:** with no test running, both Raft groups
  were advancing ~33 entries/s (g0 commit 3259035 → 3333442 in ~7 min).
  Candidates: a leftover IOR/posix process on a client, or the reaper on
  the old-binary leaders. Do not restart anything until you know which.

**Steps (as of 22:45 Sep 20 — steps 1–4, B and C are DONE; what is left):**

- **A. DONE — commit `30c41ee`** (W6 correctness batch + the 100 ms
  floor). posix jobs=1 after the outbox fix: **200/201 both-pass + mmap
  SKIP in 60 s** (`results/posix/20260921-024204`; the four 15 s walk
  timeouts are gone). **The cluster still runs `d0fd0448adb6-dirty`**
  (byte-identical source to `30c41ee`, built before the commit); the next
  server restart must be stop-all-four / start-all-four, `roll_efsd.sh`
  will refuse. Clients likewise run the dirty build — fine until the next
  `deploy_fuse_clients.sh`.
- **B. DONE** — reaper drained, both groups flat; `raft_log_tail.py` on 004
  no longer dominated by `LANE_SWEEP`. Keep the check as a habit before
  any measurement.
- **C. DONE** — `results/perf/20260921-md-latency.txt`: medians 6–9 ms.
  The pump's per-apply KV WAL fsync is NOT a serialization point at this
  load (0.3 ms per fsync on NVMe); no `sync_mode` question for the user.
- **D. DONE** — `results/io500/20260921-debug-9x4-outbox/`, 0 read errors,
  all unlinks OK, mdtest 4.7–11× (see the 23:10 entry). Then step 6.
- **E. DONE (user chose (b), 08:45 Sep 21 entry)** — §7.2 reductions on
  the parent row / dseq / dir lane; repro is now a script,
  `tests/stress/same_parent_storm.sh` (PASS, 14 400 ops, parent clean).
  Any directory whose `stat` nlink ≠ 2 + subdir count after this build is
  a NEW bug, not this one. Step 5 (9×4 IO-500) PASS on it
  (`results/io500/20260921-debug-9x4-reduce/`); step 6 = the §7.2 commit
  (`git log -1 --grep="commutative reductions"`). **Nothing is in flight after that commit** — take the lowest
  open item in §1a (W6 residuals first). The cluster and the 9 clients run
  `b2184a5c7faf-dirty` = the same source as that commit; the next server
  restart is a build-ID change → `tests/roll_efsd.sh --all`.

Original steps (1–4 done twice, kept for the commands):

1. Pre-flight (deploy rule). Then the churn check: on fcstor007–015
   `pgrep -x io500; pgrep -x ior; pgrep -f posix_suite` (kill leftovers
   with `pkill -9 -x`, never `-f` on an ssh command line); on fcstor003 and
   fcstor006 `tail -c 400000 /tmp/efs/efsd.log | grep -oE 'raft-host: [a-z-]+ [a-z-]+' | sort | uniq -c | sort -rn | head`.
   If it is the reaper (`lane-sweep`), the restart below stops it; if it is
   a client, kill it and re-check `commit` is flat for 30 s.
2. Rolling restart: `efs-bg.sh start roll 'bash tests/roll_efsd.sh 1 2 3 4'`
   from the login node (runs on node9901; builds on each node, `pkill -9
   -x efsd`, starts, waits for per-group catch-up, refuses on a build-ID
   change). ~2 min for four nodes. Expect a few minutes of group-2
   elections afterwards (RDMA peer conns re-forming) — wait for
   `leader != -1` and a stable `term` on all voters.
3. **The gate for the fix:** on fcstor007
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-write 3000`, wait for
   `last-run.log` to finish, remount every client (deploy rule "Restart one
   efs-fuse" — cold verify or it is dcache), then
   `SLOTS=4 bash tests/perf/io500/run.sh ior-hard-verify 3000`. Pass =
   **0** `-R` mismatches; also run `/tmp/hardcheck.py` and expect `bad
   records 0`. Expect hard-write MiB/s to DROP (the STALEs the old code
   swallowed are now retried) — record it, do not tune it here. Also the
   W1 gate: `tests/stress/n1_shared_pwrite.py` `prepare` on 007, then
   `write-a` on 007 ∥ `write-b` on 008, remount 009, `verify` on 009 →
   `lost=0` (shape and expected output: `results/stress/20260918-n1-w1/gate10c.txt`).
4. `EFS_TRANSPORT=tcp POSIX_JOBS=1 bash tests/run_tests.sh posix fcstor007.ib`.
   Gate: ≥ **195/201**, only the known signature (`dir_deep_nesting*` /
   `dir_many_files` / `names_crazy_dirs` 15 s walks, `mmap_write_read`
   SKIP, `concurrent_writes_disjoint` flake); no `ino_dup` in any
   efsd.log; no `apply lane-sweep rc=-5` on any leader after the restart.
5. 9×4 IO-500 debug once (`SLOTS=4 NP=36 bash tests/perf/io500/run.sh
   debug`, detached driver; poll `driver.log`). Gate: every phase
   finishes, ior-easy-read **and** ior-hard-read `-R` errors = 0, every
   mdtest unlink OK. Copy `result.txt` + ini + `driver.log` to
   `results/io500/<id>/`.
6. Commit code + rules + this file with the result directories cited, then
   delete this block (W6 residuals move to the W6 queue item).

---

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

**What the hardware allows.** Every performance item is measured against
this, not against last week's number. Cluster traffic rides `ibs1f0`
(200 Gb/s IPoIB, `ip route get 172.16.223.57` on a client); servers have six
NVMe each.

| ceiling | value | derivation |
| --- | --- | --- |
| one client, logical write | **~16.7 GB/s** | 25 GB/s line rate ÷ 1.5 (2+1 EC sends 3 fragments per 2 data) |
| one client, logical read | ~25 GB/s | line rate; a read fetches k=2 fragments |
| cluster, logical write | **~44–57 GB/s** | 4 hosts × 16.7–21.4 GB/s NVMe (`results/nvme/`) ÷ 1.5 |
| 1-client honest write today | 639–724 MiB/s | **3.8–4.1 %** of the client's ceiling (8 GiB dd+fsync after W3) |
| 4-client honest write today | 251 MiB/s | **0.46 %** of 44 GB/s (8 GiB dd+fsync, 4 own files; 0.41× one client) |
| 9-client honest write today | 202 MiB/s | **0.37 %** of 44 GB/s (8 GiB dd+fsync, 5 first-write clients; 0.32× one client) |
| 1-client honest read today | 4102 MiB/s | ~16 % of the client's ceiling (sr-1m, W4 morning) |

[architecture.md §1](../architecture.md) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is currently a bug, not a tuning task, and
"done" for W3–W5 is defined by the table above, not by beating the previous
run.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`):

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync` | **639 MiB/s** (13.5 s; best 724 / 11.86 s) | `results/perf/20260918-w3-split/gate.txt` |
| 1-client honest fio 9×2g sw-1m | **758 MiB/s** | `results/perf/20260918-w1-honest/` |
| 1-client honest fio 1-job 50g | **341 MiB/s** | `results/perf/20260918-w4-honest/` (W1 was 373; loaded reruns EIO on fsync) |
| prior 1-client 9×2g sw-1m | 924 MiB/s | `hot-sw-1m-9job-fcstor007.txt` |
| first honest matrix, 1 client | sw-1m 209 · … · sr-1m 3141 | `results/perf/20260917-honest/` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 9×4 debug | easy-write **0.814 GiB/s**, hard-write 0.044, reads 0 errors | `results/io500/20260920-debug-9x4/` (W6) |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

#### Decisions pending — recommended answers (Sep 23 2026)

Four queue items stop on a choice the spec does not make. Each item below
carries a **Recommendation**, the reason, and the steps that follow from
it. Nothing here is implemented until the user ratifies the row. The
order is the build order: W13 and W11 share one primitive.

| item | question | recommended | why, in one line |
| --- | --- | --- | --- |
| W13 | full-L1 compaction holds the apply path 2.4 s and costs a term | **compact on a background thread**; per-range compaction later; memtable size is not a fix | it is a lock hold, not CPU — moving it off `l->mu`/`h->mu` removes the stall at any table size; per-range does nothing while one L0 spans every shard; a bigger memtable makes the same stall rarer, and one stall is one election |
| W11 | the KV export is larger than one 4 MiB SNAP command, so the log never truncates | **the Raft paper's chunked InstallSnapshot** (`offset`, `done`) over a snapshot *file* the leader exports from a pinned segment view | it is the protocol Raft already specifies, so nothing is invented; the pinned view is the same primitive W13 needs; the follower imports on `done` with the existing `efs_kv_group_import` |
| W9 | bound the client staging table | **ratify the pin rules and the 256 MB default** (`EFS_CLIENT_META_MB`), soft cap | 256 MB is ~1M rows, far above any FUSE working set; the pin rules are the only ones that make a report-record miss impossible; a pinned-full table grows and logs rather than losing a write |
| W10 | RDMA needs an empty table to gate | **no wipe.** Gate on the private 3-node cluster (`tests/rdma_first_inode.sh`, fcstor007, port 19950), then switch 19810 to RDMA in place | the bug only needs an *empty* table, and the private cluster is one; a populated 19810 already ran millions of RDMA creates clean, so the live switch needs no wipe. Keep the wipe for when W11's gate wants a table that grows from zero |

---

#### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE

**Done Sep 18 2026** (working tree on leftover-1 19810, TCP). I12 CAS is
live: `efs_chunk_rec.base_gen` + `chunk_generation` on the wire, leader
checks the writer's base before propose, apply STALE is audible
(`EFS_INODE_RPC_STALE=11` / `EFS_ERR_STALE=-14`) and does **not** stall
`last_applied`, client refetch+overlay+PUT retries. Fragments are
`{ci}.{fi}.{gen}`; GET uses `fragment_path_at` and mints the export shell.
Report identity comes from the last PUT (`putid`), not a GETCHUNKS stub.

`peer_shared_pwrite` is concurrent (`("a", a0), ("ab", (a, b)), ("a", a2)`).
STALE bound is **64** with 2–20 ms backoff — 8 loses to 16 fsyncs/chunk.
`EFS_CHUNK_BASE_UNCOND` (`UINT64_MAX`) is only for a full-chunk overwrite.

- **Gate:** `results/stress/20260918-n1-w1/` — n1-w1-gate10c
  `lost=0 decode_eio=0`; isolated `peer_shared_pwrite` **5/5**;
  `test_meta_apply` / `test_wire` OK (`test_publish_stale_then_retry`).
  Suite 2 one pair `results/posix2/20260918-122419/` **60/3** (was 58/4).
  Suite 1 jobs=1 `results/posix/20260918-123250/` 186/10 — every new FAIL
  isolated PASS except known `concurrent_appends`. Honest 1-client
  `results/perf/20260918-w1-honest/`: sw-1m **758** (not a regression vs
  209); sw-50g **373** (was NET). `FIO_ONLY=1` skips the 4/9 sweeps.
- **Forbidden to reopen:** a distributed chunk lock; per-record report
  status arrays; sending `UINT64_MAX` from any path that read a base;
  wipe / `raft-mkfs` / inventing chunked SNAP.

#### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE

**Done Sep 18 2026**, option (i): the spec moved. [architecture.md §3](../architecture.md)
now lists three deviations; a returned `write()` is client-buffered;
durable + cross-client visible at `fsync` / last `close` / `O_SYNC`.
`O_SYNC`/`O_DSYNC`/`-o sync` is specified write-through and is **not
wired**. Do not implement publish-on-every-`write()` — that is the
rejected 10× throughput change.

Measured (`results/stress/20260918-w2/`): peer sees **0/10** of an
un-`fsync`ed 4 KiB `pwrite`; `kill -9` of `efs-fuse` loses 64 MiB of an
acknowledged `write()` (file exists, size=0).

- **Forbidden:** implementing option (ii) publish-on-write; editing §3
  back to "`write()` is durable"; wiring `O_SYNC` as a silent side-cut
  of a later item.

#### W3 — Split the single-client fsync tail, then remove the larger half — DONE

**Done Sep 18 2026** (leftover-1 19810, TCP). Measure said REPORT owned
73 % of the 8 GiB `fsync` (flush 5.7 s / report 15.0 s / wall 25.8 s).
Cuts, in order: `HOST_PUB_BATCH_N=2048` + skip `get_chunk` when
`base_gen==0`; raft-log `sync_hold` across the report's proposes;
client flush pipeline (`dcache_steal` + put pool); PUBLISH follower
forward is `host_propose` not `host_propose_wait`; O_APPEND fetches a
published mapping before sparse RMW; truncate/symlink report only that
ino; FUSE `flush` waits `report_dirty_ino(ino, 1)` (W2 close is
durable — kick-only left `i_size` 0 so the next O_APPEND wiped the
prefix). N=4096 did not win. AE 1 MiB is ~5168 pubs; deeper batching
is exhausted.

- **Gate:** `results/perf/20260918-w3-split/gate.txt`. 8 GiB
  `dd+fsync` **13.501 s / 639 MB/s** (best cut 11.862 s / 724 MB/s);
  remount `HEAD_OK` `TAIL_OK`. **3.8–4.1 %** of 16.7 GB/s (was 1.9 %
  at 317 MiB/s). posix jobs=1 `results/posix/20260918-w3f/` **195/201**
  (190 both-pass; W1 was 186/10). posix2 one pair
  `results/posix2/20260918-w3f/` **58/63** (W1 60/3). Remaining
  suite fails are load / known O_APPEND atomicity / 15 s walks, all
  isolated PASS except `concurrent_appends`. Remaining tail is REPORT
  pack+push (~6 s).
- **Forbidden to reopen:** a REPORT split into multiple RPCs;
  weakening `fsync`; async flush to inflate the number; raising
  `EFS_IO_TIMEOUT_MS`.

#### W4 — 4-client and 9-client honest fio and dd — DONE

**Done Sep 18 2026.** Writes share a ceiling and more clients make it
worse. Gate: `results/perf/20260918-w4-honest4/gate.txt`.

- 1-client morning matrix `results/perf/20260918-w4-honest/`: sw-1m
  **694**, sr-1m **4102**, sw-50g **341** (W5). All `FUSE_OK` `err=0`.
- 4-client fio (same dir, morning): sw-1m AGG **1589** (2.3×), ow-1m
  636, rw-1m 343. After bounce/grown table: sw-1m 281, ow-1m 240, then
  rw-1m **400 s ssh TIMEOUT** on all 4 (REPORT tail). Do not raise it.
  4/9-client fio reads and 9-client fio writes were not finished.
- 8 GiB `dd+fsync` own file, remount HEAD/TAIL `0x5a` OK: 4-client
  **251 MiB/s** (32 GiB / 130.5 s = 0.41× one client, 0.46 % of 44 GB/s);
  9-client first-write 011–015 **202 MiB/s** (40 GiB / 203 s = 0.32× one
  client). Do not quote 007–010's 13–31 s 9-client walls — those files
  kept the 4-client mtime.
- A 4-client fio storm can lose raft heartbeats (`report-split`
  `rc=-15` NOT_PRIMARY). Recovery is keep-storage efsd bounce, not wipe.

W3's leftover (REPORT pack+push) is the multi-client wall. The 1-client
limit was not per-client CPU.

- **Forbidden to reopen:** a REPORT split into multiple RPCs; quoting a
  run where any host failed the FUSE check; one shared file (W1);
  raising `EFS_IO_TIMEOUT_MS`.

#### W5 — Re-measure `sw-50g` after W3 — DONE

**Done Sep 18 2026** as the W4 morning 50g row:
`results/perf/20260918-w4-honest/` sw-50g **341** / sr-50g **2203**,
`FUSE_OK` `err=0`. W1 was 373. Later loaded reruns laid 50 GiB then
`end_fsync` EIO (the same REPORT tail). Completes when the path is
healthy; do not treat EIO as a reason to raise `EFS_IO_TIMEOUT_MS`.

- **Forbidden to reopen:** raising `EFS_IO_TIMEOUT_MS`; splitting REPORT
  without asking.

#### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) — CORRECTNESS DONE, perf residuals open

**Correctness gate met Sep 20 2026** (commit `cc828d8`, servers
`708b350`+, TCP, 9 clients × 4 ranks): 9×4 debug
`results/io500/20260920-debug-9x4/` — **every phase finished, ior-easy-read
and ior-hard-read 0 verification errors, every unlink OK**. The path from
the Sep 18 numbers (hard-write DNF in 2 h 18 m, `-W` 4244 errors, 76108
easy-read errors, 27 undeletable files) to this is in
[../project-history.md](../project-history.md) "W6"; the fixes were: client
STALE retry cost + server partial-commit on STALE (hard-write livelock);
`dcache_image_current` / `snap_seq` ordering / forwarded-cmd reply index
(read coherency, `concurrent_appends`); Raft follower dedupe + leader AE
flow control (005 catch-up); **duplicate ino on concurrent CREATE**
(`alloc_hint_or_next` — apply is the allocator); reaper `lane-sweep`
batch-full misread as error (no file >8 MiB/lane was ever reclaimed).

| phase | Sep 20 9×4 | Sep 18 9×1 |
| --- | --- | --- |
| ior-easy-write | **0.814 GiB/s** | 0.263 |
| ior-hard-write | 0.044 GiB/s (495 s) | 0.025 (9×4: DNF) |
| ior-easy-read | 1.80 GiB/s, 0 errors | 0.60 (`-R` 512 errors) |
| ior-hard-read | 3.55 GiB/s, 0 errors | 1.26 (`-R` 15982 errors) |
| mdtest-easy-write | 0.050 kIOPS | 0.053 |

Not a list submission (stonewall 1 s). Do not quote the Sep 19 easy-read
4.0 GiB/s — it was zero-fill. Do not quote stonewall intra GiB/s.

**Open under this item (performance, not correctness):**

1. **ior-hard-write** — measured
   `results/measure/20260921-162514-ior-hard-scaling` (47008 B, 3000 segs,
   one file): **372 / 33 / 69 / 82 MiB/s at 1 / 4 / 9 / 36 ranks**. The
   1-rank number matches today's own-file dd (~380). From 4 ranks up,
   about half of the logged REPORTs return `EFS_ERR_STALE` (-14) and
   `finish_ms` (wait for apply) is the bulk of server time (37 s summed
   at 36 ranks, IOR wall 59 s). Throughput rises 4→36 rather than falling,
   so it is shared-chunk CAS replay plus apply wait, not a cliff that
   gets worse without bound. Spec answer remains §9 immutable delta
   objects — a user decision; do not add a chunk lock.
2. **1 GiB open costs 20 s of a 22 s easy-read** — 128 sequential
   GETCHUNKS + a 64-lane stat per open. Spec §8 per-lane range fetch
   ([performance.md](performance.md)) is the fix; not implemented.
3. ~~mdtest `rmdir` ENOTEMPTY/EIO under load~~ — **FIXED Sep 21** (it was
   the parent-row lost update, not transient; §7.2 reductions). **Rate
   measured** `results/measure/20260921-161931-samedir-rate` (ROUNDS=100,
   storm PASS at every level): **138 / 134 / 159 ops/s aggregate at 1 / 9 /
   36 procs** — a flat ceiling, 7.3 → 226 ms/op/proc. `busy_n=0` (the
   50 ms BUSY backoff is not it). Each client's fuse daemon spends the
   wall in RPC recv (`recv_us` ≈ the storm wall, `checkout_us` ~15 ms).
   The group-0 leader's log tail during the storm is the storm itself
   (PREPARE 31 %, CREATE/UNLINK/RMDIR, RESOLVE, LEASE_CLOSE, REAP_DONE);
   GC_ACK is 0.4 %. On-CPU samples are LSM prefix scans inside
   `reduces_pending` / `guards_conflict` / `efs_txn_resolve`. Do not
   change the backoff or the txn protocol; the remaining question (LSM
   scan vs Raft fsync, which cpu-clock cannot separate) is the user's.

Harness (`tests/perf/io500/run.sh`): `SLOTS=4 NP=36 … debug` detaches
`prterun` and logs to `$IO500_DIR/last-run.log` (the ssh timeout used to
kill it); `ior-easy-write|verify <mb>` and `ior-hard-write|verify <segs>`
run IOR directly so a cold verify is possible (the io500 driver deletes
its data at the end of a run).

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate (met):** 9×4 debug with 0 `-R` errors on both reads. For the
  three open sub-items the gate is the number in the table moving with
  the same harness and 0 errors kept.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); a chunk lock (W1); `pkill -f`
  (matches the agent). Kill hung `io500` with `pkill -9 -x io500` then
  remount FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies). Never gdb-attach an MPI rank through a timeout'd ssh
  (left a rank T-stopped, job unrecoverable).

#### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE

**Done Sep 21 2026.** The 15 s failures were the metadata wakeup floor
(fixed Sep 20), not a remaining per-test bug. Isolated on an idle cluster,
`results/measure/20260921-133437-posix-isolated` (budget 15 s, jobs=1):

| test | result | seconds |
| --- | --- | --- |
| `concurrent_creates_same_dir` | PASS | 4.9 |
| `mtime_monotonic_many_writes` | PASS | 1.1 |
| `dir_deep_nesting` | PASS | 6.1 |
| `dir_deep_nesting_beyond_64` | PASS | 4.8 |
| `names_crazy_dirs` | PASS | 3.3 |
| `dir_many_files` | PASS | 5.2 |

Suite 1 jobs=1 on the same build is **200/201** plus `mmap_write_read`
SKIP (`results/posix/20260921-123904`), above the 193 floor. If any of
these walks exceed 15 s again, the cluster was not idle or a wakeup
regressed — do not raise the budget.

- **Forbidden:** raising `POSIX_TEST_SEC` or the `@budget(...)` values. A
  timeout is a failure to be removed, not re-labeled.

#### W8 — 9-node POSIX suite 1 (gate: 201 rows, 0 NOTRUN, every host)

**State (Sep 21 22:15): 191–195 / 201 on every host, 0 NOTRUN, all nine
finish in ~62 s** (`results/posix/20260922-020950`). History and the
three fixes that got here are in the §1b progress log (harness clock at
submit; `h->mu` contention after `read_mu`; KV WAL fsync per apply).
Earlier symptoms — nine hosts at the 385 s cap with `[None]` rows
(`results/posix/20260917-191430`), the 1.2 s / 1.03–1.08 s root mkdir,
`rmdir` ENOENT on a just-created name, `EBUSY` after the 10.4 s backoff —
are closed: whole-shard txn scans (`165e779`), stranded txn records
(`9534e53`, `3291c6d`), `read_mu` (`223da15`), peer-pool starvation
(`f10fec0`), harness (`a683def`), view (`4eb1419`), WAL hold (`84a2a55`).

What still fails, in order (details and instructions in §1b):
0. Half-applied cross-shard txns (I17) — **fixed `46d54e6` and gated**
   (Sep 22): freeze both leaders during `same_parent_storm`. The parent
   row stayed consistent (`nlink=5 nents=3` with three real children on
   the run that left names behind; the other run removed the parent).
   `arc_term_miss` moved. Details in §1b.
1. Retry of a committed non-idempotent op after a BUSY (EEXIST on a
   fresh LINK name, EIO, empty read) → I16 op-id dedup for
   LINK/UNLINK/MKDIR/RENAME. Mechanical, spec §7.9.
2. The synchronous full-L1 compaction: 2.4 s under `h->mu`+`l->mu` on
   every replica at once, costs the leader its term. **Decision.**
3. Six many-op tests at 144 concurrent jobs (mkdir p50 21 ms under
   load; apply 0.3 ms/entry of KV reads). Measure, then decide.

Run it as `bash tests/measure/w8_stall_timeline.sh` (runs the suite,
gives the raft/latency timeline) and read `raft-obs:` from the four
`efsd.log`s. Do not raise the 70 s warmup, the 15 s per-test budget,
or the 400 s suite timeout; do not point the suite at a subdirectory;
do not raise the election timeout.

- **Forbidden:** reporting `[None]` rows as failures, or as passes;
  the clock-at-submit bug coming back (a queued test cannot time out).

#### W9 — The client staging table is unbounded

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A, the only
part of step 12 that never landed.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[../client-cache-design.md](../client-cache-design.md). Two design points in it
are unratified — bring them to the user before implementing.

**Recommendation (Sep 23): ratify both as written.** (1) The pin rules —
dirty / publishing / dirty dcache, ghost with an open fd, in-flight op pin,
live append reservation or lock — are exactly the set of rows whose absence
a report or an open fd could observe; anything narrower loses a write,
anything wider is not a bound. (2) `EFS_CLIENT_META_MB` = 256 MB default,
soft: at ~200–300 B per row plus chunk maps that is on the order of a
million rows, above any FUSE working set we have measured (IO-500 9×4,
the posix suites, `find` over the 410-name root), and a table where
everything is pinned grows and logs once rather than evicting work. Pick
a different number only if a client RSS measurement says so; the cap is
an env var, not a protocol.

Steps, once ratified:
1. Pin bookkeeping: a per-row pin count set by the dirty/publishing sets,
   `efs_open_note`/`close_note` for ghosts, and a scoped pin in every
   dual-apply window (create, rename, link, unlink). Unit test: a report
   built while eviction runs never skips a dirty row.
2. LRU by last-touch tick over unpinned rows; evict chunk maps of clean
   closed files first, then rows. Ghost reclaim at last close.
3. `statfs` from server numbers; `efs_export_fits_page_cap` becomes a
   best-effort early-out (the doc's §4).
4. Gate: `make test`; posix 1 (jobs=1 and 9-host) and posix 2 with no new
   failures; the walk-RSS gate — one client walks a multi-million-file
   tree and RSS stays near the cap where it grew linearly before; the
   valgrind leak gate (eviction is a new free path).

- **Gate:** the walk-RSS gate in that doc, plus posix 1 + 2 and `leaks`.
- **Forbidden:** evicting a row that is dirty, publishing, a ghost with an open
  fd, or pinned by an in-flight op. Bounding it by dropping records instead of
  refetching them.

#### W10 — RDMA empty-table first `mkdir` — gate on the private cluster, no wipe

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

Default transport stays TCP until it is gated. Every ceiling in the table
above assumes RDMA eventually carries the data path; TCP over IPoIB will
not reach it, so every perf item after this one is capped until W10 closes.

**Recommendation (Sep 23): do not wipe 19810 for this.** The repro needs an
*empty* table, not *the* table: `tests/rdma_first_inode.sh` stands up a
private 3-node efsd on one host (fcstor007, ports 19950–19952, storage
under `/tmp/efs-rdma-first`, `EFS_TRANSPORT=auto`) and runs the first
`mkdir`. That is the gate, and it touches nothing on 19810. The live
switch of 19810 to RDMA then needs no fresh table either — the project
state rule records that a populated table drove millions of creates over
RDMA with 0 errors; the empty-table hang was the only open defect. A wipe
buys nothing here that the private cluster does not; keep it for W11
(below), whose gate is easier on a table that grows from zero.

Steps:
1. `efs-bg.sh start w10-first 'bash tests/rdma_first_inode.sh'` with
   `EFS_RUNNER=fcstor007.ib`, ×5 (the tree must be built on 007 first:
   `efsd`, `efs-mgmt`, `efs-fuse`). Pass = every run prints
   `MKDIR_RC=0 WRITE_RC=0` and exits 0 (the first `mkdir` and a write
   inside it each have an 8 s `timeout`). Fail = any `MKDIR_RC=124`, and
   the fix is not in; stop and bring `/tmp/efs-rdma-first/fuse.log`'s
   `RDMA transport|WAIT TIMEOUT|SOCKET CLOSED` lines.
2. Switch the live cluster: `tests/roll_efsd.sh --all` with
   `EFSD_ENV="EFS_TRANSPORT=rdma EFS_RAFT_OBS=1"`, then remount the
   clients with `EFS_TRANSPORT=rdma`. Wait for `commit` flat on both
   groups (post-roll election churn is ~2–5 min).
3. Gate on RDMA, same numbers as TCP or better: posix jobs=1 (200/201),
   9-host posix (≥185/201, 0 not-run), `i17_leader_freeze.sh` ×2 with
   0 worker errors, idle `md_latency.py` within the TCP reference.
4. Re-baseline the write wall (`efs-fio-honest`: 8 GiB dd+fsync 1/4/9
   clients) and record it in §1a's ceiling table. Then TCP is no longer
   the default in the deploy rule and START-HERE.
5. If step 3 fails on anything that passes on TCP, roll back to TCP with
   the same `roll_efsd.sh --all` and bring the failure; do not debug
   RDMA on the live cluster with the suites down.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping 19810 without being asked
  (this item no longer needs it).

#### W11 — fcstor005 lags because its group's snapshot does not fit — recommendation below, awaiting ratification

`raft_host` only compacts when the whole group's KV export fits in one
`EFS_WIRE_RAFT_MAX_CMD` command. On 19810 one group is over that, so the log
is never compacted and `snap_oversized` latches. Measured Sep 21
(`results/measure/20260921-182308-raft-snap-state`): logs 1.83–4.36 GB
and still growing ~400–800 B/s idle, both groups `commit == applied`
(fcstor005 is not behind on this build; the gossip `DOWN` right after a
bounce is the STATUS probe, not a dead process — check `pgrep -x efsd`).
A follower restart is a full replay of that log.

The fix is a chunked / multi-message InstallSnapshot, and **that protocol is
not specified anywhere**. Do not implement it before the recommendation
below is ratified. Until then this is a known, documented lag.

**Recommendation (Sep 23): adopt the Raft paper's InstallSnapshot chunking
(§7 of the paper: `lastIncludedIndex`, `lastIncludedTerm`, `offset`,
`data[]`, `done`), and take the snapshot as a pinned segment view, not a
RAM blob.** Two facts drive it:

- The wire cap is the smaller problem. Today `efs_raft_snapshot` calls
  `snap_get` at `last_applied` **under the SM lock** and keeps the whole
  KV export in `r->snap_blob` in RAM; `host_snap_get` is a full
  `efs_kv_scan` of the group. Removing the 4 MiB cap alone would turn every
  `HOST_SNAP_MIN`-entry snapshot into a multi-hundred-MB scan under `h->mu`
  on the leader — the same stall class as W13, on every snapshot instead of
  every fourth flush. The spec already says snapshots exist only for log
  truncation and the KV is durable, so the snapshot *point* should cost a
  memtable flush and a `save_snap(idx, term)`; the *export* happens only when
  a follower actually needs one, off the pump thread.
- The protocol is not an invention. Chunked InstallSnapshot with
  `offset`/`done` is the one the Raft paper specifies; the code base already
  has the "regenerate at the current applied index on demand" precedent
  (`send_snap` after a leader restart). The follower side stays
  `efs_kv_group_import` on the assembled bytes, unchanged.

Steps, once ratified (after W13's step 1, which builds the pinned view):
1. Spec: add to [architecture.md](../architecture.md) §KV the snapshot
   rule — snapshot point = memtable flushed + `save_snap`; export is lazy,
   from a pinned immutable segment view taken in one pump cycle at applied
   index N (so it is exactly the state at N); shipped as ≤ 4 MiB `SNAP_REQ`
   chunks `(incl_index, incl_term, offset, done)`; the follower stages
   chunks in `<storage>/mdraft/snap-<group>-<incl>.part`, restarts from
   `offset 0` on any `(incl_index, incl_term)` change, and on `done` imports
   the file, sets `last_applied = incl`, and truncates. Under the existing
   rule: no snapshot past the KV's durable point.
2. `raft.c`: drop `snap_blob`; `snap_get` becomes `snap_open(incl) →
   (handle, total_len)` + `snap_read(handle, offset, buf, n)` +
   `snap_close`; `send_snap` keeps one chunk in flight per peer (like
   `ae_inflight`) and advances on the `SNAP_REP` ack carrying `offset`.
   The 8-byte `app_old/app_new` prefix stays on chunk 0.
3. `raft_host.c`: `host_snap_open` pins the view in the pump cycle, exports
   it to `mdraft/snap-<g>-<incl>.kvx` on the GC thread (the one that runs
   `host_txn_recover_pass`), unpins, and serves `pread`s from the file;
   one snapshot file per group, replaced when a newer one is opened.
   `snap_oversized` and the `snapshot skipped` path go away.
4. `raft_sim` test: a follower behind a truncated log catches up from a
   snapshot that spans ≥ 3 chunks; a leader change mid-transfer restarts
   the transfer from offset 0; a follower crash mid-transfer leaves no
   `.part` applied.
5. Live gate on 19810: both groups' `raft.log` shrink and stay bounded
   (`results/measure/…-raft-snap-state` re-run: today 1.83–4.36 GB); kill
   and restart fcstor005's efsd — it rejoins by snapshot in seconds, not by
   replaying GBs; `apply_max` in `raft-obs` does not spike at a snapshot;
   `md_latency.py` unchanged. 9-host posix and the freeze script as usual.

- **Forbidden:** implementing before the row is ratified. Killing 005 to
  "fix" the lag — a 2+1 PUT needs every fragment ACK, so removing a node
  breaks writes.

#### W13 — Synchronous full-L1 compaction is the remaining election trigger

Measured Sep 21 (`results/measure/20260921-220933-w8-stall-timeline/obs-*.txt`):
`kv_compact_locked` runs inside the apply path under `l->mu` + `h->mu`,
rewrites all of L1 (~16 MiB; 64 MiB L1 segments) every 4 memtable flushes
because one L0 segment spans every shard and so overlaps every L1 segment.
`apply_max=2464250us` on 004 and 005 at once, group 0 term 5299→5302→5303,
client `stat` 2 s. It is the last non-hardware cause of a term change under
the 9-host suite, and it scales with table size, so it gets worse. It was
listed in §1b as "unspecified → decision"; the decision request is here.

**Recommendation: run compaction on a background thread; then, as a
follow-on, bound its per-cycle I/O with a partitioned flush. Do not raise
the memtable or the election timeout.**

- *Why background first.* The stall is a lock hold, not CPU: the apply path
  waits on `l->mu` for a 2.4 s file rewrite it does not need to observe.
  Segment files are immutable, so a compactor can read the old L0/L1 files
  and write new L1 files with no lock at all; only the manifest swap needs
  `l->mu`, and that is microseconds. The logical content of the KV is the
  same on every replica regardless of when compaction runs, so it does not
  touch determinism, the applied index, or the durability rule (the WAL and
  the manifest swap are what persist). This removes the stall at any table
  size.
- *Why not per-key-range compaction alone.* Leveled compaction bounds the
  bytes per compaction only when L0 segments have narrow key ranges. Our
  memtable holds every shard, so every L0 segment overlaps every L1 segment
  and a range compaction still rewrites all of L1. It needs a partitioned
  flush (one L0 file per key range) first — that is the follow-on, and it
  is worth doing for read cost (one `pread` per overlapping segment per
  KV read), not for the stall.
- *Why not a bigger memtable.* It makes the same stall rarer, and one stall
  is one election. Same reason the election timeout stays: a longer timeout
  hides a 2.4 s apply stall today and a 5 s one at twice the table size.

Steps:
1. **Pinned segment view** (shared with W11): `kv_lsm_view_pin()` returns a
   refcounted copy of `{l0[], l1[]}`; a segment file is unlinked only when
   the manifest no longer lists it *and* its pin count is 0. Unit test:
   a scan over a pinned view is unaffected by a concurrent flush and
   compaction.
2. **Compactor thread** in `kv_lsm`: the write path flushes memtable → L0
   as today and signals when `n_l0 >= l0max`; the compactor pins the L0 set
   plus overlapping L1, merges to new L1 files with no lock held, then
   under `l->mu` swaps the manifest (atomic rename — verify
   `kv_manifest_write` is), drops its pin, unlinks unreferenced files.
   `kv_compact_locked` remains only for `efs_kv_lsm_compact` (tests,
   `--compact` tools). Back-pressure: when `n_l0` reaches `KV_LSM_MAX_SEGS`
   the write path waits on `l->cv` for the compactor — the only stall left,
   and it means the compactor is 16× behind (log it). Crash test: kill
   after every file write in a compaction, reopen, verify every key.
3. Remove the compaction call from `efs_kv_lsm_flush` (the snapshot path)
   — it only needs the memtable flushed.
4. **Gate:** `raft-obs` `apply_max` < 100 ms on every replica across ≥ 3
   compactions during the 9-host suite (log `kv-compact: start/end` with
   bytes and ms); `tests/measure/w8_stall_timeline.sh` shows no term change
   on either group through the run; `md_latency.py` within the idle
   reference; `make test` green with the two new tests; 9-host posix
   ≥ the current 185–197/201.
5. **Follow-on (after gate):** partitioned flush — write the memtable as
   one L0 file per shard range so compaction touches only overlapping L1
   segments; gate = bytes rewritten per compaction bounded by
   `KV_LSM_L1_TARGET` × ranges touched, read `pread` count per KV get ≤ 3
   on a 1 GiB table.

- **Read:** `src/kv/kv_compact.c`, `src/kv/kv_lsm.c` (`efs_kv_lsm_flush`,
  `kv_maybe_flush_locked`), the "One KV WAL fsync per pump cycle" and
  "Synchronous full-L1 compaction" learnings in the project state rule.
- **Forbidden:** raising the election timeout; raising `KV_LSM_MEM_DEFAULT`
  as the fix; any compaction step that holds `h->mu`.

#### W12 — Repo hygiene

`results/` holds only Raft-engine runs now (the pre-Sep-11 history was
removed). Commit a run directory when it is cited as a gate from this page or
from `.cursor/rules/`; otherwise delete it.

Run `make docs-check` after any doc edit: it regenerates
`architecture-full.md` **and** `architecture.html` from the markdown sources
and validates links plus every `I1..I25` reference. Never edit either
generated file. `make test` must be fully green — there is no
accepted-failure list.

---

**Decisions only the user can make — bring the evidence, do not start.**
These came out of reviewing efs as an HPC parallel filesystem. Each one
changes what efs *is*, so an agent must not pick a side; but each has a
cheap measurement an agent can produce first, named here:

- **Per-file layout control.** Chunk size and the EC profile are per export;
  there is no per-file or per-directory equivalent of Lustre's `lfs
  setstripe`. A 4 KiB-record checkpoint and a 1 GiB-per-rank dump therefore
  get the same 128 KiB geometry, and the declared 32× small-write
  amplification ([architecture.md §9](../architecture.md)) has no opt-out.
  Evidence to bring: W6's IOR-hard/IOR-easy ratio and rw-4k from W4.
- **An interface beyond FUSE.** FUSE is the only client. libfuse 3.10.2 cannot
  negotiate `FUSE_MAX_PAGES`, so every request is ≤128 KiB regardless of
  `max_write`; it cannot emit `FOPEN_PARALLEL_DIRECT_WRITES`; and Linux takes
  the inode lock exclusively for extending direct writes and the parent
  directory lock for `O_CREAT`, **per mount** (already in
  [architecture.md §9](../architecture.md) as an open kernel-interface item).
  So 64 ranks on one node writing one file serialize in the kernel before efs
  is called. Every production PFS has a kernel client, a user-space library,
  or an MPI-IO ADIO driver. Evidence to bring: W4's per-client scaling and a
  1-node 8-rank IOR-easy vs 8-node 1-rank IOR-easy comparison from W6.
- **Whether `write()` is durable** — W2 closed: spec says `fsync`/`close`/
  `O_SYNC`. Do not reopen as publish-on-write. `O_SYNC` wiring is a later
  item, not a silent side-cut.
- Already listed before this review: C1 relaxed coherence; a pressure-triggered
  directory-spread bound (unspecified); cutover of a 36T `efs-test`; any new
  REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](../product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including the
things that contradict a guarantee the spec already makes (no fragment
repair, no protection-debt tracking, no session/fencing on the client, and
W1/W2 above). Those are not queue items beyond what W1–W2 say; each needs a
design decision first. Do not start one without asking, and do not treat the
queue above as the whole distance to a product.

Everything the §10 steps delivered (10.5c-1..35d, step 11's deletion of the old
engine, step 12 parts A–D) is landed and gated; the per-increment narrative is
in the commit history and in `.cursor/rules/efs-project-state.mdc`, not here.

**When the queue above is empty,** the next task comes from a measurement, not
from this page: run the gates in [testing.md](../testing.md), and take the
largest gap between what a gate reports and what the ceiling table at the top
of §1a says the hardware allows. If closing it needs a design decision the
spec does not contain, stop and ask (§4).

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
| Production Raft host (`EFS_MD_RAFT`) | `src/server/raft_host.c`, [architecture.md §10](../architecture.md) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied; SNAP blob is the existing WAL item payload | `tests/test_kv_lsm`, `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
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
