# Raft log rotation off the metadata path — 2026-10-17

**Rotation stall removed. Implementation, unit tests and private-cluster
A/B PASS; orcd-mini live NOTRUN (deploying needs the user's go-ahead).**

- Raft log rotation no longer runs under the host or disk locks. Its
  longest `d->mu` hold fell from the whole rotation (5.3 s here, 12.7–14.0 s
  on orcd-mini) to 1.1–3.8 ms.
- On the private cluster, the longest metadata stall fell from **5.46 s**
  (inside the rotation) to **4.36 s**.
- That 4.36 s, and every other stall over 1 s in both arms, overlaps a
  **snapshot export**. That is a separate, pre-existing stall and the next
  gate (see Limitations).

Linked gate: the server-side stall recorded in
[the stage-evictor report](orcd-mini-stage-evict-20261017.md#under-the-users-workload-shape)
("EIO, and why it is not this change"). Roughly every 7.5 hours at the user's
write rate, orcd-mini's Raft log reaches about 5.8 GB. All three daemons then
rotate it at once and metadata stops for 12.7–14.0 s. Client inode RPCs
exhaust their 16-retry / 10.3 s budget, so applications get EBUSY on reads
and EIO on close.

## Why rotation stalled everything

- **Trigger.** The host pump (`host_maybe_snapshot`, `src/server/raft_host.c`)
  calls `efs_raft_snapshot` → `save_snap` while holding `h->mu`.
  `disk_save_snap` sets `check_at = 0`, so `raft_log_maybe_rotate_locked`
  checks right away. Once the file is 3× its live size, it rotates.
- **Locks held.** The whole rotation ran under `d->mu` (the disk store), and
  through `save_snap` also under the host's `h->mu`. Proposals, ReadIndex
  reads, apply, AppendEntries and every other group's appends wait on one or
  both.
- **Work under those locks.** For 1.5 GB of live records, `raft_log_rotate_locked`:
  - rewrote every live record into `raft.log.new`; evicted commands were
    read back with one `pread` per entry from the old file;
  - ran `fsync`, `rename` and a directory `fsync`;
  - walked every index entry to reset `end_off`/`payload_off`.
  - `raft_log_maybe_rotate_locked` also walked every entry to compute the
    live size.
- **Evidence.** In node9901 `/data1/efs/logs/efsd-n{1,2,3}.log` at
  2026-10-17T06:47:07–06:47:10Z, `raft-log: rotate old_bytes=5.8e9
  live_bytes=1.5e9` took `elapsed_us` 12.70 / 14.04 / 12.78 s. `raft-snap:
  advance` (the pump's whole snapshot call) took 12.94 / 14.35 / 13.12 s.
  All three replicas snapshotted within six entries of each other
  (39,332,748–39,332,754).

## Change

`src/raft/raft_log.c`, `raft_disk.c`, `raft_disk_internal.h`,
`include/efs/raft_disk.h`.

- **Off the caller's thread.** In SYNC mode (production), each disk store
  gets a rotator thread. `maybe_rotate` only signals it, so `save_snap`
  returns at once. NOSYNC mode (tests, simulator) keeps the deterministic
  inline rotation.
- **Short lock steps.** The new file is written in two parts:
  - **image:** the live state at a capture point X (old-file bytes [0, X)).
    Entry metadata is read under `d->mu`, 8192 entries per acquisition.
    Frames are copied verbatim from the old file outside the lock, with
    framing, index, term and CRC checked.
  - **suffix:** old-file bytes [X, end), copied raw outside the lock until
    under 256 KiB remain.
  - **swap:** the final step runs under `d->mu`. It copies the last piece,
    then runs `fdatasync`, `rename` and the directory `fsync`, and swaps
    the fd.
- **Why image + suffix replays correctly.** Replaying the suffix on top of
  the image is the same fold that took the state at X to the current state.
  If an entry not yet imaged is truncated, overwritten or compacted away,
  the attempt returns `EFS_ERR_STALE`. The old file stays authoritative, and
  the rotator retries after 4 MiB more log.
- **Capture waits for in-flight mutators.** A store mutator logs a record,
  can drop `d->mu` for `fdatasync`, and only then applies the record to
  RAM. A capture inside that window would find the record in the file but
  not in RAM. The capture now waits for in-flight mutators to drain, and
  holds new ones off for that moment (`raft_mut_begin/end`). The regression
  test found this race. The old code had a narrower form of it: a sync
  waiter woken after `syncing = 0`.
- **No O(entries) work at the swap.** Offsets are logical (physical +
  `d->base`). Suffix records keep their logical offsets, so the offsets of
  everything appended after X stay valid. Imaged entries carry their image
  position tagged with the rotation's generation (`img_off`), and the swap
  makes those tags current in O(1).
- **Watermarks never move backwards.** `efs_raft_disk_synced_bytes` is now
  logical, so a watermark read before a swap can never cover a record
  written after it. The host reads it and `covered_index` separately under
  `h->mu`, while the rotator swaps under `d->mu` alone.
- **Write-behind.** On Linux, the rotation writes its file back in 8 MiB
  `sync_file_range` steps. A large dirty set therefore never builds up for
  Raft log fsyncs on the same device to queue behind.
- **Live-size check is O(groups).** It uses `bytes_all`; there is no index
  walk.
- **Durability order unchanged.** All old-file bytes reach the new file and
  are fdatasynced before the rename. The directory is fsynced before any
  append lands in the new inode. A failed directory fsync fences the store
  (`io_failed`), which the old code ignored. The explicit
  `efs_raft_disk_rotate` keeps its `EFS_ERR_BUSY`-while-syncing contract.
- **Cost.** One extra `u64` per in-memory index entry (40 → 48 bytes).

## Tests

- New `tests/test_raft_rotate_stall.py`, in `make test`. It rotates ~96 MiB
  of live log (2 groups × 12,000 × 4 KiB). Meanwhile a prober thread takes
  `d->mu` in a loop and appends to group 2.
  - **Asserts:** the rotation's longest `d->mu` hold < 100 ms, and the
    prober's longest wait < 100 ms.
  - **Also covers:**
    - a truncate + rewrite inside the captured range during the copy
      (STALE, then a clean retry);
    - reads of every entry through the swap, with the command cache dropped;
    - a second rotation over imaged entries;
    - logical watermarks (a pre-swap `synced` must not cover a post-swap
      append);
    - the background rotator scheduled by `save_snap`, with `save_snap`
      returning in < 100 ms;
    - SYNC and NOSYNC cold replay of every entry.
  - **Results:** 20/20 on node9901 and 10/10 on macOS. Typical run: rotation
    0.4–0.7 s, longest hold 0.1–4.7 ms, longest prober wait < 5 ms.
  - Before the in-flight-mutator fix it failed 5 of 8 runs with a lost
    entry (`bad entry g=2 … rc=-7`).
- Existing rotation tests pass unchanged except where noted:
  - `test_raft_store` (rotation shrink, reserved tail, snapshot base 0,
    bounded cache);
  - `test_raft_sync_rotation` (stalled fdatasync pins rotation, exact
    watermarks);
  - `test_raft_append_offsets` (offset negative still rejected).
- `make test` on node9901, build `rotnew`, final Raft code, each recipe
  command run individually:
  - 352 PASS, including `test_raft_rotate_stall`.
  - 4 more failed with "LeakSanitizer has encountered a fatal error"
    (ptrace/LSan on node9901) and PASS with `ASAN_OPTIONS=detect_leaks=0`:
    `test_readindex_pair`, `test_raft_ack_window`, `test_host_outbox` and
    `test_stage_evict_lru`.
  - 1 FAIL: `docs/check-architecture.py`. The remote copy had the parent
    commit's stale `architecture-full`; this commit regenerates it, and the
    gate passes locally (5/5).
  - An earlier full run, before write-behind, gave the same result.

## Private-cluster A/B (node9901)

**Environment.**
- **Host:** node9901, AMD EPYC 9474F, kernel 5.14.0-503.38.1.el9_5.
- **Stores:** `/tmp`, on XFS over `md0` (RAID1 of two NVMe partitions).
  `/data1` is below efsd's 20% free-space reserve.
- **Cluster:** 3 efsd on 10.1.99.1:17631–17633, RDMA `mlx5_0` (InfiniBand),
  `--direct-io`, `--quota 1000G` (200G ran out at ~3.3M tiny files in run
  `new`). Default storage engine; `EFS_MD_RAFT_N=3`.
- **Client:** one efs-fuse mount of the same build, built against the
  private libfuse.
- **Snapshots:** `EFS_RAFT_SNAP_BYTES` set to 128 MiB in both builds by
  `build`. Snapshots then come every ~4.5 min per group. Rotation fires once
  the file is 3× live, about 1.4 GB here, ~25 min into a run.
- orcd-mini (ports 17432–17434), w100 and the mounted clients were not
  touched.

**Builds.**
| Label | Source | efsd sha256 |
| --- | --- | --- |
| old, old2 | `859b2b9eb` (devel parent; Raft code identical to orcd-mini's `4aff22c62`) + 128 MiB snapshot | `d80a7f457e96bb7e…` |
| new | this change before write-behind + 128 MiB snapshot (binary since replaced) | — |
| new2 | this commit's Raft code + 128 MiB snapshot | `de51eb46b1c3ef30…` |

**Workload.**
- 16 Python writer threads create 256-byte files: 2,000 per directory, one
  directory tree per thread.
- One probe creates, writes, closes and stats a file every 50 ms; it records
  the latency of each.
- 30 minutes per arm (the first `old` ran 15 min).
- Stall attribution: a probe counts toward a window when its interval
  overlaps a node's rotation or snapshot export.

| Run | Files/s | Probe p99 / p99.9 | Longest stall | Probes > 1 s | Rotations (all 3 nodes, same time) | Worst probe overlapping a rotation | Worst probe overlapping an export only |
| --- | --- | --- | --- | --- | --- | --- | --- |
| old (15 min) | 2,144 | 109 / 268 ms | 2.21 s | 4 | none (log reached 1.15 GB) | — | 2.21 s |
| **old2** | 1,750 | 102 / 384 ms | **5.46 s** | 9 | 1.40 GB → 362 MB in 5.26–5.30 s, all under the locks | **5.46 s** | 4.68 s |
| new | 1,904 | 154 / 431 ms | 4.08 s | 7 | 1.43 GB → 389 MB in 5.06–5.21 s, hold 48–69 ms | 4.08 s | 3.78 s |
| **new2** | 1,794 | 113 / 369 ms | **4.36 s** | 10 | 1.42 GB → 378 MB in 5.39–5.64 s, **hold 1.1–3.8 ms** | 3.63 s | **4.36 s** |

- **old2:** the one probe that overlapped the rotation started at
  18:31:12.9Z. It completed when the rotation ended at 18:31:18.2Z, so it
  was blocked for the whole rewrite. Same mechanism as orcd-mini's 14 s,
  scaled to 362 MB live instead of 1.5 GB.
- **new and new2:** the rotation overlapped that group's snapshot export
  (both start at the same snapshot point). The probe's worst latency in that
  window (4.08 s, 3.63 s) is within the range of export-only windows in the
  same runs (3.78 s, 4.36 s).
  - The rotation's direct cost is its lock hold. In `new`, the final
    `fdatasync` of hundreds of dirty MB ran under `d->mu` (48–69 ms).
    Write-behind cut it to ≤ 3.8 ms in `new2`.
- **Other stalls:** every stall over 1 s in all four runs overlaps a
  snapshot export. Probes outside rotations and exports peaked at
  424–876 ms.
- **Errors:** none in the runs. `new` hit ENOSPC against its 200 GiB quota
  in its final minute (182k writer errors, 1,709 probe errors, all errno
  28). The quota was raised to 1000 GiB for `new2`/`old2`.
- **Scaling:** under the old code, the stall grows with the live log:
  5.3 s for 362 MB here, 12.7–14.0 s for 1.5 GB on orcd-mini. The new
  lock hold depends on chunk size (8192 entries) and the final tail
  (≤ 256 KiB plus one fdatasync), not on log size.

**Raw output, outside Git.** node9901 `/tmp/efs-rot/run/{old,old2,new,new2}/logs/`
(`efsd-n{1,2,3}.log`, `fuse.log`, `probe.csv`) and `/tmp/efs-rot/*.out`.
`probe.csv` sha256 prefixes:
- old `0cbdbe3f7d1c58e0`
- old2 `e28382153b0b29ad`
- new `1f97cc18e9902543`
- new2 `7e1dd41af2cba034`

`make test` per-command results: `/tmp/efs-rot/test-results.txt`.

## Limitations and remaining gates

- **orcd-mini live NOTRUN.** Deploying needs the user's go-ahead and a
  daemon restart. A full restart currently fails efsd's 20% free-space
  check on `/data1`.
  - Expected there: the 12.7–14.0 s rotation stall disappears, but the
    concurrent group snapshot export remains: 7.5–8.4 s for 3.1 GB, with an
    unmeasured stall.
  - Whether that stays under the clients' 10.3 s retry budget is unproven.
- **Snapshot export stall: the next gate, not addressed here.**
  - **Seen:** every probe stall over 1 s in both arms overlaps a
    `raft-snap: start … end` export, growing with snapshot size: 1.3–2.2 s
    at 0.35–0.52 GB, 3.6–4.7 s at 0.7–0.87 GB.
  - **Client view:** inode RPCs saw BUSY and completed after 3.6–4.8 s
    (`inode-rpc: slow-ok … saw_busy=1`).
  - **Server view:** KV L0 sat at 64–80 files during the export.
  - **Shared by all replicas:** all three replicas export at the same
    snapshot index at once.
  - **Candidates:** stagger snapshot points across replicas; write the
    export with the same write-behind; find the BUSY source.
- **`save_snap` itself:** 220–313 ms under `h->mu` on orcd-mini even
  without a rotation (`raft-snap: advance group=0 … elapsed_us=220484`).
  - It is mostly the retained-window `memmove` in `raft_group_snap` over the
    mmap'd index, plus residency resets.
  - This change makes each index entry 8 bytes larger, so that memmove grows
    by ~20%.
  - An O(1) window drop (a head offset instead of a memmove) is a follow-up.
- **Staggering rotation across replicas** is no longer needed for locks.
  The rotation's only remaining cost is I/O. It is paced by 8 MiB
  write-behind, and all three replicas still rotate at the same snapshot
  point.
- **Single run per arm,** with a 128 MiB snapshot window instead of 512 MiB.
  The baseline probe ran in Python through FUSE, so its resolution is
  ~10 ms.
- **macOS:** no `sync_file_range`; write-behind is a no-op there.

## Reproduce

On node9901, from a checkout of this commit (`$NEW`) and of its parent
`859b2b9eb` (`$OLD`):

```bash
scripts/raft_rotate_stall_ab.sh build $OLD 128
scripts/raft_rotate_stall_ab.sh build $NEW 128
scripts/raft_rotate_stall_ab.sh run $NEW-snap128 new 1800
scripts/raft_rotate_stall_ab.sh run $OLD-snap128 old 1800
```

- `build` copies the tree to `<src>-snap128` and sets
  `EFS_RAFT_SNAP_BYTES` to 128 MiB there. That way snapshots and rotations
  happen within a 30-minute run; production is 512 MiB.
- `run` starts the private cluster on ports 17631–17633 under
  `/tmp/efs-rot/run/<label>`. It runs 16 writer threads and the 50 ms probe,
  prints the summary and per-event attribution, then stops the cluster and
  deletes its stores. Logs and `probe.csv` stay in `/tmp/efs-rot/run/<label>/logs/`.
- `analyze <logs dir>` re-scores a finished run. Each probe is attributed
  to a rotation window, an export-only window, or neither, by interval
  overlap across all three nodes.
- Prerequisites: an RDMA `mlx5_0` device, the private libfuse at
  `/data1/efs/deps/libfuse`, and no other cluster on 17631–17633.
- The unit gate needs no cluster: `python3 tests/test_raft_rotate_stall.py`.
