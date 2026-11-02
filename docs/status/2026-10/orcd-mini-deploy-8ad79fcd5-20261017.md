# orcd-mini deploy 8ad79fcd5 and full POSIX qualification — 2026-10-17

**Deployed. Core POSIX PASS on every suite that tests the filesystem; two
suites have known, non-filesystem failures.** The user asked for every POSIX
suite after deploying all of devel, to find out whether the code base works.

- Core suite, two-client, durability, XFS comparison and the private GC +
  POSIX gates (TCP and RDMA) all PASS.
- `posix_extras` FAILs 2 of 5. Both are `.efs` query freshness checks that
  break under the new 60 s catalog refresh default.
- `posix_expiry` FAILs in its own fixture, identically on the previous
  release.

## Build and deploy

- **Source:** devel `8ad79fcd5` ("fix(raft): rotate the log off the metadata
  path"), on top of `859b2b9eb`.
  - Nothing else on devel was undeployed; `34cee9229` is docs and scripts
    only.
- **Build:** a clean `git archive` without `results/`, built on each node
  with the private libfuse.
  - Build ID `8ad79fcd5857`, `v0.3.2-pre-alpha-604-g8ad79fcd5`, built
    2026-10-17T19:08:50Z.
- **node9901** `/data1/efs/releases/8ad79fcd5857`:
  - efsd `078f3873bcb98dc04c6d7ba89c72083e232166a236ef8cd67f69479ca3fcd6f3`;
  - efs-fuse `5e0d07439ced8544f857cba919728bb66bba94d582bc977b626f149a7bd8a735`.
- **node9900** `/data1/efs/orcd-mini-client/releases/8ad79fcd5857`:
  - efs-fuse `f4e858b348405443cb7217ac8cf9e344adc087d1748b6c6963bac61e742ce0f9`.
- **Previous:** `859b2b9eb320` on daemons and clients (see
  [GC/query catalog](orcd-mini-gc-query-catalog-20261017.md)).
- **Order:**
  1. `fuser -m` on both mounts: no users.
  2. Devops copy already in sync.
  3. Stopped the node9900 client, then `EFS_CLUSTER=orcd-mini ./stop.sh`.
  4. Switched both `src` symlinks, then `start.sh` and `client.sh start`.
- **After start:** all three daemons report `8ad79fcd5857`, and the query
  catalog was restored from disk (`query-restore rc=0`).
- **Environment:** node9901 runs 3 efsd (10.1.99.1:17432–17434, RDMA
  `mlx5_0`, `--direct-io`, 800 GiB quota each, about 75% used). Clients are on
  node9901 (`/data1/efs/mnt`) and node9900 (`/data1/efs/orcd-mini-mnt`);
  `EFS_EXPIRY=0` on the clients.

## Results

| Suite | Command | Result |
| --- | --- | --- |
| Core POSIX, one mount | node9901 `EFS_CLUSTER=orcd-mini ./smoke.sh` (`posix_suite.py`, jobs 4, 30 s case timeout) | **PASS 216/216** (13.8 s) |
| Core vs local XFS | `posix_suite.py` on an XFS `/tmp` directory, then `compare.py XFS orcd-mini` | **PASS** (every test passes on both sides) |
| POSIX extras | `posix_extras.py /data1/efs/mnt --jobs 4 --timeout-s 30` | **FAIL 2/5**, 1 skip (see below) |
| Two-client, node9901 + node9900 | `scripts/orcd_mini_posix2.py RESULTS.tsv` from the Mac | **PASS 67/67** (446 s) |
| Durability prepare/remount/verify | node9901 `EFS_CLUSTER=orcd-mini ./smoke-persist.sh` | **PASS 27/27** |
| Private GC + POSIX, direct, TCP | `tests/live/gc_reclamation.py --mode direct --posix --root /tmp/A --root /tmp/B` | **PASS** (71 PASS lines) |
| Private GC + POSIX, direct, RDMA | same with `--transport rdma`, `EFS_RDMA_DEV=mlx5_0` | **PASS** (71 PASS lines) |
| Private expiry lifecycle | `tests/posix/posix_expiry.py M1 --suite expiry --root /tmp/A --root /tmp/B --peer-mount M2` | **FAIL** after 13 PASS lines, in the fixture (see below) |

### POSIX extras failures

- **`virt_stats_readable` and `virt_find_query` FAIL.**
  - On a directory created moments earlier, `.efs/stats` and `.efs/find`
    exit 1 with `[Errno 16] Device or resource busy`. The directory is not
    yet in the published catalog.
  - Retried 65 s later, the same commands succeed ("Results as of 37 seconds
    ago; validated against this snapshot").
  - The tests expect catalog freshness within the case budget (30 s). The
    default refresh interval is now 60 s (`859b2b9eb`, the user's choice).
  - Not a filesystem failure. Choices for the user: run these tests with
    `EFS_QUERY_REFRESH_MS=2000` on the daemons, let them wait for one refresh
    interval, or have `.efs` commands report "not yet indexed" rather than
    EBUSY.
  - There is no earlier orcd-mini extras run to compare against.
- **`mmap_write_read` SKIP:** mmap returns ENODEV on this direct-I/O mount.
- `mmap_private_not_visible` and `virt_find_not_a_real_dir` PASS.

### Expiry fixture failure (pre-existing)

- The private four-node expiry suite passed its first 13 checks: listening,
  default-off, authorization, path and FileID inspection, fixed birth, and
  removal with physical reclamation.
- It then stops at `tests/live/expiry_lifecycle.py:90`, "fixture failed to
  choose a different inode group". None of 32 new directories got an inode
  of the other metadata group's parity.
- **The previous release `4aff22c62f9f` fails at the same line** with the
  same fixture, so this predates both deploys.
- The later expiry checks (cross-group inheritance, restart, offline member,
  forwarding) did not run.

## Raw output (outside Git)

On node9901 in `/data1/efs/logs/`:
- `posix-20261017-151336.tsv`;
- `posix-extras-20261017-151359.tsv`;
- `posix-xfs-20261017-151359.tsv`;
- `persist-{prepare,verify}-20261017-152350.tsv`.

On node9901 in `/data1/efs/prof/qcat-live/suites/` (sha256 prefixes):
- `gcr-tcp.txt` `4a052f247b61d05f`;
- `gcr-rdma.txt` `2d306b1022c5788e`;
- `expiry.txt` `e90f1fa41cb6f876`;
- `expiry-4aff.txt` `3b9898cce91d3f13`.

The two-client TSV stayed on the driver Mac (sha256 prefix
`5172e9f0daa87d4c`). All test directories on the mounts and all private
roots were removed.

## Remaining

- Decide how `.efs` freshness tests should treat the 60 s default (above).
- Fix the expiry fixture's group selection, so that the cross-group
  inheritance, restart and failover checks run again.
- The cleanup-deferral defect from `a2d869fde` is still open (see the
  [GC/query catalog page](orcd-mini-gc-query-catalog-20261017.md)).
- Not run: the fault suite (`tests/faults/run_faults.sh`). It is not a POSIX
  suite.
