# Client staging evictor: O(1) LRU — 2026-10-17

**Verdict: PASS.** Private RDMA A/B, then deployed to both orcd-mini clients
(`155695ccd`; POSIX smoke 216/216). Under the user's workload shape, the
node9900 writer held about 1,000 and then about 1,900 files/s, where the
old client had decayed to about 50. A 14 s Raft log rotation on all three
daemons (pre-existing; see below) failed one close with EIO. With the staging table over its cap,
the old evictor's cost grew with every inode the client had seen. The new
one holds create throughput flat after warm-up. A separate first-minutes
drop remains with eviction disabled too, so it is not this bug.

## Problem

Under the user's tiny-file workload on orcd-mini (`4aff22c62`), the node9900
writer fell to about 50 files/s after an hour. A serial create on that
mount took 207 ms p50, while the same create on the node9901 mount took
24 ms. On node9900, efs-fuse spent 96% of its samples in one thread,
`stage_evict_main`, and 46% of its context switches were mutex waits. They
were spread over lookup, getattr, release and flush.

Cause, in `src/client/stage_evict.c`:
- Once the staging table is over `EFS_CLIENT_META_MB` (256 MB), every
  staging op kicks the evictor.
- Each pass scanned the whole LRU hash table under `g_lru_mu` (at least
  4 slots per staged ino) to find 256 candidates, and evicted at most 64.
- Every touch on the create, stat and read paths needs `g_lru_mu`.
- Every productive wake also compacted all ~4,096 shard tabs under the
  table lock and `idx_mu`.

With a crawler staging millions of inodes, passes ran back to back, and
their cost grew as the client saw more inodes.

## Change

- **Ordered LRU.** A node per staged ino sits on a doubly linked list in
  touch order, with an open-addressing index from ino to node. Touch,
  remove and lookup are O(1), and a pass reads its candidates from the list
  head.
- **Pinned candidates go to the newest end** (second chance). This replaces
  the tick cursor, so a pinned prefix is not re-read every pass. Pins are
  still re-checked under the table locks before anything is dropped.
- **Watermark drain.** Over the cap, a wake runs passes down to 7/8 of the
  cap within a 20 ms budget. It compacts once per 4,096 drops and once at
  the end, not after every 64.
- **Pacing.** If the drain made progress but the table is still over the
  cap, the evictor sleeps for as long as it worked (20 ms), then runs
  again. If nothing was evictable, the existing 1 s backoff applies, and the
  "nothing evictable" warning can fire again once the table has been back
  under the cap.
- **Unchanged:** cold-tab eviction (D18), which now targets the same low
  watermark.

`tests/test_stage_evict_lru.py` compiles the real LRU and pass code from
the source and checks it against a reference model:
- list order and the index after 1.2M random touches and removes;
- oldest-first reads;
- requeue only for an untouched entry;
- a pass evicts unpinned entries oldest first and requeues pinned ones;
- 2,000 passes over 2M entries take about 0.03 s.

It runs plain and under ASan/UBSan, and is part of `make test`.

## Builds

- **Daemons in every run:** orcd-mini release `4aff22c62f9f`
  (efsd `7bb704f5…4b0e`).
- **Old client:** the same release's efs-fuse,
  `2311201f22d815d6682f70b91743a413830e7ef4116567d88659ba0b0faf8948`.
- **New client:** this commit's source on `619f5297e`, built on node9901
  `/data1/efs/work/evict` (`EFS_GIT_ID=619f5297evct`, 2026-10-17T00:57:45Z,
  same flags). efs-fuse
  `6a46ff34cde45d50196efbf3e3869443adba3d858287fdf1bd081120d058483a`.

## Environment and method

- **Cluster:** node9901, three efsd on 10.1.99.1:17631–17633, RDMA mlx5_0,
  `--direct-io`. Stores are under `/tmp/efs-ab`, because `/data1` is below
  efsd's 20% free-space startup reserve. One local efs-fuse mount runs with
  `EFS_CLIENT_META_MB=16`, so the table stays over its cap the whole run,
  as on orcd-mini.
- **Script:** `scripts/orcd_mini_stage_evict_ab.sh DAEMON_REL CLIENT_REL
  LABEL SECONDS`:
  1. 32 threads populate 200,000 files of 256 B in 400 directories.
  2. 16 writers create 256 B files, each in its own directory, while one
     crawler loops over the tree and the writers' directories with lstat.
  3. After 15 s, the 200-file probe runs in the foreground, alongside
     per-thread CPU and a 10 s `perf -e cs -g` of efs-fuse.
  4. With `INTERVAL=60`, rates are printed every minute.
- **Runs:** six 60 s runs, alternating old and new. Run new2 was discarded:
  another session's client wrote to that cluster at 01:25–01:28Z, and it was
  rerun as new2b. Then three 600 s runs with `QUOTA=300G`: old, new, and new
  with `EFS_CLIENT_META_MB=65536`, where eviction never runs.

## Results

60 s runs:

| Run | Creates/s | Crawler lstats/s | efs-fuse mutex waits | Hottest thread |
| --- | --- | --- | --- | --- |
| old1 / old2 / old3 | 2,292 / 2,125 / 2,098 | 632 / 514 / 555 | 27.0 / 23.9 / 24.8% | evictor 47–87% CPU, 36–63 s total |
| new1 / new2b / new3 | 2,415 / 2,337 / 2,361 | 782 / 790 / 802 | 11.4 / 8.4 / 9.5% | 20–27% CPU (a FUSE worker); evictor 3–7 s total |

The loaded probe was much the same in both arms: create p50 2.7–3.2 ms,
stat p50 0.6–1.3 ms.

600 s runs, creates/s per minute:

| t | old | new | new, no eviction |
| --- | --- | --- | --- |
| 60 s | 2,263 | 2,327 | 2,301 |
| 120 s | 2,047 | 1,946 | 1,991 |
| 180 s | 1,714 | 1,611 | 1,570 |
| 240 s | 1,428 | 1,438 | 1,450 |
| 300 s | 1,335 | 1,346 | 1,488 |
| 420 s | 1,302 | 1,380 | 1,445 |
| 540 s | 1,272 | 1,341 | 1,380 |
| 600 s | 1,127 | 1,319 | 1,378 |
| total | 904,047 | 927,454 | 949,526 |
| crawler lstats/s | 770 | 857 | 861 |
| efs-fuse mutex waits | 25.5% | 9.6% | 2.8% |

- **Old client:** keeps declining (1,428 → 1,127 over the last 6 minutes).
- **New client:** flat after 4 minutes, within about 4% of the
  no-eviction arm.
- **All three arms:** fall from about 2,300 to about 1,450 in the first
  4 minutes, including the one where eviction never runs. That first drop
  is outside the client evictor; each writer's single directory grows to
  about 57,000 files in this test. It is still open (see below).

Its remaining lock waits are the evictor's own (`stage_evict_main` and
`evict_cold_tabs`, about 64%) and `meta_flush_main` (24%). At a 16 MB cap,
the D18 tab floor is far above the cap, so whole tabs keep being dropped
and reloaded; at the default 256 MB this happens much less.

## Tests

`make test` on node9901, candidate tree, run line by line with
`/data1/efs/prof/evict-test.sh`: **264/265 PASS**.
- **Without leak checking:** four lines ran with `ASAN_OPTIONS=detect_leaks=0`
  after the node's LeakSanitizer fatal error. Three are the known ones
  (`test_readindex_pair`, `test_raft_ack_window`, `test_host_outbox`); the
  fourth is the new `test_stage_evict_lru.py`.
- **The one failure:** `docs/check-architecture.py`. The generated
  architecture files were stale after the earlier status edits; `ffb1dee9b`
  regenerated them, and the gate passes 5/5 on this tree.

## Reproduce

```sh
# node9901, with a daemon release and a client build of each tree
for r in 1 2 3; do
  scripts/orcd_mini_stage_evict_ab.sh <daemon-rel> <old-client> old$r 60
  scripts/orcd_mini_stage_evict_ab.sh <daemon-rel> <new-client> new$r 60
done
export QUOTA=300G INTERVAL=60
scripts/orcd_mini_stage_evict_ab.sh <daemon-rel> <old-client> long-old 600
scripts/orcd_mini_stage_evict_ab.sh <daemon-rel> <new-client> long-new 600
META_MB=65536 scripts/orcd_mini_stage_evict_ab.sh <daemon-rel> <new-client> long-nocap 600
```

- **Prerequisites:** free ports 17631–17633, `perf_event_paranoid <= 1`,
  `mlx5_0`, the private libfuse, and room under `/tmp` (`AB_ROOT`).
- **Cleanup:** each run formats, measures and removes its own cluster.
- **Raw output (outside Git), node9901 `/data1/efs/prof/`:**
  - `evict-ab.log` `00ab43bf…`
  - `evict-ab2.log` `1b98f157…`
  - `evict-ab3.log` `e1be95df…`
  - `evict-iso.log` `82a197be…`
  - `evict-test.log` `3979dd4a…`

## orcd-mini deployment of `155695ccd` (clients only) — 2026-10-17 06:30Z

The fix is client-only, so only the two efs-fuse mounts were restarted. The
user's workloads had stopped, and `fuser -m` was empty on both mounts.

- **Builds:** clean `git archive`, build ID `155695ccdd27`, version
  `v0.3.2-pre-alpha-600-g155695ccd`, built 2026-10-17T06:29:09Z.
  - node9901 `/data1/efs/releases/155695ccdd27`: efs-fuse
    `1e53c8982ad5786f5880653737edd7c58e3c21cbc3e30178df08c2396c042d3d`.
  - node9900 `/data1/efs/orcd-mini-client/releases/155695ccdd27`: efs-fuse
    `6edfdc2e149bcc8ddf51b8e164a3cf6ef0981c13a4a836d0c2c8a5447ae54299`.
- **node9900:** `client.sh stop`, repoint `src`, `client.sh start`.
- **node9901:** `client.sh stop /data1/efs/mnt`, then the new release's
  `client.sh`, with the running client's environment (`EFS_TRANSPORT=rdma
  EFS_RDMA_DEV=mlx5_0 EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 EFS_EXPIRY=0
  EFS_CLUSTER=orcd-mini`).
- **Left on purpose:**
  - **`/data1/efs/src` still points to `4aff22c62f9f`, which the three
    daemons run.** If one daemon restarted alone from a new `src`, its build
    ID would not match its peers and its HELLO would be refused.
  - **A full daemon restart is blocked anyway:** `/data1` is below efsd's
    startup check of 20% free space.
  - **Effect on devops:** until the next full deploy, `start.sh` would
    object that the node9901 mount is served by a different binary than
    `src`.
- **Checks:** RDMA up on both clients; `smoke.sh` POSIX **216/216 PASS**.

### Under the user's workload shape

The workload was the user's commands, and its files are kept, as the user
asked:
- **node9900:** `tiny_files.py --max-depth 8 --max-files-per-folder 10000
  --max-files 1000000 --workers 16`, `tiny_files_md5.py` over `tiny-files4`,
  and `ecrawl --no-write`.
- **node9901:** `tiny_files_md5.py` over `tiny-files2` and `ecrawl`.
- **Probe:** a 50-file serial create every 5 minutes into
  `tiny-files-evict-probe/`.

The writers wrote into `tiny-files-evict-20261017-0635` and, chained after
the first one ended, `…-0635-b`.

| | Old client, 2026-10-16 | New, writer 1 | New, writer 2 |
| --- | --- | --- | --- |
| Rate over time | 271 files/s average at 35 min; about 50/s at the end | 1,001 / 993 / 1,003 files/s (6 / 11 / 14 min) | 1,825 / 1,904 / 1,907 files/s (3 / 5 / 9 min) |
| Files | 585k in 37 min | 848,962 in 14 min, ended by the EIO below | 1,000,000 in 8.7 min |
| efs-fuse | evictor thread 94% CPU, 4.0 GB RSS | hottest thread ≤ 27%, 0.7–1.0 GB | same |
| Serial create p50 | 207 ms | 13.9 / 8.7 / 5.6 ms | 5.6 ms |

Writer 2 ran after the node9900 md5 reader had finished (1,000,000 files at
1,096/s), so its load was lighter. The old run also had a writer on
node9901. These are not identical loads; the comparison that matters is
that each new run kept its rate flat.

**EIO, and why it is not this change.**
- **The stall:** at 06:46:55–06:47:10 all three daemons snapshotted
  group 2. Then each rotated its Raft log (5.8 GB old, 1.5 GB live): 14.0 s
  on the leader, 12.7–12.8 s on the followers.
- **What the client saw:** BUSY, until the inode-RPC retry budget ran out
  (16 retries, 10.3 s) and returned EBUSY.
- **What failed:** one `tiny_files_md5.py` read (`errors=1`), and a
  metadata flush for an open file (`flush-meta: resource busy`). That flush
  surfaced as EIO on close in `tiny_files.py`, which stopped the writer.
- **Not new:** the same 14–15 s rotation hit all three daemons at
  2026-10-16T23:17Z under the previous build. It recurs whenever the log
  reaches about 5.8 GB. It is a separate server-side gate.

Raw output (outside Git): node9900
`/data1/efs/orcd-mini-client/prof/evict-wl-20261017-0635/`
(`writer.log`, `writer2.log`, `md5.jsonl`, `crawl.log`), the efs-fuse logs
on both nodes, and node9901 `/data1/efs/logs/efsd-n{1,2,3}.log` around
06:47Z.

## Limitations and remaining gates

- **Raft log rotation stalls metadata for about 14 s on all three nodes at
  once.** Client requests then exhaust their 10.3 s retry budget, and
  applications see EBUSY or EIO. Fix (not yet deployed):
  [Raft log rotation off the metadata path](raft-rotate-stall-20261017.md).
- **The first-minutes drop is open.** About 35% within 4 minutes, with
  eviction disabled too. It needs a server-side profile; growing
  directories are the first suspect.
- **One run per long arm,** at a 16 MB cap; production uses 256 MB.
