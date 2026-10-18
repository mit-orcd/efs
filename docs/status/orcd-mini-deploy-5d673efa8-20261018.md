# orcd-mini deploy `5d673efa8`: `h->mu` steps 1–3 and restarted-member fsync fix — 2026-10-18

**PASS. Deployed to orcd-mini as `5d673efa8fca` (branch `raft-host-mu`).
Every suite passes, and private `posix_expiry` passes 6/6, where it passed
4/6 before.**
- **Contents:**
  - [`h->mu` plan steps 1–3](raft-host-mu-steps-20261017.md);
  - a fix for creates that failed fsync with EIO for up to 30 s after a
    storage member restarted;
  - clearer `efs-mgmt expiry show` failures.
- **User's tiny-file workload (same recipe, two runs each):**
  - writer 1,470 → 1,617 files/s (+10%);
  - efsd CPU 9.6 → 9.1 cores (−6%);
  - context switches −17%;
  - md5 reader unchanged.

The user approved the deploy for two hours after their go-ahead, which was
01:13Z. The first deploy, `4f184856a696` (steps 1–3 only), went out at
01:21Z; this build followed at 02:33Z after the fixes below.

## Changes since the previous deployment (`e04a80f63cc4`)

Branch `raft-host-mu` = devel `e04a80f63` plus:

| Commit | Change |
| --- | --- |
| `a0eae0f09`, `05c44513f`, `fa26387a0` | `h->mu` plan steps 1–3: opt-in write-path phase histograms, leader-cover routing from the published view, lock-free apply verdicts ([details and private A/B](raft-host-mu-steps-20261017.md)) |
| `df9609d1c`, `382249415`, `cccdec6dc` | A/B scripts; `orcd_mini_workload_profile.sh` takes `MD5_DIR` (`tiny-files1` no longer exists) |
| `e88ddc2ee` | **Fix:** connect cooldowns end once a restarted member answers (client and server) |
| `5d673efa8` | `efs-mgmt expiry show` names the failing step and status |
| `7788bee1f` | Test-only: stub in `test_resolver_deadline.py` (not in the deployed tree) |

### Restarted-member fsync fix (`e88ddc2ee`)

**Found by:** the private `posix_expiry` gate.
- On the first deployed build, both runs failed.
- A six-run comparison put both `e04a80f63cc4` and `4f184856a696` at 4/6,
  with the same two failure modes, so neither came from steps 1–3.
- The more common failure was `expiry_lifecycle.py:209`: fsync EIO on the
  first file created after a member rejoined.

**Cause:** there were two independent connect cooldowns, and each refused a
restarted member until it expired.
- **Client:** efs-fuse marked a node down for 30 s after 4 failed
  checkouts (`EFS_NODE_DOWN_MS`, `src/client/node_cache.c`).
- **Server:** efsd's peer pool marked a peer down for 10 s after 3 failed
  connects (`EFS_PEER_DOWN_MS`, `src/server/peer_pool.c`). That refused
  server-side traffic for the fsync.
- A create whose placement included the member failed fsync with
  `EFS_ERR_NET` (EIO) until the cooldown ran out.

**Fix:**
- **Client:** while a node is in cooldown, a detached background thread
  makes one plain TCP connect per node per second. A success ends the
  cooldown. Callers still get an immediate refusal, so a blackholed peer
  costs no foreground connect timeout; that was the reason for the long
  cooldown.
- **Server:** a successful heartbeat (every 2 s) ends that peer's pool
  cooldown. `server_peer_note_up` never creates a pool slot.

**Reproduction:** `scripts/rejoin_fsync_repro.sh` runs a private 4-node TCP
cluster with direct I/O on node9901. It stops node 2 and restarts it, then
creates, writes and fsyncs a file every 0.2 s for 40 s.

| Outage | `4f184856a696` (deployed before the fix) | With the fix |
| --- | --- | --- |
| 3 s (`DOWN_S=3`) | 132 of 197 creates fail, until 27 s after node 2 listens | 1 fails, 2 ms before node 2 listens |
| 20 s (default) | 49 of 196 fail (~10 s) | 6 of 197 fail (~1 s) |

`e04a80f63cc4` behaved the same as `4f184856a696` with a 20 s outage (49
of 196).

Still open: creates fail while a member is down (0 of 20 succeed in every
arm). Degraded publication and a committed "unavailable" state are not
implemented ([product gaps §1.2](../backlog/product-gaps.md)); this fix
does not change that.

**Tests:**
- `tests/test_node_reprobe.py` (new): the probe runs in the background
  only, is rate-limited, keeps the cooldown on failure and ends it on
  connect.
- `test_conn_checkout_budget.py`: a checkout in cooldown refuses at once
  and only requests a reprobe.
- `test_peer_pool_deadline.py`: the heartbeat ends the cooldown without
  creating slots.

### `expiry_lifecycle.py:66` (fixed after this deploy in `a8bb97750`)

`efs-mgmt expiry show` sometimes exits 1 right after a create: 1/6 runs on
each of the old and new builds. The message covered eight RPCs. It now
names the failing step and status. The failure did not recur in 10
instrumented runs, so its cause is still unknown; there is no retry that
would mask it.

## Build and deploy

- **Source:** clean `git archive` of `5d673efa8` without `results/`, built
  2026-10-18T02:27:06Z with the private libfuse.
  - Build ID `5d673efa8fca`, `v0.3.2-pre-alpha-622-g5d673efa8`, branch
    `raft-host-mu`; `mk.sh` is in each release directory.
- **node9901** `/data1/efs/releases/5d673efa8fca`, efsd/efs-fuse/efs-mgmt.
- **node9900** `/data1/efs/orcd-mini-client/releases/5d673efa8fca`, efs-fuse.
- **Order:**
  1. `fuser -m` empty on both mounts.
  2. node9900 `client.sh stop`.
  3. node9901 `client.sh stop /data1/efs/mnt`. This is a plain stop: run
     before `stop.sh` so `stop.sh`'s force-discard fallback cannot apply.
  4. `EFS_CLUSTER=orcd-mini ./stop.sh`; all three roots logged `stopped
     clean`.
  5. Switch both `src` symlinks.
  6. `EFS_START_TIMEOUT=900 ./start.sh`, preserving storage.
  7. node9900 `client.sh start`.
- **Start timeout:**
  - On the `4f184856a696` deploy, n1's storage inventory took 179.7 s
    (706 GB), just under `start.sh`'s 180 s default, so `start.sh`
    reported "n1 did not start" before mounting. The daemons had started.
    A rerun with `EFS_START_TIMEOUT=900` found them running and mounted the
    client.
  - The default is now too short for these roots.
- **Capacity:** quota 720 GiB; the safe maximum logged at start was
  753.82 GiB. Usage was 657 GiB per node (91.3%).
- **Environment:** RDMA `mlx5_0` on all daemons and both clients,
  `--direct-io`, devops copy matching `nuc-efs` `4de926a`, profiling off.

## Results on `5d673efa8fca`

| Suite | Result |
| --- | --- |
| `make test` on node9901 (full-tree archive of `7788bee1f`, `ASAN_OPTIONS=detect_leaks=0`) | **EXIT=0** |
| Core POSIX, one mount (`smoke.sh`) | **PASS 216/216** |
| Core vs local XFS (`compare.py`, baseline `posix-xfs-20261017-151359.tsv`) | **PASS** |
| `posix_extras` | **0 fail**: 2 PASS, 3 SKIP (mmap ENODEV; two `.efs` freshness checks "not indexed yet"), as before |
| Two-client (`scripts/orcd_mini_posix2.py`) | **PASS 67/67** (443 s) |
| Durability (`smoke-persist.sh`) | **PASS 27/27** |
| Private `gc_reclamation --mode direct --posix`, TCP | **PASS** (71 PASS lines) |
| Same, RDMA | **PASS** (71 PASS lines) |
| Private `posix_expiry`, two runs | **PASS, PASS** (134 PASS lines each) |
| Private `posix_expiry` with the fix, six runs | **6/6 PASS**; `e04a80f63cc4` and `4f184856a696` were 4/6 each |

The `4f184856a696` deploy passed the same suites before the fix: smoke
216/216, XFS comparison, extras 0 fail, two-client 67/67, persist 27/27
and GC over TCP and RDMA. It failed both `posix_expiry` runs.

## Tiny-file workload

**Recipe:** `scripts/orcd_mini_workload_profile.sh LABEL 180` from the
Mac, with `MD5_DIR=tiny-files6`. It runs on node9900:
- `tiny_files.py` with 16 workers into a new directory;
- `tiny_files_md5.py` with 16 workers over `tiny-files6`;
- `ecrawl --no-write`.

After a 60 s warm-up it records 10 s of efsd context switches, 15 s of efsd
CPU and a 200-file probe on both mounts, then stops at about 105 s.

**Conditions:** no other users on the mounts (`fuser -m` empty). The
baseline ran on `e04a80f63cc4` before the deploy, the comparison on
`5d673efa8fca` after it.

| Metric | `e04a80f63cc4` (2 runs) | `5d673efa8fca` (2 runs) |
| --- | --- | --- |
| Writer files/s | 1,423 / 1,517 (mean 1,470) | 1,703 / 1,531 (mean 1,617, +10%) |
| md5 reader hashes/s | 1,311 / 1,365 | 1,294 / 1,347 (unchanged) |
| efsd CPU, three daemons | 973% / 954% | 905% / 911% (−6%) |
| efsd CPU per completed write+hash | 3.43 ms | 3.09 ms (−10%) |
| efsd context switches, 10 s | 404,231 / 387,528 | 325,723 / 331,560 (−17%) |
| Mutex waits, share of switches | 8.9% / 8.2% | 7.2% / 7.9% |
| node9901 probe create p50 / p99 (ms) | 3.31 / 53.5; 2.92 / 54.7 | 2.39 / 29.3; 2.55 / 15.0 |
| node9900 probe create p50 / p99 (ms) | 3.47 / 52.2; 3.17 / 57.2 | 3.41 / 70.3; 3.38 / 22.7 |
| Probe stat p50 (ms), node9901 / node9900 | 1.29 / 1.62; 0.84 / 1.98 | 0.94 / 1.89; 1.01 / 1.89 |

The md5 reader's 21–27 "errors" in every run are `crawl interrupted`
entries written when the script stops it. Two runs per arm cannot separate
a 10% writer gain from noise; the CPU and context-switch reductions are
consistent across both runs. Removal of the four writer directories
(`tiny-files-prof-*`, about 640,000 files) started at the end, from
node9900 in the background; its completion marker is
`/data1/efs/orcd-mini-client/prof/mu-cleanup.done`.

## Raw output (outside Git)

- node9901 `/data1/efs/prof/mu-live/` (sha256 prefixes):
  - `ab-results.txt` `7a2ed74bf7e0a9b9`, the private A/B of the steps page;
  - `gcr-tcp.txt` `1050d6302f4243be`;
  - `gcr-rdma.txt` `1fe1d085e118f85f`;
  - `expiry1.txt` `08bb3ea97a62b078`;
  - `expiry2.txt` `e8306cfcbc1c6c34`;
  - `ab-summary` `da07aa197ebaa008`, the old/new expiry comparison;
  - `fix-summary` `6b9cb4be407e8e8a`, the 6/6 run;
  - `make-test-7788bee1f.log` `b439dd9e79c4c7d9`;
  - `make-test-d377478c3.log`.
- node9901 `/data1/efs/logs/posix-*-20261017-2*.tsv` and `persist-*` from
  `smoke.sh`, `posix_extras` and `smoke-persist.sh`.
- node9900 `/data1/efs/orcd-mini-client/prof/wl-{pre-e04a,pre-e04a-2,post-5d67,post-5d67-2}/`
  and node9901 `/data1/efs/prof/wl-*/`: the workload runs.
- Two-client TSVs on the driver Mac: `posix2-4f184856a.tsv`,
  `posix2-5d673efa8.tsv`.
- The private A/B, gate and reproduction clusters under node9901 `/tmp`
  were removed. No other clusters run on node9900 or node9901.

## Remaining

- `expiry_lifecycle.py:66` intermittent `expiry show` failure: fixed on
  devel in `a8bb97750`, not deployed. `expiry_lookup` did not follow the
  namespace leader's NOT_PRIMARY hint, so it failed whenever the seed node
  followed the root's group.
  - Confirmed on a private 4-node cluster: through the two group-0
    followers, the old binary fails with `step: lookup, status -15` and
    the new one passes.
  - `posix_expiry` 4/4 PASS with it.
  - Deploying it touches only `efs-mgmt`.
- Creates fail while a member is down: degraded publication is not
  implemented (product gaps §1.2, W71/W74).
- `start.sh`'s default 180 s start timeout is shorter than these roots'
  storage inventory.
- `h->mu` plan steps 5 and 6: NOTRUN (see the plan page).
