# orcd-test: fresh deploy of devel `394f2b791`, all POSIX suites over RDMA — October 27

**Purpose:** user request (Oct 26): deploy current devel to orcd-test with
RDMA, wipe if needed, and run all POSIX suites. The stores predated K33
(membership store v2 refuses them), so the user's standing OK covered a wipe.
**Verdict: PASS** for the cluster's POSIX, durability, two-client and live
`.efs` suites on all seven clients. **Two findings:**
- **W167:** `raft-mkfs` sent to the seed answers BUSY forever after an
  online node addition. Setup worked around it.
- **W159 again:** the expiry fixture's closing core POSIX run failed one case
  with EBUSY. The expiry checks themselves PASS.

Not a release gate.

| | |
|---|---|
| Source | devel `394f2b791e3e` (`v0.3.2-pre-alpha-1615-g394f2b791`), built on every host by `~/git/cluster/orcd-test.py deploy --ref devel` (efsd built 2026-10-27T01:08:10Z); SHA-256 prefixes `efsd` bbc05ed8d6d11a5e, `efs-mgmt` 764ecae51bd7eece (fcstor003), `efs-fuse` 35e0f609c828eff4 (fcstor007) |
| Cluster | fcstor003–006 storage (6 NVMe stores each), fcstor007–013 clients; RHEL 9.8, kernel 5.14.0-687.39.1.el9_8, Python 3.9.25, libfuse 3.16.2 |
| Transport | RDMA on `mlx5_2` for daemons and every mount (`start/mount --transport rdma`) |
| Features | fragment I/O always `O_DIRECT` (D42); `.efs` on by commands-policy (mask 3, the default after formation) |
| Data | all 24 stores wiped (old Oct 22 data, ~36 GiB); fresh formation |

## Deploy and formation

1. All 11 hosts were idle (no efsd or efs-fuse, no mounts) on an Oct 22 build.
   `deploy` built and installed `394f2b791` everywhere. The previous trees are
   kept at `/tmp/efs-before-orcd-test-394f2b791e3e`.
2. Each storage host's six store directories were removed with `edelete` and
   recreated empty.
3. `start --transport rdma`. K33 formed the cluster automatically with
   three members (fcstor003–005). fcstor006 joined online as member 4
   (revision 2). Group 2 moved its voters from 0x7 to 0xe through joint
   consensus. That retired node 1's group-2 replica, as designed.
4. `init-export` failed silently (W167 below). `raft-mkfs` succeeded against
   node 3, and so did `raft-setattr` (root owned by the test user, mode 0755).
   The init marker was then written by hand. All seven clients mounted over
   RDMA; status OK, 4 up, 75.69 TiB usable.

## Results

| Suite | Result |
|---|---|
| POSIX `posix_suite.py --suite all --jobs 16`, 7 clients at once | 220/223 PASS, 0 FAIL, 3 SKIP on every client |
| SKIPs | `mmap_write_read` (W104, by design); `virt_find_query`, `virt_stats_readable` (not indexed within the 10 s case budget; covered with 150 s by `virtual_interfaces`) |
| `posix_persist` prepare | 27/27 on all 7 clients |
| Full `orcd-test.py stop`, `start --transport rdma`, remount | clean; 4/4 identical versions |
| `posix_persist` verify after the restart | 27/27 on all 7 clients |
| Live suites, fcstor007, two RDMA mounts (`scripts/orcd_test_live_suites.sh`) | PASS all harnesses |
| ↳ `posix_2client --local` | 68/68 PASS |
| ↳ `efs_commands`, `efs_command_options`, `efs_command_pages` | PASS |
| ↳ `virtual_interfaces --freshness-s 150` | PASS |
| ↳ `query_page_close`, `query_large_delta`, `efs_commands_load` | PASS |
| ↳ `chmod_truncate`, `concurrent_extend_eof`, `exclusive_create_visibility --spawn`, `fsync_admission`, `mtime_barrier` | PASS |
| ↳ `w121_exclusive_fork` (stock fork writers, informational) | 153/160 (W121, kernel; as on Oct 22) |
| Expiry (`posix_expiry.py --suite expiry`, private four-node TCP fixture under /tmp on fcstor007, port 24410) | expiry checks PASS (79 PASS lines: bulk reclamation, 144-file page backlog, worker status, owned tree cleanup and the rest); closing core POSIX run 217/218, `names_crazy_roundtrip` failed with EBUSY (W159) |

## W167: `raft-mkfs` at the seed answers BUSY after an online node addition

After fcstor006 joined as member 4, `efs-mgmt raft-mkfs 172.16.223.57:17432`
(node 1, the seed) answered `rc=-13 (resource busy) leader_hint=2` on every
attempt (12 tries over ~2 minutes; the index advanced each time). The same
command to node 3 or node 2 answered `rc=0 (success)` at once.
`raft-setattr` through node 1 worked.

Node 1's `raft-status` still showed its retired group-2 replica at
`voters=0x7 joint=1 retired=1 commit=4 last=5 unapplied=1`. The group-2
leader (node 3) and the other members were at `voters=0xe joint=0
commit=81`. The likely cause, not proven: node 1 routes `raft-mkfs` to its
retired, frozen group-2 replica instead of forwarding to the leader or
answering NOT_PRIMARY. Effect: `orcd-test.py init-export` (and anything that
formats through the seed) fails after an automatic formation plus an online
addition, while mounts and metadata are unaffected. A retired replica should
not answer BUSY forever; it should redirect.

## W159 recurrence

The expiry fixture's closing core POSIX run failed `names_crazy_roundtrip`:
`OSError: [Errno 16] Device or resource busy` on a directory removal. On
orcd-mini (Oct 24) the same fixture failed `dir_times_mkdir_rmdir` with rmdir
EBUSY. The real cluster's own core runs passed 220/223 on all seven clients.
This adds a third host family to W159.

## Reproduce

```sh
cd ~/git/cluster
./orcd-test.py deploy --ref devel
# wipe: on each storage host, edelete --force --delete every
# /data1/0N/efs-orcd-test-erbmi1 (N=1..6), then mkdir them again
./orcd-test.py start --transport rdma
# init-export fails at the seed (W167); instead, on fcstor003 in /tmp/efs:
#   ./efs-mgmt raft-mkfs 172.16.223.59:17432
#   ./efs-mgmt raft-setattr 172.16.223.59:17432 1 6 0 "$(id -u)" "$(id -g)"
./orcd-test.py mount --transport rdma
```

Per client (detached, `cd /tmp/efs`):
`posix_suite.py /tmp/efs-mount/posix-rdma-$(hostname -s) --suite all --jobs 16`,
then `posix_persist.py /tmp/efs-mount/persist-$(hostname -s) --phase prepare`.
Then `./orcd-test.py stop`, `start --transport rdma`, `mount --transport rdma`
and `--phase verify` on every client. On fcstor007: a second RDMA mount at
`/tmp/efs-mount2` with `scripts/client.sh`, then
`scripts/orcd_test_live_suites.sh OUT /tmp/efs-mount /tmp/efs-mount2`, then
`posix_expiry.py ROOT/m1 --suite expiry --root ROOT/a --root ROOT/b
--peer-mount ROOT/m2 --jobs 4` under /tmp.

**Raw output:** `~/orcd/scratch/efs/orcd-test/<client>/suites-rdma-394f2b791/`
(POSIX TSVs, persist, `live/`, expiry, driver scripts); daemon logs in
`~/orcd/scratch/efs/orcd-test/<storage host>/efsd.log`. Hash list:
`fcstor007/suites-rdma-394f2b791/SHA256SUMS.all` (sha256 prefix
`e3f9b25e6a05e2e1`).

## Remaining

- W167: fix `raft-mkfs` at a node whose replica of the group was retired;
  `orcd-test.py init-export` could also target the group leader.
- W159: EBUSY in the expiry fixture's closing core run.
- Not run: IOR, `make test-all`, TCP.
