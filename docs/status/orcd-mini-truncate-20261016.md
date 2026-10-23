# orcd-mini truncate: apply wedge and lane-fence cap — 2026-10-16

**PASS (scoped, functional).** Two truncate defects found on orcd-mini through
`dd if=/dev/null of=<efs file>` (open with O_TRUNC). Both are fixed and
deployed on orcd-mini.

1. **One-replica apply wedge (`502277d19`).** n3 retried group 2 index
   4063608, a LANE_FENCE, forever: `apply lane-fence rc=-5` every 50 ms from
   14:32:14Z, about 25,900 times. n3 was the group 0 leader, so truncate
   returned EIO and mkdir/rm returned EBUSY on both mounts. No
   crash/qualification gate is claimed.
2. **Truncate of large files (`4d6530dd8`).** A LANE_FENCE could delete at
   most 32 chunks per lane, so truncating a freshly written file larger than
   about 1 GiB returned EIO (`apply lane-fence rc=-2`). Same-group TRUNCATE
   refusals were answered OK and left the old size.

Linked item: user-reported bug (dd truncate EIO on orcd-mini); follows the
orcd-mini deployment of `fa29f9277` (cluster repo `ORCD.md`).

## Cause and change

**Wedge.** The KV apply scope (`c0dd0fa97`) validated `vlen` on every staged
item. DEL items built in scan callbacks (`trunc_del_cb`, the sweeps) set only
op/key/klen over uninitialised stack arrays. On n3 the stack bytes made
`vlen > 16 MiB`, so `lsm_batch` returned INVAL and poisoned the scope. The
failed `apply_end` made Raft retry the index. Replaying the same entry offline
against a copy of n3's KV (blooms built, cache warmed) applied cleanly; the
stack contents differ. The WAL encoder already ignored DEL val/vlen after an
earlier `apply lane-sweep rc=-5` incident. Fix: the staged check validates
val/vlen only for PUT. `test_kv_lsm` stages a garbage-vlen DEL in a scope:
3 failures on the old code, PASS on the fix.

**Cap.** Chunks are 128 KiB over 64 lanes. A 1 GiB file has 128 rows per lane
and a 100 GiB file about 12,800, which exceeds even the apply scope's
32,768-key bound. The change:

- `efs_meta_apply_lane_fence` deletes one heap page (4,096 items, with
  reserve for the stamp and an encoded plan). After committing it, it answers
  `EFS_META_FENCE_MORE` (a ring verdict, never a Raft retry).
- An equal fenced epoch resumes the delete; an older fence stays a no-op.
- The scan seeks past the new size and skips rows whose `content_epoch` is
  not below the fence epoch, so a late resume never deletes post-truncate
  data.
- `host_truncate` re-proposes each cross-group fence while MORE.
- It now reads the TRUNCATE verdict (`host_wait_verdict` instead of
  `host_wait_settled`).
- On NOMEM it fences and drains the same-group lanes at the same epoch, then
  retries TRUNCATE once.

## Environment

- orcd-mini: three efsd on node9901 (RHEL 9.7, AMD EPYC 9474F, 376 GB),
  ports 17432–17434, storage `/data1/efs/n{1,2,3}` on one xiRAID XFS
  (`allocsize=131072k`), 800 GiB quota each.
- Files storage, `--direct-io`, native IB RDMA (`EFS_TRANSPORT=rdma`,
  `EFS_RDMA_DEV=mlx5_0`), `.efs` commands on (`EFS_COMMANDS=1`,
  `EFS_COMMANDS_SERVER=1`), `EFS_EXPIRY=0`.
- Clients: node9901 `/data1/efs/mnt` and node9900 `/data1/efs/orcd-mini-mnt`.
  libfuse 3.16.2 private build with rpath.
- Builds:
  - `502277d1910b`: efsd `027b8fcd…3033650`.
  - `4d6530dd83b0`: efsd
    `0a7c30281826afcafa53076cc8c860edb16bdf17f3c99069fe0846ab5b865440`;
    efs-fuse node9901 `a69b5446…b5c5088`, node9900
    `67e4cb1be8a76339a8f1b2c898871cc8629061294bf435f17482596b7246b676`.
- Deployed with `EFS_CLUSTER=orcd-mini ./stop.sh`/`start.sh` (devops) and
  `client.sh stop|start` on node9900, with both mounts idle.

## Results (node9900 mount, `4d6530dd8`)

| Check | Result |
| --- | --- |
| 1 GiB `dd if=/dev/zero` then `dd if=/dev/null` onto it, ×3 | PASS, size 0, 53–88 ms (was EIO 3/3) |
| 1 GiB random → `truncate -s 300MiB`, prefix MD5 vs source | PASS (`a8483092…`) |
| Regrow that file to 1 GiB: non-zero bytes past the old EOF | 0 |
| 64 MiB write, truncate to 0, rewrite new data, MD5 | PASS |
| 100 GiB `dd if=/dev/zero bs=4M` (106.7 s, 1.0 GB/s) then `dd if=/dev/null` | PASS in 4.34 s; 64 lanes × 7 pages = 448 fence entries |
| Group 2 apply after the wedge fix | caught up (4063716/4063716 on all nodes) |
| mkdir/rmdir/rm, leftover `trunc-repro-*` removal | PASS (were EBUSY) |

Expected log lines: `apply truncate rc=-2` is the first TRUNCATE attempt
refusing before the paged fallback. `apply lane-sweep rc=-13` (BUSY) from
unlinks predates this change.

Full `make test` on node9901 from the working tree of `4d6530dd8` (1,346
commands, 449 s, `ASAN_OPTIONS=detect_leaks=0` because `ptrace_scope=2`):
1 failure, `test_storage_principles` (6 punch cases). That failure
is node9901's `allocsize=128M`; the test passes 13/13 on node9900 with the
same tree, as with `fa29f9277`.

## Reproduce

On a client mount `M` of a cluster built from `4d6530dd8`:

```sh
dd if=/dev/zero of=$M/z.dat bs=1M count=1024 && dd if=/dev/null of=$M/z.dat && stat -c %s $M/z.dat   # 0
dd if=/dev/zero of=$M/big.dat bs=4M count=25600 && time dd if=/dev/null of=$M/big.dat
grep -h 'truncate ino=.*pages=' /data1/efs/logs/efsd-n*.log | tail   # pages per lane
rm -f $M/z.dat $M/big.dat
```

Unit coverage: `tests/test_kv_lsm` (garbage-vlen DEL in a scope),
`tests/test_meta_apply` (`test_lane_fence_paged`: 6,144 rows in 3 pages,
partial size, new-epoch row kept across a late resume).

The wedge was diagnosed by replaying the stuck entry against a copy of n3's
`mdraft/kv` and `mdraft/log` with a throwaway harness, and by `perf record -g`
of the live pump. Raw copies are on node9901 under `/data1/efs/evidence/debug-lf` (not
in Git, deletable).

## Limitations and remaining gates

- Between the fence pages and the TRUNCATE commit, a concurrent reader can see
  ranges past the new size already deleted while the row still has the old
  size. Cross-group lanes had this window before.
- Large truncates cost one Raft entry per page (100 GiB: 448 entries, 4.3 s).
  This is not tuned.
- Not run: crash/power loss during paging, multi-client concurrent truncate,
  the Intel cluster.

**Raw evidence note (2026-10-23):** the bulk data in `debug-lf` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
