# orcd-mini: deferral, expiry fixtures, refused-startup fence — 2026-10-17

**Deployed to orcd-mini as `e04a80f63cc4`.** The core, extras, two-client,
durability and private GC POSIX suites all PASS. `posix_expiry` passes end
to end but is intermittent: 2 of 4 runs on the deployed build, 2 of 2 on
the commit source.
This closes the items left open on the
[9099380f3 page](orcd-mini-deploy-9099380f3-20261017.md), plus one fault
found while deploying.

1. **Cleanup deferral works live** (`f91afa40a`).
2. **`posix_expiry` passes end to end** (`f91afa40a`): 134 PASS lines; it
   stopped at 13 before.
3. **New: a refused startup no longer fences the storage roots**
   (`e04a80f63`). orcd-mini's roots, fenced by this bug at 22:07–22:20 UTC,
   were repaired with the user's approval.

## 1. Cleanup deferral under Raft apply (`f91afa40a`)

- **Defect** (from
  [GC/query catalog](orcd-mini-gc-query-catalog-20261017.md)):
  - Raft apply stages an entry's KV writes (`efs_kv_lsm_apply_begin`) and
    commits them at `apply_end`.
  - The cleanup scope opened inside lane-sweep/reap-done apply had closed by
    then, so reap deletes still scheduled catalog refreshes.
  - Live symptom: `deferred_cleanup` stayed 0.
- **Fix** (`src/kv/kv_lsm.c`):
  - The staged scope records which keys' last write was a cleanup delete,
    and passes per-item marks to the committed batch.
  - A later ordinary write of the same key, or an aborted entry, clears the
    mark.
- **Tests:**
  - `test_kv_lsm` `test_cleanup_scope_apply`: commit after the scope closed,
    put/delete overrides, aborted entry.
  - `test_meta_apply` `test_reap_cleanup_deferred` now runs lane sweep and
    reap through `apply_begin/end`.
  - Negatives: dropping the commit marks fails 2+2 checks; disabling
    last-write-wins fails the override and abort checks.
- **Live result** on `f91afa40a223`, 20k tiny files created and deleted:
  - `deferred_cleanup` rose 128 → 32,896;
  - after the deletes were published (22:26:52), no catalog publish ran for
    5 min 20 s while only GC worked. Before, one ran per minute.

## 2. Expiry suite fixtures (`f91afa40a`)

- **Cross-group choice:**
  - Nested `mkdir` keeps the parent's metadata group (mkdir group affinity),
    so no child of `expiry-gates` can be in the other group.
  - The cross-group and coordinator directories are now created at the
    root, where placement is scattered, and renamed under the policy root.
    The inode keeps its group.
- **Clean stops:**
  - The expiry restarts and offline members now stop with SIGTERM.
  - After a SIGKILL every root fails COMMIT closed (W71, "recovery
    uncertain") until verified recovery. The following gates write new
    files, so they hit EIO.
  - Crash-restart coverage of expiry therefore stays with W71.
- **Result:** the private four-node `posix_expiry` passes, 134 PASS lines
  (on the commit's source and on the deployed release).

## 3. Refused startup fenced the roots (`e04a80f63`)

### What happened

- At 22:07 UTC the restart onto `f91afa40a223` refused to start: "Storage
  quota 800.00 GiB exceeds safe maximum 726.72 GiB".
  - Since 20:36 each store had grown from 642.9 GB to 723.1 GB, about 240 GB
    in total, from user workloads.
  - `/data1` had dropped to 657 G free.
  - Every build applies this check.
- The quota was lowered to **720 GiB** with the user's approval:
  devops `clusters/orcd-mini.env`, synced to node9901.
- The 22:20 start then logged, on every root: "recovery uncertain; COMMIT
  returns EIO until verified recovery or fresh format".
  - The single-mount POSIX suite failed 12 fsync/flush cases with EIO.

### Cause

- `server_put_sync_init` rewrites each root's `.efs-durable-epoch` as the
  new boot, unclean, before the quota, listen and join checks.
- A refused start exits without `server_put_sync_stop`, so the next start
  saw an unclean previous stop.
- That boot never served anything.

### Fix

- At attach, the record's previous bytes are kept.
- An exit handler registered after attach restores them verbatim (or
  removes a record this boot created) when startup ends before serving and
  no PUT was acknowledged.
- Unchanged, still fail-closed:
  - a crash (no exit handler runs);
  - any exit after serving began;
  - an orderly stop;
  - a boot that acknowledged a PUT;
  - an unreadable previous record.
- **Test:** `test_store_durability` `test_startup_abandon` covers a fresh
  root, a restored clean predecessor, serving, touched and stopped boots,
  and a corrupt record.
- **Negatives:** disabling the handler fails line 124; ignoring
  acknowledged PUTs fails line 151.

### Repair of orcd-mini (user-approved)

1. `fuser` on both mounts: no users.
2. Client stop, then orderly `stop.sh`. Each root logged "stopped UNCLEAN
   (barrier failed)": the final barrier is refused while fenced, but the
   periodic barriers had run.
3. `sync -f /data1`.
4. `repair_epoch.py --apply` (kept outside Git, at
   `node9901:/data1/efs/prof/open-live/repair_epoch.py`):
   - validates each record (v2 magic, zlib CRC-32 matches, `clean=0`);
   - backs it up;
   - sets `clean=1`, keeping boot 13, seq, time, verifier and root count,
     with a new CRC.
5. Original records are kept in `node9901:/data1/efs/prof/open-live/epoch-backup/`
   (sha256 prefixes: n1 `1db57545997d5df9`, n2 `4208437680314e1d`,
   n3 `cb49fd0c00ead5a8`).

Justification: the only unclean interval was the refused boot, which served
nothing. Everything written afterwards was flushed by the periodic barriers
and the `syncfs` before the repair.

After the switch to `e04a80f63cc4`:
- boot 14 started with no "recovery uncertain";
- safe maxima are 736–739 GiB against the 720 GiB quota;
- an fsync probe on node9900 succeeded.

## Build and deploy

- **Source:** devel `e04a80f63`, clean `git archive` without `results/`,
  build ID `e04a80f63cc4` (`v0.3.2-pre-alpha-613-ge04a80f63`), private
  libfuse.
- **node9901** `/data1/efs/releases/e04a80f63cc4`:
  - efsd `fdc0467dbddafec49282729555dfdcd60cf9f05a4db1a71a262930227892fb2e`;
  - efs-fuse `87eac83d2402ea0678fac4b904ae66fe307a969015106b4e9b0ea49d1296ab85`.
- **node9900** efs-fuse
  `1109bae6fae8c0af738fee399e930571cc3201f5bf95211c98b92b55de0c4a1f`.
- **Quota:** 720 GiB per node (was 800). Usage is about 673 GiB per node.
- **Environment:** RDMA `mlx5_0`, `--direct-io`, 60 s catalog refresh
  default.

## Results on `e04a80f63cc4`

| Suite | Result |
| --- | --- |
| Core POSIX, one mount (`smoke.sh`) | **PASS 216/216** |
| Core vs local XFS (`compare.py`) | **PASS** |
| `posix_extras` | **0 fail** (2 PASS, 3 SKIP: mmap ENODEV; two `.efs` freshness checks "not indexed yet") |
| Two-client | **PASS 67/67** (449 s) |
| Durability (`smoke-persist.sh`) | **PASS 27/27** |
| Private `gc_reclamation --posix`, TCP | **PASS** (71 PASS lines) |
| Private `gc_reclamation --posix`, RDMA | **PASS** (71 PASS lines) |
| Private `posix_expiry` | **2/4 PASS** (134 PASS lines when passing); 2 intermittent failures, see below |
| `make test` (fix tree on node9901) | 768 commands, 5 environmental failures |

The `make test` failures are the 4 LeakSanitizer tests, which pass with
`ASAN_OPTIONS=detect_leaks=0`, and the doc-link check, which fails only
because the tree lacks `results/`.

### `posix_expiry` intermittent failures

Each failure happened once, in different steps:

1. **`expiry_lifecycle.py:66`.** `efs-mgmt expiry show PATH --mount` exits 1
   right after the file is created.
   - `expiry_lookup` (`INODE_LOOKUP_PATH`) maps any non-OK status to BUSY,
     and neither the tool nor the test retries.
   - This is a race in the test or tool. It is not the query catalog, and
     not this change.
2. **`expiry_lifecycle.py:209`.** fsync returned EIO on the first file
   created after the coordinator-loss step's offline member (node 2)
   rejoined.
   - The fixture logs were removed by the runner, so the cause is
     unconfirmed.
   - One candidate: the 20 s SIGTERM wait in `expiry_offline` fell back to
     SIGKILL, which fences that root (W71).
   - Reruns now keep the fixture on failure.

Neither reproduced in the two later runs.

## Raw output (outside Git)

- node9901 `/data1/efs/logs/`:
  - `posix-20261017-190956.tsv`;
  - `posix-extras-20261017-191009.tsv`;
  - `persist-*-2026101719*.tsv`.
- node9901 `/data1/efs/prof/open-live/`:
  - `e04/` (private gates). sha256 prefixes: `gcr-tcp.txt` `b2f7c8ffeccb82df`,
    `gcr-rdma.txt` `3c2f48103018b45a`, `expiry.txt` `c007d0b1f0f857be`
    (fail, line 209), `expiry2.txt` `36a5bd722dbcab1c` (fail, line 66),
    `expiry3.txt` `193a7015b2203db5` (PASS), `expiry4.txt`
    `c79d04953a14f4ad` (PASS);
  - `expiry5.txt` (commit source, 134 PASS, sha256 prefix
    `d95ded0751f74d88`).
- Two-client TSV on the driver Mac.

## Remaining

- `posix_expiry` flakiness: add a bounded retry to `efs-mgmt expiry show`
  (or the test) for transient BUSY, and diagnose the post-rejoin EIO with a
  kept fixture.

- Disk headroom: orcd-mini is at about 93.5% of its 720 GiB quota, with
  about 47 GiB per node left. `/data1` growth sets the cap.
- W71 (verified recovery after a crash) is still open. A real crash still
  fences the roots until it exists.
