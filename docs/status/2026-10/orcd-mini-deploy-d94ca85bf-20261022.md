# orcd-mini deploy of devel `d94ca85bf` with suites and ior — 2026-10-22

**Deployed; every functional suite PASS. ior easy write dropped by about 40%
against `08cc16af4` (unexplained; A/B NOTRUN).** User request: deploy the new
devel to orcd-mini and the node9900/node9901 FUSE clients, then run all test
suites and ior. No format change, so no wipe. No backward compatibility
(AGENTS.md).

## Build and deploy

- **Source:** devel `d94ca85bf35a`. Since `08cc16af4` it adds:
  - W143 (client diagnostic environment switches read once);
  - W144 (the leader hint reads voters from the published view);
  - W145–W147 (snapshot-install commit bound, `raft-resync`, snapshots over
    4 GiB, the open snapshot image kept while committing);
  - the pack-validate-once merge (`842a014b3`);
  - harness and docs changes.
- **Build:** clean `git archive` on each node, `make -B`, `EFS_GIT_ID=d94ca85bf35a`,
  describe `v0.3.2-pre-alpha-902-gd94ca85bf`, private libfuse 3.16.
- **Binary hashes:**

  | Node | Binary | sha256 |
  | --- | --- | --- |
  | node9901 (`/data1/efs/releases/d94ca85bf35a`) | efsd | `e75f1e064fd3591675d79065ed4a4311b3da3097b43b93a9bf0432157ab0d21b` |
  | node9901 | efs-fuse | `640789d21d5c3615089092146d84388af94e68150620da6b09cedacd938cdbeb` |
  | node9901 | efs-mgmt | `13c954d140bd4ae7545586c8261a3e0928357886ddb1271eb75def48a4062e80` |
  | node9900 (`/data1/efs/orcd-mini-client/releases/d94ca85bf35a`) | efs-fuse | `eb1aeb1ebe251f1d2729eef28f2da4db13dcf7c51cc218fe3a8e87c4da66055c` |

- **Deploy at 17:05Z:**
  1. Before the stop, `fuser -m` was empty on both mounts.
  2. Stop order: node9900 `client.sh stop`, node9901 `client.sh stop`, then
     devops `stop.sh`.
  3. No efsd was left running; then the `src` symlinks were switched.
  4. `start.sh` with `EFS_START_TIMEOUT=900` kept the existing storage; then
     node9900 `client.sh start`.
- **After start:** all three nodes agree on group 0 (term 971, commit 35,559,761)
  and group 2 (term 1135, commit 34,004,312), and n1 leads both.
- **Environment:** three efsd on node9901 (RDMA mlx5_0, direct I/O,
  720 GiB quota each). Clients on node9901 `/data1/efs/mnt` and node9900
  `/data1/efs/orcd-mini-mnt`. The cluster held 5.3 GiB (logical) at test
  time.

## Results

| Suite | Result |
| --- | --- |
| Core POSIX (`smoke.sh`, jobs 4) | 216/216 PASS |
| POSIX extras (`posix_extras.py --timeout-s 150`) | 4/5 PASS, 1 SKIP (mmap) |
| Durability (`smoke-persist.sh`) | 27/27 + 27/27 PASS |
| ior hard (8 ranks, 47008 B × 2000) | rc 0; write 47.9 MiB/s, read 240 MiB/s |
| `.efs` virtual interfaces (`virtual_interfaces.py --freshness-s 150`) | 10/10 PASS, including `nested_authorization` |
| Two-client (`scripts/orcd_mini_posix2.py`, from the Mac) | 67/67 PASS (371 s) |
| ior easy (8 ranks, 1 MiB × 2 GiB per file) | rc 0; three runs below |

ior easy, three runs on `d94ca85bf` against earlier builds on the same
cluster:

| Build | Write MiB/s | Write time / close (s) | Read MiB/s |
| --- | ---: | --- | ---: |
| `49679a570` (02:58Z) | 1,304 | 12.6 / 5.3 | 6,296 |
| `08cc16af4` (04:34Z) | 1,329 | 12.3 / 6.4 | 4,391 |
| `d94ca85bf`, chain | 710 | 23.1 / 11.0 | 4,090 |
| `d94ca85bf`, rerun 1 | 863 | 19.0 / 8.0 | 5,300 |
| `d94ca85bf`, rerun 2 | 721 | 22.7 / 9.5 | 4,878 |

The write slowdown repeats across three runs and comes with longer close
(flush) times; reads are within earlier variation. It is **not attributed**:
- **Code:** no change between the builds touches the large-file write path
  directly.
- **Environment:** the cluster's history differs since `08cc16af4`. That
  includes the 32 M-file workload and its deletion, the W145 repair and a
  5.2 GB group-2 snapshot install.
- **Closing it:** this needs an A/B/A′ on this cluster (switch to
  `08cc16af4` binaries, run ior easy twice, switch back). That restarts
  orcd-mini, so it is NOTRUN here pending the user's go-ahead.

## Reproduce

- Mounted chain on node9901 in screen: core, extras, persist, ior easy/hard
  (OpenMPI 4.1.7, `~/orcd/scratch/efs-io500/io500/bin/ior`) and virtual
  interfaces with a temporary peer mount `/data1/efs/mnt-vi-peer`. Set
  `EFS_QUERY_TEST_HOST=10.1.99.1 EFS_QUERY_TEST_PORT=17432`.
- ior easy: `mpirun -np 8 --host localhost:8 ior -a POSIX -F -e -w -r -t 1m -b 2048m`.
- ior hard: `mpirun -np 8 --host localhost:8 ior -a POSIX -e -w -r -t 47008 -b 47008 -s 2000`.
- Two-client: `python3 scripts/orcd_mini_posix2.py RESULTS.tsv` from the Mac.

Raw output stays outside Git, on node9901 in
`/data1/efs/prof/suites-20261022-1711/`, with the ior reruns as
`ior-easy-rerun{1,2}.out`. The ior directories were deleted afterwards with
`edelete`.

## Remaining gates

- The ior easy write A/B (above).
- The packing read mismatch stays OPEN
  ([review](review-pack-validate-once-20261022.md)).
- `make test` on this commit was not run here; `842a014b3` passed 819/819.
