# Superseded contract/measurement wording — before round 6

Preserved Oct 7, 2026. Use the current work-item and failure-tolerance guides;
these earlier statements are evidence of the correction, not current directions.

#### W23 — Server: the apply path blocks on L0 back-pressure, and compaction rewrites the table to absorb a few MiB

> **Current status (Oct 2 2026; a document claim — the cited gate directory is the evidence):** D9–D13 DECIDED and rolled (Sep 29; the "pending" header below is historical). Open under this number: (1) the **memory / lag bound is unproven** — see the correction below the steps; (2) the GC frag pass over a 50-file L0 steady state is **D26** (decided: watermark first, after W44 a). **Oct 5Z update on (1):** the stalled-compactor test is IN TREE — fault hook `EFS_FAULT_COMPACT_STALL` + `/tmp/efs/fault` in `compactor_main` (compiled only with `EFS_FAULTS=1`, parks between iterations holding no lock), the pump's `kv-obs: mt_bytes= l0_bytes= l0= l1=` line it samples, and `tests/measure/w23_stalled_compactor.sh` (private 3-node cluster on one dev VM, one client, dd to fresh files, the 5 s/node sampler, the text's five stop bounds, then fault cleared and catch-up timed). Deploy gate PASS (dev cluster, 16/16 unit suites; strings gate: hook absent from the normal build). **The measurement itself has no numbers yet:** the one run died at setup on a scratch-dir EACCES (fixed after, with a results-path bug in the script); rerun owed — `results/measure/20261005-014440-w23-stalled-compactor/SUMMARY.txt`.
>
> **Remaining action:** the first measurement is recorded (`results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt`): no stall-specific effect up to 4.35 GiB — n_l0 stayed 0 on every node, so merges were never needed and the stall never bit; the rss-2x stop fired on ordinary working-set growth (the healthy leader grew the same way). The follow-up run, when this item is picked up again: differential RSS bound (node3 vs max(leaders)) and a pre-stall phase that forces n_l0 > 0 (SUMMARY.txt "Interpretation"); then D26 per its row (DONE Oct 4 — see its row).
>
> **Governing decision:** D9–D13, D26.
>
> **Gate:** the stalled-compactor test's numbers recorded in `results/measure/`; `md_latency.py` medians unchanged; no `kv-compact: backpressure` line in a 9×4 IOR.

**Historical record (dated).** Evidence and steps as they were written at the time. A step marked *done* or *superseded* in the status block above is not to be executed; its text stays so the gate directories and the reasoning remain findable.

**Status (Sep 29 2026, 05:11Z).** In tree and rolled
(`a53b253f2455-dirty`, RDMA, `--perf --strace`). Step 1: the apply path
does not wait in `kv_maybe_flush_locked`; a publish batch returns BUSY
from `host_pub_batch_propose` while L0 is within one flush of the cap.
Step 2: a range is compacted when its L0 bytes are at least 1/8 of its
L1 bytes (or when L0 is at the file cap, whichever comes first);
`key[0]==0` is 16 subranges (`key[1]`'s top nibble) at flush and at
compaction output; compaction iterators read 1 MiB ahead. Not gated.
The 1 GiB L0 byte cap in place of the 64-file array is not in this
change — the file cap still forces a compaction, but of one subrange.

**Source (Sep 29 2026 04:07–04:27Z, `results/measure/20260929-040800-idle-trace/ana`,
perf + `strace -f -tt -T` on every daemon, a user `ecopy` 04:15–04:22Z
writing 408K fragments per server; the handoff archive in [../project-history.md](../archive/project-history.md) has the full list).**

- `kv_maybe_flush_locked` (`kv_compact.c:711`) waits on `l->cv` while
  `n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS` (i.e. L0 ≥ 48 files). The
  caller is the apply path on the pump under `h->mu`. On fcstor004 the
  pump's `futex` waits were 4.14, 1.68, **24.23**, 2.70, 3.57 s, each the
  length of one `kv-compact: end ms=`; `pump_hold_max` 4.4 s (history
  6.4 s), `apply_max` 4.4 s. fcstor005 2.14 s. W13 step 2 documented
  this wait as "the only stall left" and logged it (`kv-compact:
  backpressure n_l0=`); the log shows it continuously through the write.
- While the pump is held: `host_wait_applied` hits its 400 ms deadline
  (`raft-host: apply-sleep us=400xxx`, dozens per hold) → the REPORT
  answers BUSY (`report-split nrec=76757 … finish_ms=1881 rc=-13`; 12 of
  116 on 004) → the client's `fsync` returns EIO (W17.1) — the same
  abort the last two IORs died of. No heartbeat leaves either: group 0
  9418→9421 (04:15:43Z), group 2 2902→2904 (04:16:17Z), inside the
  holds. With W22.1–2 rolled and zero snapshot activity, this is the
  multi-second pump hold that is left.
- Why L0 is always near the cap: a flush writes one L0 file per
  `key[0]` range (W13 step 5, ≤ 16 files of ~256 KB from a 4 MiB
  memtable); compaction takes one range's L0 files (`inputs=` 5–11) and
  rewrites that range's whole L1 — 190–330 MB for ranges 1–15, **1.8 GB
  for range 0**. 433 compactions in 20 minutes on 004 (441 / 648 / 656
  on 005 / 003 / 006) summed to **159 GB** of `bytes=` against a 5.3 GB
  table; range 0's 34 rewrites are 61 GB of that and the 24 s hold. The
  compactor thread is 48 % / 55 % of fcstor004 / fcstor005's samples
  (`cm_sift_up`, `memcmp`, `cm_pop`, `kv_seg_probe`), with 5.25M 4 KiB
  `write`s (stdio default buffer) and 3.7M ~8 KB `pread`s (one per
  input block).
- The compactor's segment `fsync` is the 100 ms mode (299 of 695 on 004
  in 90–120 ms, avg 51 ms; 407 of 733 on 005); the pump's Raft-log
  `fsync` averages 0.38–0.40 ms. That closes D6 (D11): not the sharing.

**Steps as written Sep 29 (D9–D10 were DECIDED the same day and rolled 05:31Z; D12/D13 followed — historical):**

1. **The pump never waits for the compactor (D9).** In
   `kv_maybe_flush_locked`, when the caller is the apply path (pass a
   flag, or make the apply's flush a separate entry point), do not wait:
   flush what can be placed, or let the memtable grow past
   `memtable_max` and return OK; the memtable is bounded by what the log
   can commit ahead of the KV (512 MiB since W22.1). Back-pressure moves
   to admission: `host_propose` (or the REPORT handler before it)
   returns BUSY when the local L0 bytes are over the cap, so the client
   retries on its existing 16-attempt / 10 s budget and no follower's
   pump ever stalls. `kv_compact_locked` for tests and `--compact` keeps
   the synchronous path. Gate: a 9-client `run.sh ior` with `pump_hold_max`
   under 100 ms on every node throughout (`raft-obs`), zero
   `apply-sleep` lines above 100 ms, zero `report-split … rc=-13`, no
   term change on either group; `make test`.
2. **Compaction by bytes, and a bounded range (D10).** (a) A range is
   compacted when its pending L0 bytes reach 1/8 of its L1 bytes
   (`kv-compact: start` logs both); the L0 cap becomes a byte budget
   (1 GiB) and the 64-file cap goes — the read path already probes every
   L0 file, so the file count is not the constraint. (b) A range whose
   L1 exceeds ~256 MB is split on the next key byte at flush and at
   compaction output, so no single rewrite exceeds that. (c) The merge
   reads each input segment through a 1 MiB sequential buffer instead of
   one `pread` per block. Gate: over the same IOR, `kv-compact: end`
   `bytes=` sum under 10× the KV's growth for the run; no compaction over
   1 s; compactor under 15 % of any server's samples; `test_kv_lsm` and
   `test_partitioned_flush` pass; a KV copy from a live node reopens and
   verifies every key.
3. **Already in tree (8g), roll with the next build:** `disk_log_new_bytes`
   O(1); `kv_seg_w_open` `setvbuf` 1 MiB.

**Correction (Oct 2 2026) — the bound in step 1 is unproven.** Step 1
says the memtable "is bounded by what the log can commit ahead of the
KV (512 MiB since W22.1)". It is not: the 512 MiB figure is a
*snapshot trigger*; it neither admits nor refuses writes, and a
follower acknowledges AppendEntries on log persist, not on apply, so
a follower whose compactor is stalled accumulates unapplied log (on
disk) without the leader noticing except through follower-served
`host_wait_applied` timeouts. The mechanisms that actually exist:
(a) `memtable_max` flushes the memtable to an L0 file; (b) the 64-file
L0 cap, at which the apply path **blocks** (`kv-compact: backpressure`)
— a pump stall, which is the thing D9 forbids, so today the bound is a
stall; (c) D9's admission BUSY on local L0 bytes over 1 GiB applies to
the leader's own KV only; (d) D12/D13 keep `n_l0` under the cap by
L0→L0 merges while the compactor runs. **Remaining action (no
decision needed to measure).** Fault hook `EFS_FAULT_COMPACT_STALL=1`
(compiled only with `EFS_FAULTS=1`), **fault location:** `compactor_main`
parks at the top of its loop, *between* iterations, holding neither
`l->mu` nor `h->mu` and owning no pinned view — i.e. the compactor is
alive but never starts a merge. Memtable flushes to L0 (`kv_flush_locked`,
apply-path) continue; only L0→L1 and the D12/D13 L0→L0 merges stop.
Parking while holding `l->mu` would test a lock-hold, a different
failure, and is not this experiment. **Run:** private 3-node cluster
(`tests/rdma_first_inode.sh` layout on one fcstor, `/dev/shm` or a scratch
dir), one client, `dd bs=1M` of a non-zero source to fresh files, the
hook set on **one follower only** (the leader keeps compacting, so the
leader-side admission (c) is not what fires). **Sample every 5 s per
node:** memtable bytes, `n_l0`, `pump_hold_max`, `apply-sleep` count,
`kv-compact: backpressure` count, `commit − applied`, RSS. **Bounded
stopping condition — stop at the first of:** the follower logs
`kv-compact: backpressure` (bound = the stall at 64 L0 files, name the
bytes written at that point); follower RSS exceeds 2× its pre-run RSS;
follower `commit − applied` exceeds 10 000 entries for 30 s; 10 GiB
written; 10 minutes. **Then** clear the hook (`/tmp/efs/fault`) and
record how long the follower takes to reach `commit == applied` (must be
under 2 min, else that is a second finding). **Result:** the dir under
`results/measure/` names which of (a)–(d) stopped the run and at what
size. A finite run can show a bound that fired, or lag that grew for the
whole run with no bound observed; it cannot prove unbounded growth —
write whichever it is. If the answer is "a pump stall at 64 L0 files" or
"no bound observed within 10 GiB / 10 min", that is a design ask (a
follower-lag admission rule) — bring the numbers, do not pick.

- **Read:** `src/kv/kv_compact.c` (`kv_maybe_flush_locked`, `compactor_main`,
  `kv_compact_locked`), `src/kv/kv_lsm.c` (`kv_flush_locked`), `src/kv/kv_seg.c`,
  W13 (closed; [../project-history.md](../archive/project-history.md) "START-HERE closed items") and the
  "L1 compaction is a background thread" learning in the project-state rule.
- **Forbidden:** raising `KV_LSM_MEM_DEFAULT` or `KV_LSM_L0_DEFAULT` as the
  fix (W13, closed); raising the election timeout, `HOST_TICK_US`, or the 400 ms
  apply budget; any compaction step under `h->mu`; making `fsync` succeed
  on a merged-back dirty set; moving `mdraft/` to another device to hide
  the compactor's I/O (D11 says it is not the sharing).

**When the status page's queue is empty,** the next task comes from a measurement,
not from this page: run the gates in [testing.md](../how-it-works/testing.md), and take
the largest gap between what a gate reports and what the ceiling table in
[performance.md](../how-it-works/performance.md) says the hardware allows. If closing it needs a design
decision the spec does not contain, stop and ask ([developing.md](../how-it-works/developing.md) §4).

---

---

## Queue rows and plan texts moved from the status page (Oct 3 2026)

The sections below are the full texts of the status page's queue rows and
plan rows, moved here verbatim so [the status page](../status/README.md)
can be a one-line-per-item index (table cells reflowed to sections; the
text is unchanged). The queue order and the one-line rows live in
[../status/README.md](../status/README.md) §1a; references of the form
"§1a", "§1b", "row N" and "plan row N" below refer to that page's tables
and its archived handoff blocks unless they are linked. D-numbered
decisions cited here are in [../status/decisions.md](../status/decisions.md).

**Open correctness rows (Oct 1–2 2026; these go before every
performance row). Rows that are done (1j, 0f, 1i, 0d, 1a–1h) are in
project-history.md "START-HERE closed items".**

## W54 · a fold's GC deletes the live base (queue row 0i)

**Oct 5 implementation checkpoint — IN TREE, local gates pass; uncommitted,
cluster gate pending.** `efs_meta_apply_publish` now skips GC for a superseded
base or span (including replay tombstones) whose generation matches the new
base or whose nodes/checksums alias it. Distinct superseded objects still emit
GC records. `test_gc_fold_live_alias` covers live-span and tombstone aliases,
same-generation and different-generation cases, fold replay, and a non-alias
reclamation control. The regression produced 12 failed assertions before the
fix; the complete `test_meta_apply` suite passes after the fix, including under
ASan/UBSan. No GC-side guard was added; that remains undecided below. Remaining:
roll and the two-peer fold / wait-for-GC / remount cold-read gate in (c), plus
cold IOR-hard verification and hardscan. No cluster rollout performed here.

**W54 · a fold's GC deletes the live base: when the folded image's object is also a tombstoned span (same content hash → same generation), `efs_meta_apply_publish` queues that generation for GC and the reaper unlinks the fragments the row still names; the next cache-miss read is EIO (Oct 2 17:04Z, IO-500 9×4 ior-hard-read `MPI_ABORT`). MUST FIX before any performance row; data loss with no repair**

**Follow-up steps.** (a) **fix the apply (mechanical, L7 already says a fragment set the live row names is not an orphan):** in the fold branch of `efs_meta_apply_publish` (`meta_apply.c` ~3625, "A full image replaces the spans") skip a span whose `generation == stored.generation` or whose `(nodes, checksums)` alias `stored` (`chunk_aliases`); apply the same filter to the tombstone walk, which today would also GC the previous base when a new image aliases it; (b) `test_meta_apply`: span of object X, then full image with `candidate_gen == X` → the batch holds no GC key for X; (c) repro + gate: two peers write adjacent ranges of one chunk so both merge to the identical image (ior-hard shape; or a posix2 `peer_shared_chunk_fold_gc`), wait past the GC latency (≥ 2 s), remount, cold read; plus a cold `ior-hard-verify` + `hardscan` after every 9×4 run (this run surfaced it only because a same-mount read missed the cache); (d) **ask (not decided):** a GC-side guard — `host_gc_record` point-gets the chunk row before each delete and skips a generation it still references (one get per record on the GC thread = D26's cost; the GC key lacks the inode generation the chunk key needs); (e) stop-all/start-all roll of the four servers, then the gate in (c)

**Evidence and limits.** `results/measure/20261002-165956-redeploy-posix-ior/SUMMARY.txt`; fcstor015 `fuse.log` 17:04:40Z `fetch published ino=656804 ci=181944 rc=-9 then pull rc=0 rc=-9` (row unchanged across the pull — not a stale map), `efs-fuse read: decode error (efs_rc=-9) off=23847816512 len=47008`; `raft-getchunks 656804 181944`: `base_gen=15366554570668337119 spans=2` with tombstones `4218386339069290638 seq=1088` and `15366554570668337119 seq=3731` — the base IS the second tombstone; on disk under `…/0065/6804/177/` only `181944.{0,1,2}.3089159234672355549` (one per fcstor003/004/005), the live generation gone. `gc_queue` (`meta_apply.c:3154`) checks nothing; `host_gc_local_del` → `efs_store_del_if_sum` passes on the same object's sum. Server code unchanged since `2b5a25df`; the hit is probabilistic (same-gen collision, GC runs, cache miss) so the 15:21Z clean hard-read does not clear it. The file `/tmp/efs-mount/io500/2026.10.02-13.02.57/ior-hard/file` is unrecoverable (test data; delete it). Forbidden: zero-filling the read (I9); a longer GC latency to hide it; a client-side retry loop


## W55 · span committed to raft, fragment PUTs never landed → read EIO, data loss (queue row 0k)

**W55 · a span is committed to raft naming generations/node sets whose fragment objects do not exist on any node — nothing was ever PUT, nothing was reaped — and every subsequent read of the chunk fails with `EFS_ERR_DECODE` (-9) → EIO (Oct 5 2026, xorinox test cluster: 3-node libvirt, 2+1, gateway nfsd re-export mounted by a macOS client). Data loss with no repair; same user-visible signature as W54 but a different mechanism — W54's reaper deletes objects that exist, here the objects never exist.**

**Follow-up steps.** (a) **trace the span write path stage → PUT fragments → report span:** a committed span whose PUTs never ran must be impossible; find where the PUT leg is skipped or its failure swallowed while the REPORT still lands (start at the `putid miss` fallback — "identity from staging table" — in the client report path, cf. W27); (b) add two log lines first, then repro: the span REPORT (putid/gen + node set) and per-fragment PUT completion/failure keyed by that gen — one repro then shows which leg vanishes; (c) repro loop on the test cluster: `cp <file> /mnt/nfs-export/` from the Mac, then `efs-mgmt raft-getchunks <seed> <ino>` + `find /data1 -path '*<ino>*'` on each node — span-without-objects = hit, objects present + read back = clean; (d) decide the relation to W27 (REPORT identity / `putid miss`) — possibly one root; (e) gate: the repro loop clean 20/20 plus a posix suite run on the nfsd re-export mount

**Evidence and limits.** xorinox cluster, Oct 5 2026 (efsd built 20:59Z; both hits on gateway xefsgw, fuse log `/mnt/efs-fuse-efs.log`, server logs `/data1/efsd.log`). **Hit 1** — ino 8193, written 21:38:06Z (pre-`all_squash` export, macOS EXCLUSIVE4 mode-0 create, uid 501): read 21:39:45Z → `fetch published ino=8193 ci=0 rc=-9 then pull rc=0 rc=-9`, row identical before/after the pull (`gen=0 nd=1 seq=5`), empty `{…}/8193/0/` fragment dirs on all nodes; raft applied only lease open/close (kind 8/9) for the ino; the staged span's **seq advanced 5→9 across pure read attempts** (22:00:04 → 22:00:25Z, no writes — reads mutating the staged record). **Hit 2** — ino 32769 (`/tiny_files.py`, 2922 B), written 22:00:38Z *after* the export fix, cluster fully up (no efsd restarts 21:14 → 22:05Z): `raft-getchunks 32769` = `ci=0 nodes=0,0,0 base_gen=0 ck0=00000000 spans=1 seq=1; span off=0 len=2922 gen=5683377705060155088 nodes=3,1,2`; `find /data1/data/exports/1 -path '*32769*'` on all three nodes: **zero fragment objects**; reads EIO persistently from Mac NFS and FUSE-direct on the gateway. **Excluded:** GC/reaper (no delete records near either hit; W54's mechanism needs objects that exist); the export uid/squash (hit 2 post-dates the `all_squash` fix by 20 min); a stale client map (row identical across pull). **Controls, same window, same mount, all landed + read back fine:** rsync of a git tree (20:44–21:08Z), fresh 2922 B random file (21:48Z), same content to a new name (21:49Z), create-then-overwrite (21:52Z), FUSE-direct write on the gateway. Both hits were `cp` of the same source file from the same Mac client; the trigger is not isolated (a third cp onto the broken name at 22:05Z also failed but is contaminated — it raced an efsd roll). Client log in the same window shows `efs: report rec … identity from staging table (putid miss, n=1)` for two other inos (29918, 31158) whose files read fine. Both hit files unrecoverable (objects never existed; test data — delete). Forbidden: zero-filling the read (I9); treating this as NFS-export configuration (FUSE-direct reads fail identically); a read-side retry that papers over the missing objects


## W56 · root-level rename leaves a ghost name in the renaming client's local lookup (queue row 0l)

**Oct 6 fix:** `f8fef814` drops matching old-directory-name cache rows from both lookup tabs after authoritative rename. It preserves other names/chunks and ignores a replacement inode. NUC full POSIX jobs=4/jobs=1 and full posix2 PASS; the root directory regression now passes. Xorinox roll remains owed.

**W56 · after `mv /export/A /export/B` with the parent the export root (or any spread directory), the renaming FUSE client keeps resolving the old name in LOOKUP — stat/open on the old path still succeed and return the renamed inode — while the server metadata and every other view (parent READDIR, other clients, fresh mounts) are correct. The ghost lasts until remount (Oct 5 2026, xorinox test cluster, gateway FUSE mount). User-visible trigger: a tool that stats the output dir before creating refuses to run against the ghost of a just-renamed directory.**

**Follow-up steps.** (a) fix: the rename local-apply must resolve the old dentry with the same tab order as `efs_export_lookup` (dentry-hash tab first for root/spread parents) and delete the name-index entry there — today `efs_export_rename_at` (`metadata.c`) probes only the parent's shard tab, so for a root-level entry `name_idx_get` misses, the by-ino fallback `efs_export_rename` upserts the row under the new name, and nothing removes `(root, old_name)` from the hash tab; (b) audit `efs_client_unlink`'s local apply for the same tab asymmetry (rmdir tested clean Oct 5 at both levels, but confirm the code uses the lookup tab order rather than the parent shard only); (c) gate: IN TREE Oct 5 — `tests/posix/posix_suite.py` `@root` group (`root_rename_dir_old_name_gone` FAILS on the Oct 5 build, reproducing the ghost within the suite; the sibling root-level rename/create/unlink/mkdir tests pass) — then full posix jobs=1 + posix2 on the dev cluster after the fix

**Evidence and limits.** Repro on xefsgw (xorinox 3-node libvirt, bits=12 export, efsd built Oct 5 22:05Z): root-level `mkdir rt; mv rt rt2; stat rt` → still resolves ≥ 65 s later (bug); nested `nt/sub → nt/sub2` → ENOENT (clean); root-level and nested `rm -rf` → ENOENT (clean). Narrowed Oct 5 by the new posix `@root` group on xefsct1: only **directory** renames with a root parent ghost — root-level **file** renames and root→nested dir moves are clean, i.e. the missed tab holds directory dentries, not file dentries. Ghost is not the 0j 50 ms lookup memo (old dir name still resolves ≥ 2 s later). Mechanism chain: `efs_fuse_lookup_at` answers directory LOOKUPs from the local staged table with no RPC (by design, the mkdir-walk O(n²) note in `efs_fuse.c`); on a sharded export, dentries of root/spread parents live on the `hash(parent,name)` dentry shard and `efs_export_lookup` (`metadata.c`, the "Dentry shards only" fix) checks that tab first — the rename local-apply never does. Not server-side: parent READDIR, other clients and fresh mounts are all correct; it is not the kernel dentry cache either (the VFS moves the old dentry on a same-mount rename — the stale answer comes from efs-fuse). Forbidden: routing directory LOOKUPs via RPC as the "fix" (that path exists for files and was deliberately not taken for dirs); a time-based expiry that papers over the missed removal


## 0j · the client's 50 ms lookup memo returns pre-mutation stats (queue row 0j)

**Oct 5 implementation checkpoint — IN TREE, uncommitted; local gates pass,
Linux integration gates pending.** FUSE mutation requests clear the 32-slot
memo at entry and exit, and a balanced active-mutation count suppresses memo
use while operations overlap. LOOKUP captures a mutation serial before reading
its row; a reply spanning a mutation cannot reinsert stale attributes after
invalidation. Coverage includes write/write_buf, setattr (chmod/chown/size/times),
namespace mutations and replacement targets, O_TRUNC, fallocate, xattrs,
flush/fsync/release publication, and asynchronous writeback. Clearing all slots
is conservative: unrelated mutations also cause misses; no inode-resolution
RPC is added solely for invalidation. No-mutation LOOKUP → GETATTR still gets
its one-shot 50 ms hit, and attr_timeout remains zero.

`make test-lookup-memo` extracts the production memo and representative FUSE
callbacks with RPC stubs: writes, setattr reply attributes, link/unlink/rename,
O_TRUNC, failures, fallocate early exits, overlapping mutations, stale RPC
completion, TTL expiry and read-only/fresh hits pass. ASan/UBSan pass; earlier
client-memory tests pass again. An adapted-copy Mac FUSE syntax check passes;
this is not a production Linux build. Remaining gates: Linux posix jobs=1 back
to the recorded 200/201 baseline and Spark du performance measurement. No roll
performed. Next code item in the agreed sequence is W38.

**Client regression in `20745142`/`efc0f499` (not W54, same gate run): `lookup_memo_take` answers a GETATTR within 50 ms of the LOOKUP from the LOOKUP row — a stat right after write/chmod/link/utimens on the SAME client shows the pre-mutation row.** posix jobs=1 **164/201, 36 fail** (`results/posix/20261002-170119`: `basic_dd_rw` size 0, `attr_chmod` mode 420, `hardlink_basic` nlink 1, `attr_utimens_ns` old mtime …); posix2 63/63 (the peer holds no memo). The 15:19Z tree was 200/201

**Follow-up steps.** mechanical: the memo must be invalidated by every local mutation of that ino (write/truncate/setattr/link/unlink/rename/utimens — the same set that already drops `g_lookup_memo` candidates on nothing today), or consumed only when no local op on the ino happened since `lookup_memo_put`; gate: posix jobs=1 back to 200/201 on one client and the Spark `du` number the commit cites not lost

**Evidence and limits.** `efs_fuse.c` `lookup_memo_put`/`lookup_memo_take` (`LOOKUP_MEMO_US` 50 ms, 32 slots, keyed by ino only). `attr_timeout` stays 0 (decided). Not a server change


## W38 · ior-hard fold tombstone without the span's bytes (queue row 0e)

**Oct 5 implementation checkpoint — IN TREE, uncommitted; deterministic
local regressions pass, historical IOR gate pending.** Two unsafe paths were
identified in the current client. STALE replay fetched a base plus live spans,
then copied the older local image over it when the base still matched this
client's object (including the no-range branch); the PUT named the fetched
span list despite lacking its bytes. Replay now preserves fetched live spans
and overlays only owned dirty ranges; tombstones do not count as live bytes.
A full image without a byte-backed observation now names an empty list unless
it is a true whole-chunk overwrite, forcing a server STALE/refetch if spans
exist. Metadata-only list/sequence learning is removed. A span whose local
chain fills during PUT can become a full image only with the captured byte
observation; otherwise it returns STALE without replacing the staged mapping
or PUT identity. The existing flush failure path retains/re-dirties its body
for a later retry; this race can surface an error on the current flush rather
than silently publish an incomplete fold.

`make test-fold-observation` compiles production replay and PUT functions with
separate byte images and metadata plus RPC stubs. Restoring the old replay
conditions in a temporary copy reproduces loss of the acknowledged 4256-byte
span. Tests cover owned ranges, cleared ranges, tombstones, table changes during
PUT, missing observations, chain-full fallback, whole overwrites, failed PUTs
and no failed-object publication. Local regression and ASan/UBSan pass; write.c
syntax, earlier client-memory/0j/D25-helper tests and metadata suite pass. This
proves the identified paths, not that the cited historical IOR run has been
reproduced on the cluster. Remaining: traced one-client four-rank IOR-hard,
cold hardscan, inspect bad rows, then the 9×4 cold verification gate below.
Next code item in the agreed sequence is W43/D25 production wiring.

**W38 · ior-hard: a client's full image folds its own published span without the span's bytes (4256 B of zeros, committed)**

**Follow-up steps.** the F3 block in §1b (Oct 1 08:05Z): one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, `hardscan` cold, `raft-getchunks` on each bad chunk; fix on the client (the fold observation must come from a body that holds the span)

**Evidence and limits.** `results/io500/20261001-074905-rdma` (NOTE.txt, hardscan.txt, getchunks-118842-118844.txt): ino 10897 ci 118843 base 1774…2861 + len-0 tombstone 1838…0185 seq 1222; 1 of 747720 records; first IOR with W30 in the client. Data loss: goes before 0c


## W36 · rename-vs-unlink of one source both succeed, dangling dentry (queue row 0c)

**Current acceptance, reviewed Oct 7.** The exact-source/value PREPARE and
UNLINK verdict changes passed the named `peer_rename_vs_unlink_src` gate
20/20 on xorinox `b4a75492` and again on final NUC `128f6b7d`, with broad
peer acceptance. W36 is accepted for those recorded builds; repeating the
gate on a new rollout is a release obligation, not evidence that the old
implementation remains unaccepted. The historical recurrence/next-rollout
instructions below are superseded by the
[round-2 ledger](../archive/queue-review-20261007-round2.md).


**Oct 6 reply-path fix:** `75b06624` makes simple UNLINK wait for the actual apply verdict. Previously an apply NOT_FOUND/BUSY was hidden by `host_wait_settled`, allowing both operations to report success even when unlink lost. This is distinct from the earlier exact-source PREP protection against dangling entries. NUC race gate 20/20 and full posix2 PASS; repeat on the current xorinox build before closing its recurrence.

**W36 · rename-vs-unlink of one source both succeed, dangling dentry** (posix2 `peer_rename_vs_unlink_src`, 1 in 6)

**Plan row 5 (in tree).** the hole was one path: `efs_meta_apply_unlink_op` / `rmdir_op` probed the inode row for a pending intent (`efs_txn_key_busy`) but not the DENTRY they delete unversioned, so an unlink could drop the source name + row under a RENAME whose dentry EXCL had landed and whose inode-row REDUCE had not; the rename's RESOLVE then PUT the dest dentry over a dead row (`-?????????`). Both log paths now probe `k_loc`/`k_hash` and answer BUSY (client retries → ENOENT after the rename resolves). The silent NOT_FOUND→OK in `apply_unlink_cmd`/`apply_rmdir_cmd` is gone (W45) Gate: posix2 `peer_rename_vs_unlink_src` 20/20.

**Follow-up steps.** the F1 block in §1b (Oct 1 07:45Z): trace the two txns with `APPLY_LOG`, decide between `apply_unlink_cmd`'s silent NOT_FOUND→OK and an EXCL DEL that passes on an absent key, fix that one

**Evidence and limits.** evidence `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 (`-?????????`), `~/efs-runs/p2r422.log`, `results/posix2/20261001-073048`. Correctness: goes before 1a–1h

**Recurrence (Oct 6 2026, xorinox cluster).** The same dangling dentry appeared at `/mnt/efs/posix-2c/peer_rename_vs_unlink_src/b` (found by the user's `find -ls`: readdir lists `b`, stat → ENOENT), created ~00:41Z by a posix2 run on a fresh (mkfs Oct 5 19:31Z) 3-node cluster running `v0.1.0-pre-alpha-12-g3d3f17c2-dirty` — a build that **contains** this fix (`2b5a25df`) plus the uncommitted Oct 5 D25 transaction/`meta_apply.c` work. KV-level proof, no client cache involved: `raft-readdir 3492` lists `b`, `raft-lookup 3492 b` → `ino=0 mode=00 nlink=0`. So either the fix's BUSY-probe does not cover the path this run took, or the dirty tree's txn changes reopened the hole — the owed 20/20 gate would have caught this; run it before anything else on the next cluster. The dangling name is still in the KV for inspection (cleanup: `efs-mgmt raft-unlink <node> 3492 b` — itself a probe of the fixed path).

**Guarded cleanup (Oct 6, b4a75492).** The retained `b` was still present after
xorinox deployed d0e8dce4; its parent mtime/ctime still matched 00:41:38 UTC.
This is persisted damage, not evidence of a new occurrence on d0e8dce4.
Unlink now removes a dangling regular-file name only when its missing inode and
LOCAL parent are in the same shard, with parent/dentry/inode/dseq intent guards
and atomic directory/opid updates. No inode or object is fabricated or erased.
Directory and foreign-shard corruption still fails closed.
[Repair and live acceptance checkpoint](../../results/measure/20261006-xorinox-orphan-unlink/SUMMARY.md).


**Local follow-up (Oct 6 2026, uncommitted).** Reproduced the complementary
race before rename's first source PREPARE: log-path unlink deletes the source
and its last-link inode without bumping the dentry's transaction version, so
version-only EXCL DEL still accepts the absent source. The original BUSY probes
protect already prepared names, not that earlier window. The committed code
already has this mechanism; the dirty build does not prove D25 introduced it.
`efs_meta_capture_dentry_drop` now checks the original dentry identity and
captures exact local/hashed bytes or absence. The shared server source-drop
helper for rename/unlink/rmdir prepares those comparisons through EXCL_VALUE;
unlink or name reuse before capture/PREPARE answers STALE, and prepared keys
continue to reject log deletion with BUSY. Split tombstones mask local copies
and cannot satisfy a live source. Regression tests reproduce old unsafe
acceptance and cover both race orders, ABA, split/hashed captures and a live
resolved destination. Full metadata/transaction tests pass normally and under
ASan/UBSan, simulator and strict local server syntax pass. Still owed: Linux
server/FUSE build and cluster `peer_rename_vs_unlink_src` 20/20, first on the
next rollout. No cluster rollout or artifact cleanup in this follow-up.



## W42 · df / efs-mgmt status report the 3-node capacity model on any node count (queue row 2a)

**Current state, reviewed Oct 7.** Both FUSE statfs and management status
use `efs_capacity_logical`. Final four-node NUC evidence reports 500 GiB
capacity from four 187.5 GiB quotas, with the FUSE total agreeing. The old
mechanical replacement steps below describe the original defect, not missing
implementation. The named fcstor capacity gate has not been established by
this review. The full-stripe protection question remains open: current PUT
returns OK with two ACKs and fewer than two quota failures, and its outer
retry loop returns immediately on OK, without rerouting a quota-rejected
third fragment. Trace and validate protection debt/repair before closing W42.
[Evidence and limits](../archive/queue-review-20261007-round2.md).


**W42 · `df` / `efs-mgmt status` report the 3-node capacity model on any node count** (Oct 1 2026, user). **IN TREE Oct 2:** `efs_capacity_logical` (placement.c, binary search on the Σ min(cᵢ, M) ≥ 3M bound), used by `efs_fuse_statfs` (total = quotas, avail = room, used = total − avail) and `efs-mgmt status`; `test_placement` covers 3 equal / 4 equal / 100/100/1000 → 200 / 6 equal / < 3 nodes → 0. Still to do: verify on 19810 (`df` vs `4 × 36T × 2/3`) and the one-QUOTA-member PUT question

**Follow-up steps.** mechanical: replace `total_logical = 2 × min_quota` (`efs_fuse_statfs`, `efs_fuse.c:3108–3120`) and `usable_cap = 2 × min_quota` / `usable_free = 2 × min_free` (`efs_mgmt.c:129–185`) with the 3-of-N placement bound: the largest `M` (chunks) with `Σ_i min(c_i, M) ≥ 3M`, `c_i` = node `i`'s quota (or free) in 64 KiB fragments, times 128 KiB; count only up nodes with a quota, as today. Reduces to `2 × min` on three nodes and to `Σ × 2/3` on N equal nodes. Also make `f_blocks` and the used figure come from the same model (statfs today derives used from `Σ phys × 2/3` and total from `2 × min`, so on four nodes used can exceed total and `avail` clamps to 0 while writes still succeed). Unit test with 3 equal, 4 equal, 3 unequal (100/100/1000 → 200, not 800). Then verify on 19810 (`df` vs `efs-mgmt status` vs `4 × 36T × 2/3`)

**Evidence and limits.** Both comments say "every chunk places one fragment on each node" — true for three nodes only. Four 500 GiB nodes show 1000 GiB instead of 1333. Not a data-path change; no decision needed. **Verify while there:** what a PUT does when exactly one stripe member answers `EFS_ERR_QUOTA` (`put_fragments_parallel_once`: `quota_errors >= 2` → QUOTA, `acks >= 2` → OK) — if the chunk publishes with two fragments, a full node creates protection debt silently ([product-gaps](../backlog/product-gaps.md) §1.2); if `reroute_down_fragments` moves it, say so in [the failure-tolerance table](../how-it-works/failure-tolerance.md)


## W27 · REPORT identity from the staging table (queue row 0b)

**Oct 6 drain follow-up:** `febc55e5` distinguishes a phantom span-only
staging-row mark from actual local ownership before requeuing a missing PUT
identity. The old zero-node branch requeued indefinitely and blocked clean
stop even after all data tests passed. Both report construction paths now
retain dirty/stalled/pinned/unreported local cache work, but drop an ownership-
free mark. NUC full posix2 64/64 and both fresh client clean stops PASS. The
nonzero-node staging-identity fallback remains; this is not a complete W27 close.

**W27 · REPORT identity from the staging table**

**Follow-up steps.** (a) find the path that leaves a dirty chunk with neither a putid nor a dcache object (`write.c:1103`: putid table eviction, reclaim after `b6c1712d`'s pin release, or an irec-only threshold REPORT); one traced ecopy of a small-file tree on an idle cluster; (b) rerun on the current client first — if `putid miss` is 0 there, record and close; (c) only if (a) names the cause: a chunk with no PUT of ours is not ours to publish (keep dirty, replay from the row), as the `fragment_nodes[0] == 0` branch already does for span-only rows

**Evidence and limits.** ≥ 9016 recs (`n=8812…9016` in the last rate-limited second, all `ci=0`) reported with a mapping that "may be the server's row, not this client's PUT" — the Sep 30 (gen, off, len) / conflated-table class that lost ior-hard records. Forbidden: silencing the line, or "committing" such a rec client-side


## W43 · truncate/O_TRUNC of a file with > 32 chunks in a lane is a silent no-op (queue row 0g)

**W43 · `truncate`/`O_TRUNC` of a file with > 32 chunks in a lane is a silent no-op; the apply answers OK on NOMEM** (Oct 1 22:00Z review, §1b). **Steps b and c IN TREE Oct 2** (the truncate now FAILS with EIO instead of lying; `tests/stress/truncate_big.sh`); step a is **D25, decided and revised Oct 2** (logical truncation + background reclamation), step d follows it

**Follow-up steps.** (a) **D25 (decided):** the fence entry sets epoch + size, the reaper reclaims; (b) mechanical regardless of D25: `apply_truncate_cmd` / `apply_lane_fence_cmd` put the apply's rc on the ring instead of `EFS_OK`, so SETATTR fails (EIO/EBUSY, W16 mapping) rather than lying; (c) repro + gate: `dd bs=1M count=10 conv=fsync` of a non-zero source onto an existing 1 GiB file, then `stat` (size 10 MiB), `md5sum` (the new bytes), `efs-mgmt raft-getchunks` on chunk 100 (gone); same with a 300 MiB file (every lane > 32 chunks) and with different content; add it to posix (`truncate_big_*`) and posix_persist; (d) then the apply drains per D25 and `apply truncate rc=` never appears in `efsd.log` during the 16× dd

**Evidence and limits.** servers `efsd.log` 20:56:41–43: `apply truncate rc=-2` ×16 inodes on every replica, `apply lane-fence rc=-2` ×661; client `slow-ok type=63 … status=0`; all 34 REPORTs `skip=8192 push_ms=0`. `TRUNC_IT_CAP` = 64 + 1 + 64×32×2 + 3, `efs_meta_apply_lane_fence` `it[1+32+32]`; `trunc_del_cb` → NOMEM at the 33rd chunk of a lane. Forbidden: raising the cap (a 1 TB file is 8192 chunks per lane); deleting chunk rows from the handler thread outside the entry; returning OK for an apply that wrote nothing



**Plan row 1 (in tree).** `apply_truncate_cmd` / `apply_lane_fence_cmd` return the apply's rc as the ring verdict (both on `host_apply`'s ring-only list, so a failed apply never halts the log); SETATTR surfaces EIO (W16 mapping) Gate: `tests/stress/truncate_big.sh` exits 3 (`TRUNC_ERR`, file unchanged) until D25, never 1 (a lie).

**Plan row 3 (in tree).** repro + gate for big-file truncate: `dd bs=1M count=10 conv=fsync` onto an existing 1 GiB and a 300 MiB file, same and different content, plus `truncate -s 0`; `stat`, `md5sum` through `iflag=direct`, `raft-getchunks` on chunk 100 Gate: exit 3 = truncate refused and file unchanged (today); exit 0 = all four PASS (after D25); exit 1 = a lie.

**Oct 6 note (nuc bare-metal cluster, posix `truncate_big_ftruncate_honest` / `truncate_big_o_trunc_honest`).** Two lane-spanning truncates in flight at once (the two tests at jobs ≥ 2) make the REFUSED file's subsequent reads fail with EIO for ~1 s while the lane settles; size and bytes stay intact and reads recover (solo runs are clean, verified 3/3 rounds). The tests are now `@serial` and `_verify_big_unchanged` retries reads through that window, so the lie gate is deterministic again. Whether the transient EIO is acceptable (vs EAGAIN/queued behind the in-flight truncate) is open — no bytes at risk, but an honest EIO on an intact file can still spook a reader that races a refused truncate.

## 0a · STALE replay that never converges (queue row 0a)

**STALE replay that never converges** (no W number; the earlier `W26` label here collided with the `fallocate` item)

**Follow-up steps.** (a) identify ino 116202 on 19810 from a KV copy and compare its chunk row with what the classifier (`write.c:880–968`) would replay; (b) repro: eight `dd bs=1M` into one mount, SIGINT mid-write, client stop, `EFS_DCACHE_TRACE=1`; (c) mechanical: the unmount drain names the inos and rc it abandons, and a `report-stale` round that replays the same single chunk > 16 times logs ino/ci/row gen/verdict once; (d) **DECIDED Oct 2 03:50Z = D27** ([decisions table](../status/decisions.md)): detect non-progress as repeated STALE against the *same* server generation (a gen advance is contention), stop the loop, keep the dirty bytes pinned, errseq-style EIO on `fsync`/`fdatasync`/`flush` of that inode, `client.sh stop` refused while a stalled rec exists, forced teardown reports ino/ci/off/len/cause per rec. Spill (Oct 1 23:00Z) and drop-after-N (Oct 2 01:45Z) were both rejected. **D28 (ask):** loss on forced teardown as written policy vs server-held write intents. Note: 2768 rounds prove a stalled operation, not a content mismatch — (a) decides which

**Evidence and limits.** fstor007 Oct 1 00:05: 2768 rounds of `report-stale: chunks=1 … committed=0 replayed=1` and then `UNMOUNT DATA LOSS … rc=-14 after 60s`. Acknowledged writes were discarded; the log cannot say whose. Forbidden: widening the 60 s drain, dropping the STALE check, or publishing a rec the server rejected


## W44 · the group leader's GC frag pass scans the whole prefix every 1.2 s (queue row 0h)

**W44 · the group leader's GC frag pass scans the whole prefix every 1.2 s and is 80 % of the leader's `efsd` cycles** (Oct 1 22:00Z review, §1b). **Step a IN TREE Oct 2** (`gc-pass … fsegs= fkeys= ftomb=`); **D26 implemented Oct 4 (dev cluster)** — the per-anchor pending-GC watermark gates the scan (a 5120-record `rm` drained at ~514 records/pass, then no `gc-pass` line for 10 idle min; the recovery derive consumed the table's 19664 GC-prefix tombstones once at startup); the idle-hour reading on the live table is owed to 19810 (down), as is the raft-tail 99.7 % GC_ACK check

**Follow-up steps.** (a) count what one `host_gc_frag_pass` scan visits (`EFS_GC_DBG`, plus a per-scan key/segment counter on the `gc-pass` line) on the live table while idle; (b) if the 205 ms empty scan is the 50–54 L0 segments, that is D12/D13's file count — bring the number to the user (**D26**); if it is tombstones under the GC prefix, the fix is a per-anchor "GC records pending" watermark the apply maintains so an empty pass costs one get; (c) gate: idle leaders show no `gc-pass` line (> 5 ms) for 10 min, `md_latency.py` medians unchanged, a 10 GiB `rm` still drains at ≥ today's 140 records/s per group

**Evidence and limits.** fcstor003 `perf-pid.txt`: tid 1056219 79.7 % of 780 K samples, flat `__memcmp_avx2_movbe` 24.9 % + `merge_scan` 16.7 % + `kv_seg_iter_next` 4.1 % + `kv_msrc_advance` 2.8 %; `gc-pass ms=205 frag=205 reap=0` every 1.2 s from 20:11 to 20:56 with nothing to collect; `kv-compact: end … l0=54 l1=149` once per 7 min. Forbidden: a longer `GC_LOOP_MS` to hide it (the rm drain rate is already 78 min per 160 GiB); scanning from a handler thread


## 0m · parent directory mtime/ctime must bump on entry create/unlink/rename/link (queue row 0m)

**Acceptance update, Oct 7.** Final NUC raw TSVs show all seven directory-time
tests and `peer_dir_mtime_bump_visible` passing on `128f6b7d`; this supersedes
the 6/7 single-client and peer-gate-pending statements below. The row is archived
as accepted on that build. [Raw evidence](../archive/queue-review-20261007-round2.md).


**Parent directory mtime/ctime must bump on entry create/unlink/rename/link/mkdir/rmdir — POSIX, and ruled a bug if missing (user, Oct 6 2026: "EFS is as much as possible POSIX compliant").** The open "bug vs. intended" question is closed: intended = POSIX.

**Status (Oct 6).** Code audit: the apply implements the bump for all six entry ops — create (`efs_meta_apply_create_file_op`), mkdir, unlink, link, rename (src and dst parents via `stamp_dir_items`), rmdir. A LOCAL directory's times ride the parent row in the same atomic batch; a HASHED directory's live in the dentry shard's dir lane (`dir_lane_stamp`, §7.4); `efs_meta_apply_getattr` reduces the `used_shards` lanes for a spread directory. **Unverified end-to-end:** client-side visibility (the getattr path, dcache, the 0j memo window) — the tests below are the arbiter, and a failure is a bug. **Gate ran Oct 6 (nuc bare-metal 3-node loopback cluster):** create/unlink/mkdir/rmdir/link and same-dir rename all bump same-client (posix `dir_times_*` 6/7 after the test-wait fix below); the only failure is `dir_times_rename`'s cross-dir dst parent — localized to the renaming client's attr invalidation and filed as **W57**. Note the Oct 5 session's `dir_times_bump_on_child_mutation` (posix) was added as a *failing* gate on the Oct 5 build — since the apply is verified correct, a same-client failure localizes the bug to the client's directory attr path (0j-adjacent).

**Tests in tree Oct 6.** posix: `dir_times_create`, `dir_times_unlink`, `dir_times_mkdir_rmdir`, `dir_times_rename` (same-dir and cross-dir, both parents), `dir_times_link`, `dir_times_write_no_bump` (the negative: content writes never touch the directory). posix2: `peer_dir_mtime_bump_visible` (A creates, B sees the directory's mtime+ctime advance). Every re-stat waits a full wall-clock second (the original 0.06 s — meant only to sit outside the 50 ms lookup-memo window, 0j — could not see a legitimate bump: creation rows show whole-second granularity, and on a fast loopback cluster the mutation lands in the same second as the baseline; 1 s crosses a boundary under any client/server clock offset).

**Follow-up steps.** run both suites on a live cluster; if a `dir_times_*` test fails, the failing op's stamp path (above) is the suspect; if only the posix2 test fails, look at the peer's getattr reduction or the client's directory attr caching. The spread-directory case (≥ `EFS_DIR_SPREAD_MIN` = 65536 entries) is not covered by these tests — add it when a spread-dir fixture exists.

**Forbidden.** declaring the bump "intended to be absent" to avoid the cross-shard stamp — §7.4 already solved that with dir lanes.


## W57 · cross-directory rename never refreshes the dst parent's attrs on the renaming client (queue row 0n)

**Oct 6 fix:** `f8fef814` refreshes both parents after committed rename, updates cached directory attrs from authoritative rows, and prevents dirty namespace state from overlaying stale directory attrs. The existing directory LOOKUP shortcut remains. NUC full POSIX jobs=4/jobs=1 and full posix2 PASS, including cross-directory parent-time phases.

**W57 · after any cross-directory rename (`mv a/f b/f`), the renaming FUSE client keeps serving the DST parent directory's pre-rename attributes indefinitely — mtime/ctime still show the pre-rename value ≥ 25 s later, a readdir of the dst parent does not refresh them, only another mount shows the truth — while the server stamps BOTH parents correctly (verified from a second mount on the same cluster: ns-resolution bumps). With a SIBLING layout (`a/f → b/f`, no shared ancestors) the SRC parent goes stale as well; with a parent→child layout (`d/f → d/sub/f`) the src parent survives because it is an ancestor of the dst path and gets refreshed along it (Oct 6 2026, nuc bare-metal 3-node loopback cluster, build `4e4c10ff-dirty`). This is the same-client failure 0m predicted would localize to the client's directory attr path.**

**Follow-up steps.** (a) fix: the rename reply/local-apply path must invalidate (or restamp) the client's attr state for BOTH parent inos, including when the dst parent is not on the source path — today only the components along the two rename paths are refreshed and the dst parent's dir-lane-reduced GETATTR row is never re-fetched; (b) audit `link(2)` into an already-statted directory for the same gap (`dir_times_link` passes, so likely clean — confirm in code); (c) gate: IN TREE Oct 6 — `tests/posix/posix_suite.py` `dir_times_rename` cross-dir phases (parent→child dst-parent check and sibling src+dst checks, each with a 0.1 s post-rename wait to sit outside the 50 ms rename-reply memo; both FAIL on the Oct 6 build) — then full posix jobs=4 and jobs=1 after the fix

**Evidence and limits.** nuc bare-metal cluster (3× efsd on loopback, efs-fuse mount): `dir_times_rename` fails on the dst-parent check with identical before/after ns values; a second mount on the same cluster shows the server bumping BOTH parents at ns resolution (e.g. `:55.641246728`) while the renaming mount still shows the pre-rename whole-second row 25 s later; a sibling-layout probe shows the src parent ALSO stale at +0.15 s and +2 s. Not the 0j 50 ms memo (persists ≥ 25 s); not `attr_timeout` (0 per 0j). Same-dir rename and the create/unlink/mkdir/rmdir/link bumps are all visible same-client (`dir_times_*` pass once the test waits cross a wall-clock second — creation rows were observed at whole-second granularity, so the old 0.06 s re-stat wait could not see a legitimate bump and failed spuriously on a fast cluster), i.e. the invalidation gap is specific to the rename's implicit dst parent (and a non-ancestor src parent). Forbidden: a time-based expiry that papers over the missed invalidation; routing every dir GETATTR through an extra RPC as the "fix" (0m's dir-lane reduction already makes the fresh answer cheap — the client just has to ask)


## W58 · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed (queue row 0o)

**`open("xb")` on a never-before-used name raised `FileExistsError`** (user, Oct 6 2026, xorinox cluster, build `3d3f17c2-dirty`): `tiny_files.py --depth 4 --files-per-folder 100 --total 1000000 --workers 16 /mnt/efs/tiny_files7/` died at `folder_000231/…/file_000049.txt`.

**Analysis (Oct 6, evidence on the cluster).** The script is innocent by construction: every leaf path is namespaced by a unique `folder_index`, each leaf is written by exactly one task, each file index once — no duplicate path is generatable, and `open("xb")` failed at file 50 of 100 in that leaf. The filesystem state contradicts the verdict: `file_000049.txt` **exists** (ino 623812, created 05:17:31.319Z, **size 0** — the create landed, the write never happened because the caller got an error). Server logs: xefs1 `05:17:31.733Z raft-host: create parent=419012 name=file_000049.txt rc=-13` (BUSY) — after the successful apply; **no server ever logged rc=-17 (EEXIST) for anything in the run**. Client log: `05:17:32.459Z inode-rpc: slow-ok type=67 attempts=2 saw_busy=1 status=0 us=1173252` ×4 — concurrent creates each taking >1 s through BUSY. Chain: the create applied (05:17:31.319), a retry saw BUSY (.733), and the application ultimately received EEXIST — a verdict the server never logged, so it was either fabricated client-side or returned by a retry whose opid no longer matched its own recorded verdict (I16: a replay in the window returns the recorded verdict). BUSY on *unique-name* creates is itself new behavior on this build — the uncommitted D25 intent probes make plain creates contend.

**Follow-up steps.** (a) in the client create path, check that every retry of one logical create carries the *same* opid and that a post-BUSY retry re-probes the opid window before falling through to the name-exists check; (b) decide where the EEXIST was born — server name check on a new opid, or a client-side lookup fallback after an ambiguous verdict; (c) repro is cheap: rerun the same tiny_files command into a fresh dir under 16 workers — recurred within 23k files on Oct 6; (d) a posix2 or stress gate: parallel `open(O_CREAT|O_EXCL)` of unique names must never yield EEXIST.

**Forbidden.** "Fixing" it by having the client swallow EEXIST on create retries (that hides real EEXIST for genuinely existing names); treating BUSY as terminal.


## W59 · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains (queue row 0p)

**W59 · `dd bs=1M count=1024 conv=fsync` on a FUSE mount died on the FIRST write with `No space left on device` (0 bytes) while the export showed 156 GiB free and every node disk 85 GiB free (Oct 6 2026, xorinox cluster, xefsct1). Not a capacity problem: the client maps its internal write-cache budget exhaustion to ENOSPC, and one of the two budgets — the fixed 8 MiB dcache metadata pool — never drains, so once it pins at its cap every subsequent write on that mount fails ENOSPC until remount.**

**Local implementation (Oct 6, uncommitted; no deployment).** Admission now logs metadata live/reserved/cap/request plus the failing budget leg before reclaim. Under metadata pressure, scan linked heap entries under their shard locks and free only body-less, published entries eligible for reuse; stalled records, uncommitted object/sequence identities, pins, dirty-list members, reclaim claims and present-extra accounting remain protected. The existing 8 MiB cap stays enforced. Retry after read-cache and metadata trim, after each successful REPORT drain, and with eight 100 ms backoff waits for concurrent reservations/REPORTs to release capacity. Exhausted local admission returns EAGAIN; allocation failure returns ENOMEM. Local allocator admission now returns BUSY rather than QUOTA, separating it from backend verdicts; genuine backend QUOTA, including during pressure drain, remains ENOSPC. Code review corrects the original suggestion to remap every flush-returned QUOTA: the drain does not call request reservation, and its QUOTA originates from backend PUT/inode RPC verdicts. This bounds the additional admission backoff, not the duration of a blocking REPORT RPC.

**Validation.** The actual allocator/cache/FUSE admission harness saturates metadata at 8 MiB with zero live body bytes, reproduces rejected admission, then proves admission succeeds and metadata falls to precisely the two protected unresolved/stalled entries. Removing their protection releases the remaining charge. It also checks local congestion returns EAGAIN and genuine pressure-drain QUOTA remains ENOSPC without discarding accepted bytes. Local memory gates and ASan/UBSan pass, as do D27 runtime/fault/STALE, controlled-stop, fold, fence-view and REPORT-pressure regressions. Live gates remain open: rebuilt client process/remount, multi-GiB sequential writes followed by more writes on the same mount, and posix jobs=1. Do not close W59 from local tests alone.

**Follow-up ENOMEM (Oct 6, xefsct1; local fix uncommitted).** User deployed the first fix and `dd bs=1M count=10024 conv=fsync` failed after 251 MiB. Read-only inspection confirms the new `546f8647-dirty` client process, ~2 GiB available host RAM and `write_buf` failures without a preceding admission-pressure log; subsequent close/flush succeeds. The actual allocator regression reproduces a reservation violation: reserve the normal budget, allocate flush scratch from the drain reserve, then the admitted writer's fully credited allocation fails because allocation rechecks total live+reserved against the normal cap. Fully credited allocations now use the already enforced combined hard+drain bound; uncredited/partially credited ordinary allocations still use the normal bound. Neither configured bound is raised. The regression fills both budgets, proves the reserved allocations succeed and the next unreserved allocation fails, then verifies complete release. It fails before the fix and passes afterward; local memory and D27 recovery suites plus allocator/cache ASan/UBSan pass. `write_buf` failure logging now uses POSIX strerror/errno, correcting its former interpretation of `-ENOMEM` as EFS `NOTEMPTY`. Live causation is consistent with this reproduced race, but the same deployed dd and subsequent writes remain required to close the gate.

**Follow-up steps.** (a) **log first, then fix:** the `dcache-pressure` line (`efs_fuse.c:2799`) prints only body counters (live/reserved/backing/limit/request) — add `g_metadata`/`g_meta_reserved` and which admission leg failed, so the exhausted resource is visible on the next occurrence; (b) **errno semantics:** a server QUOTA verdict (cluster genuinely full) → ENOSPC; a client-local admission failure → block with bounded backoff, worst case ENOMEM/EAGAIN — never ENOSPC (the two sites: `efs_fuse.c:2815` flush-returned-QUOTA and `:2824` drains-exhausted); POSIX apps (dd, rsync, git) treat ENOSPC as fatal-full and abort a transfer that could have proceeded; (c) **metadata reclaim:** body-less dcache entries are kept per published chunk for report/CAS base (`write.c` `dcache_find_meta`; `dcache_keep_on_drop` blocks dropping the unreported) and chain nodes stay charged when reused ("Heap nodes remain charged when reused", `bufpool.c`), so `g_metadata` grows to a peak and never shrinks — release or evict body-less entries once their report has landed (cap per-slot chains; reclaim reported-clean entries in the pressure path), so the 8 MiB cap cannot pin; (d) **relation to mem1** (sparse writes bypass reclaim; admission hard bound — in tree): mem1 bounds the body side; W59's metadata cap is the remaining leg — confirm the mem1 implementation does not already reclaim these entries; (e) **gate:** repro loop — a multi-GiB sequential dd through one FUSE mount (thousands of 128 KiB chunk entries), then keep writing while `df` shows free space: after the fix no ENOSPC; plus posix jobs=1 regression

**Evidence and limits.** xorinox 3-node libvirt cluster, xefsct1 FUSE mount, Oct 6 2026. Symptom: `dd if=/dev/urandom of=/mnt/efs/002.dat bs=1M count=1024 status=progress conv=fsync` → `No space left on device`, 0+0 records. Capacity checks all green: `df -h /mnt/efs` = 200G total / 45G used / 156G avail; `/data1` on xefs1-3 = 112G with 85G avail each, inodes 2%; no `nospc` in any efsd log. Client log `/mnt/efs-fuse-efs.log` shows two regimes. **(1) 06:16–06:18Z, genuine transient pressure:** `dcache-pressure live=266993664 reserved=0 backing=301989888 limit=335544320 request=1572864` in a minutes-long storm — live pinned at 254.6 MiB so `g_live + g_reserved > g_hard − bytes` (266862592), failing the byte leg (`bufpool.c:93`) — while `inode-rpc: slow-ok type=67` (= `EFS_MSG_REPORT_CHUNKS`) took 5.2 s with `saw_busy=1`: the server answered REPORT with BUSY, drains could not keep up, the 16 pressure-drain rounds were insufficient → `-ENOSPC`. Real congestion, wrong errno. **(2) 06:26Z, the dd, permanent failure:** `dcache-pressure live=0 reserved=0 backing=301989888 limit=335544320 request=5242880` — cache EMPTY, yet a 5 MiB admission fails. The byte leg provably passes (0 ≤ 256 MiB − 5 MiB); per-thread credits are clean (reserved=0); the only remaining leg is metadata (`g_metadata + g_meta_reserved ≤ 8 MiB − metadata`, `bufpool.c:94-95`) → `g_metadata` pinned at the 8 MiB cap. backing=301989888 is NOT a leak: 9 warm slabs × SLAB_BYTES (256 × 128 KiB = 32 MiB) that stay registered for process lifetime by design, and slab bytes do not count against admission. Arithmetic checks out: chunk size 128 KiB → a 1 MiB dd write = 8 chunks + 2 guard = 10 → request `10 × 4 × 128 KiB = 5242880`, metadata `10 × 1024`; the drain loop finds no dirty ino (live=0, `efs_dcache_pressure_ino` → 0, break) → `-ENOSPC` at `efs_fuse.c:2824`. Persistence: identical failures minutes apart on an idle cluster — a pinned counter, not congestion. The mount had accumulated entries over its lifetime (≈45 GiB written into the export earlier; posix 2client runs on xefsct1/2). Red herring: the user's first `dd bs=1m` failed on coreutils argument parsing (lowercase `m`), unrelated. Forbidden: returning ENOSPC for any client-local budget condition; raising the 8 MiB cap (or `EFS_DCACHE_HARD_BYTES`) as "the fix" without metadata reclaim; a time-based expiry that papers over the missing release

**Confirmation (Oct 6, nuc bare-metal cluster — second independent site, user-reported).** posix suite `/data1/efs/logs/posix-20261006-023609.tsv` (mnt=/data1/efs/mnt): **183/217 FAIL, 180 of them `Errno 28`**, starting with the very first write test (`basic_write_read`); `basic_empty_file` (no write) passes. The export is essentially EMPTY (`df -h /data1/efs/mnt` = 200G total, 4.2M used) and the data disk has 731 GiB free. Fuse log `/data1/efs/efs-fuse-mnt.log` (413 `dcache-pressure` lines) replays both regimes: 04:06–04:10Z genuine byte pressure (live 247–263 MiB during heavy write tests); the 05:03Z and 06:05Z suites still pass 214/217 with zero ENOSPC; from **06:18:15Z** the signature flips to `live=2883584 reserved=0 request=1572864` — 2.75 MiB live, the byte leg trivially passes, only the metadata leg can fail — and the 06:18Z suite fails 172 tests ENOSPC, the 06:36Z suite 180. Once pinned it never recovers (two suites 18 min apart, idle cluster). Rules out anything specific to the libvirt VMs or to a filled export: the budget accumulates over the mount's lifetime across suite runs, then bricks writes permanently.


## W60 · a concurrent sequential reader's prefetch queue can park the entire read-side body budget; demand reads then fail NOMEM, surfaced as EIO (queue row 0q)

**W60 · `rg --hidden --no-ignore --stats 'search text' /mnt/efs/` on xefsct1 (Oct 6 2026, xorinox cluster, build `b4a75492-dirty`) emitted thousands of `Input/output error (os error 5)` on tiny files (`tiny_files*/folder_*/level_4/*.txt`) while all three efsd logged zero errors. The client log holds 19,112 `efs-fuse read: out of memory (efs_rc=-2)` lines between 21:58:16Z and 22:57:01Z (per-10-min: 950, 6433, 4879, 2085, 1665, 1704, 1396 — onset ~40 min into the scan, decay as rg moved off the large files). The EIO is `efs_fuse_read_ino` mapping any `efs_client_read` error to -EIO while the log keeps the real code (`efs_fuse.c:2166-2169`). Root cause established by controlled A/B reproduction (below): queued prefetch buffers can hold the full 256 MiB read-side body budget, and the demand-read scratch allocation has no trim/wait/retry — best-effort readahead starves real reads.**

**Mechanism (code).** The shared body pool (`bufpool.c`) admits demand reads only up to `EFS_DCACHE_HARD_BYTES` (default 256 MiB; the 64 MiB drain reserve is write/drain-only — `efs_buf_alloc` uses `g_hard` without a full reservation or drain context) and charges every buffer a minimum of one chunk (`buf_charge`, 128 KiB — a 3-byte file costs a full slot). A sequential reader arms prefetch after two in-order reads (`t_seq_run >= 2`, `read.c:1725-1732`); `prefetch_ahead` (`read.c:1508-1553`) allocates one pool buffer per queued chunk (`:1536`) into the GET pool, which queues up to `GET_POOL_QDEPTH=32` × `GET_POOL_N=64` (`:1194-1204`) = 2048 jobs ≈ 256 MiB — 100% of the read budget. Prefetch's own alloc failure is silent (`:1537-1539`). Any non-chunk-aligned read (every sub-chunk tiny file, every tail) needs a scratch buffer at `read.c:1797` and fails `EFS_ERR_NOMEM` (`:1802`) with no recovery — unlike the write path, which trims the read cache, drains and retries (`fuse_write_admit`, `efs_fuse.c:2900-2978`, the only `efs_rdcache_trim` caller). The log line blames the victim; the culprit is invisible.

**Reproduction (controlled A/B, Oct 6, xefsct1).** Throwaway second mount of the same cluster with `EFS_DCACHE_HARD_BYTES=33554432 EFS_DCACHE_DRAIN_BYTES=33554432` (64 MiB total = 512 chunk buffers): tiny-file loop alone (`head -c 4` × 3000 files of `tiny_files9`) → **0/3000 failed**; same loop with `cat 001.dat > /dev/null` (10 GiB sequential) concurrently → **2000/2000 failed**, first failure on file #1, 2000 NOMEM lines. Ruled out by measurement: kernel OOM (none in the window), staging-table cap (no `EFS_CLIENT_META_MB` line), rdcache body accumulation (reads leave the pool empty when no sequential reader runs), chunk-size geometry (128 KiB, confirmed by the `request=5242880` admission arithmetic). Full write-up with evidence: [fuse-memory.md](../status/fuse-memory.md) §"Read-path ENOMEM storm".

**Follow-up steps.** (a) read-path backpressure: on scratch-alloc failure, trim clean unpinned rdcache bodies and wait briefly (bounded), mirroring `fuse_write_admit`, instead of failing the read; (b) prefetch admission watermark: best-effort prefetch must never consume the last of the budget — skip submission when live+reserved exceeds a fraction of `g_hard`, and/or cap queued prefetch bytes well below the demand budget; (c) distinct log lines for prefetch-drop vs demand-read failure (the single `read: out of memory` line hides the pressure direction); (d) consider sub-chunk body charging so tiny-file reads do not pay a full 128 KiB slot; (e) gate: the 64 MiB A/B must flip to 0/2000 with the sequential reader running, and a full-root `rg` over the xorinox tree (four ~10 GiB `00*.dat` + `tiny_files2..9`) must complete with zero `efs_rc=-2` lines. Cousin of W59's errno semantics: W59 maps client-local write admission to ENOSPC, W60 maps read admission to EIO — both should be backpressure, not terminal errors.

**Forbidden.** Raising `EFS_DCACHE_HARD_BYTES` (or shrinking the GET queue) as "the fix" — that moves the onset, the starvation remains; disabling or serializing prefetch (it carries sequential throughput — DIO arrives as 128 KiB requests, see the Oct 1 read review); an unbounded retry spin on the demand path.


**Implementation and NUC gate (Oct 6).** `73ce8aaa` adds atomic half-budget
speculative/cache-body admission and clean-cache trim plus bounded demand
retry. Same four-node NUC cluster, same 32 MiB normal + 32 MiB drain budget,
same 512 MiB sequential fixture and 2000 tiny files: pre-fix read/allocator
code **2000/2000 failed**, fixed code **0/2000 failed**, zero read-NOMEM log
lines. Concurrent admission, credit/drain protection and pending/pin ownership
regressions pass. Xorinox full-tree acceptance and optional diagnostics/
sub-chunk charging remain follow-ups.


## P2.3 · W23 — the stalled-compactor test (performance plan row)

**Item.** **W23** stalled-compactor test

**Status.** **test in tree, measurement owed (Oct 5Z, dev cluster).** The fault hook (`EFS_FAULT_COMPACT_STALL` + `/tmp/efs/fault` in `compactor_main`, `EFS_FAULTS=1` only), the `kv-obs` sample line, and `tests/measure/w23_stalled_compactor.sh` are in the tree and built; deploy gate PASS (16/16 unit suites; the hook verified absent from the normal binary). The one run attempt died at setup — scratch dir on a root-owned path, EACCES before mkfs (fixed in the script after the attempt, along with its results-path bug); no samples, no numbers. Rerun is one command (`results/measure/20261005-014440-w23-stalled-compactor/SUMMARY.txt` has the full state). The 19810 dependency does not apply: the text puts this test on a private 3-node cluster, which the dev cluster provides.

**What changes (server).** the test written in [W23](#w23--server-the-apply-path-blocks-on-l0-back-pressure-and-compaction-rewrites-the-table-to-absorb-a-few-mib) "correction": compactor stalled by a fault, measure memory and lag bound

**Gate / done when.** numbers in `results/measure/`; feeds W51/D30

**Forbidden.** —




## Why there is no N = 2f mode

At N = 2f exactly (e.g. 4 nodes wanting f=2), f permanent losses leave a
minority of survivors. Every committed entry still has ≥1 surviving copy
(Q + f > N), so the *bits* survive — but plain Raft cannot re-establish
authority without a majority, and **no local merge rule over the surviving
logs can reconstruct the committed prefix.** Commitment information died
with the majority.

The tempting merge — "per log index, the highest-term entry wins" — is
unsound. Raft's log-prefix properties hold only *within logs produced
through Raft*; they do not license constructing a new log by independently
selecting entries per index. A legal situation:

```text
survivor A:   index 5  term 10  X        (uncommitted branch)
survivor B:   index 5  term 9   Y        (committed earlier)
              index 6  term 11  Z        (appended by a term-11 leader
                                          whose log contained Y at index 5,
                                          elected by a majority excluding A)
```

The merge produces `X@5, Z@6` — a log that **never existed**. `Z` was
created on a prefix containing `Y`, not `X`. Worse, the merge *drops* `Y`,
which was committed and possibly acknowledged to a client. Operation-ID
idempotency does not fix this: the problem is not duplicate application but
a synthesized state-machine history no leader authorized.

Raft is deliberately majority-based; the paper provides availability only
while a majority remains, and production systems (e.g. etcd) treat
permanent quorum loss as disaster recovery, not ordinary log
reconciliation. EFS therefore does not offer the mode: permanent majority
loss is **disaster territory** (an operator procedure over surviving state),
not an efs operating mode. If a 4-node/f=2-style disaster-survivability
guarantee is ever genuinely needed, it is designed as a separate protocol
with its own proof — not bolted onto Raft by prose.
