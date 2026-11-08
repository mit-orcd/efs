# K38 shared-chunk pressure on a distinct-host RDMA cluster (Task DW)

Task DW, Nov 7 2026. Purpose: the real-cluster, distinct-host RDMA shared-chunk acceptance that row K38 of the [checklist](../alpha-1.0-2plus1-checklist.md) and of the [Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) owe (matrix run H7, the two-client case of O1): two `efs-fuse` clients on two hosts writing one shared file against three `efsd` on three other hosts, the K38 pressure shape, over RDMA, with TCP as the control. orcd-mini was not touched.

**Verdict: PASS for the matrix's H7 shared-chunk case on distinct hosts, RDMA and TCP. O1 (orcd-mini `ior hard`) NOTRUN. K38 stays OPEN, blocks alpha: yes.** RDMA 10 of 10 runs and TCP 4 of 4 runs passed: no `EIO` from `write()`, `fsync` or `close()`, every byte exact on both mounts and after a remount, and no `publish-stalled` or `why=image-base`. K38 stays OPEN until the user approves the orcd-mini `ior hard` rerun, it is run, and the user accepts this result.**

## What ran

`tests/test_k38_shared_chunk_pressure.py` starts its daemons and mounts on loopback only, so its write and verify phases were cut out unchanged into [`scripts/evidence/dw/k38_dw_client.py`](../../../scripts/evidence/dw/k38_dw_client.py) and driven on five hosts by [`scripts/evidence/dw/k38_dw_campaign.py`](../../../scripts/evidence/dw/k38_dw_campaign.py). No product code changed.

Per run, on a fresh cluster (new stores):

1. Three production `efsd` (not the `EFS_FAULTS` build): node 1 seed on fcstor012, nodes 2 and 3 `--join` on fcstor013 and fcstor014, endpoints on IPoIB `172.16.223.66-68:18432`, `--quota 1G --writers 2 --discovery-port 0` (the test's flags), stores `/data1/01/erbmi1/efs-taskDW/<run>/nN` (local XFS, owned by `erbmi1`, checked before start). Wait for `raft-host: formed: 3 members` on all three.
2. Mount A on fcstor010 through node 1, mount B on fcstor011 through node 2, client env `EFS_DCACHE_HARD_BYTES=33554432 EFS_DCACHE_BYTES=1048576` (the test's 32 MiB body budget and 1 MiB reclaim target). Buffered FUSE I/O through the page cache, as in the test; fragment I/O on the servers is `O_DIRECT` (built in).
3. A truncates `shared` to 24 × 200 × 47,008 = 225,638,400 bytes. 24 writer threads, even ranks on A, odd ranks on B, start together (a start time read from fcstor010's clock, both hosts NTP-synchronized): record k of writer r at offset (k × 24 + r) × 47,008, so most 128 KiB chunks hold spans of two or three writers of both clients; one `fsync` per writer at the end, then `close`. Write-phase seconds = writer start to the last `close` (the slower client).
4. Verify every record on A and on B, unmount both, mount through node 3 on fcstor010 and verify again.
5. Stop both mounts (`fusermount3 -u`, wait for the pid), stop the daemons by pid (`SIGTERM`, wait for the pid), copy every log to the raw-capture directory, remove the run's `/tmp/efs-taskDW/run` and store directories.

Transport: `EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2` (InfiniBand HCA; `rdma`, never `auto`, so there is no silent downgrade) on every daemon and client, or `EFS_TRANSPORT=tcp` for the control on the same IPoIB addresses. Every RDMA-arm daemon and client log starts with `transport rdma: rdma on mlx5_2 (TCP carries setup)` and counts `RDMA transport up segmented-v1` lines (column "RDMA up"); the TCP arm shows `transport tcp` and none.

Pass per run: no error from `write()`, `fsync` or `close()` on either client; every one of the 4,800 records exact on A, on B and after the remount; the clients ran under dcache pressure (`dcache-pressure` lines > 0); no `report-stuck`, `report-span-lost`, `publish-stalled` or `why=image-base` in any client or daemon log. Reported apart, not a K38 criterion: daemon lines `segmented RDMA setup failed or incompatible peer` (the K94 symptom).

## Results

| Run | Verdict | Write s, A / B | `dcache-pressure` lines | STALE replay rounds | `report-stuck` / `report-span-lost` / `publish-stalled` / `image-base` | Records exact A / B / remount (of 4,800) | `RDMA transport up` daemons / clients | K94 lines (daemons) |
| --- | --- | --- | ---: | ---: | --- | --- | --- | ---: |
| rdma-r01 | PASS | 2.08 / 3.11 | 18 | 5 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 285 / 211 | 0 |
| rdma-r02 | PASS | 2.26 / 3.06 | 21 | 4 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 290 / 214 | 0 |
| rdma-r03 | PASS | 2.83 / 1.84 | 19 | 3 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 291 / 221 | 0 |
| rdma-r04 | PASS | 2.36 / 1.72 | 20 | 3 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 293 / 223 | 0 |
| rdma-r05 | PASS | 2.39 / 3.55 | 21 | 4 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 290 / 212 | 0 |
| rdma-r06 | PASS | 2.13 / 3.13 | 18 | 4 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 291 / 211 | 0 |
| rdma-r07 | PASS | 2.0 / 2.44 | 16 | 10 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 294 / 214 | 0 |
| rdma-r08 | PASS | 2.56 / 1.72 | 18 | 5 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 312 / 228 | 0 |
| rdma-r09 | PASS | 2.51 / 1.63 | 19 | 3 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 294 / 218 | 0 |
| rdma-r10 | PASS | 2.35 / 3.8 | 28 | 3 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 289 / 211 | 0 |
| tcp-r01 | PASS | 2.18 / 1.66 | 18 | 3 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 0 / 0 | 0 |
| tcp-r02 | PASS | 1.72 / 2.2 | 16 | 2 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 0 / 0 | 0 |
| tcp-r03 | PASS | 2.26 / 1.72 | 16 | 4 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 0 / 0 | 0 |
| tcp-r04 | PASS | 1.82 / 2.43 | 16 | 5 | 0 / 0 / 0 / 0 | 4800 / 4800 / 4800 | 0 / 0 | 0 |

Summary: RDMA 10 of 10 PASS, TCP 4 of 4 PASS. 0 `EIO` or other error from `write()`, `fsync` or `close()` (24 writers × 14 runs). 67,200 records written, each verified on both mounts and after the remount: all exact. 0 `report-stuck`, `report-span-lost`, `publish-stalled`, `why=image-base` or any `pub-stale` in any client or daemon log. Every run ran under dcache pressure (16 to 28 lines) with 2 to 10 STALE replay rounds, so the clients' REPORTs raced each other on shared chunks as the K38 shape requires. Write phase (slower client), RDMA 2.36 to 3.80 s (median 2.95 s), TCP 2.20 to 2.43 s (median 2.23 s).

**K94 (reported apart, not a K38 criterion):** 0 `segmented RDMA setup failed or incompatible peer` lines in the 30 RDMA-arm daemon logs and 0 in the client logs. The 18 to 22 s write phase K94 caused on node9900 loopback did not appear on this distinct-host cluster. That does not show K94 is fixed: its trigger (a forwarded read proof whose 400 ms budget is spent) may just not come up in this shape.

Shakedown, not counted: `shakedown-rdma-r00-harness-quote` stopped at the formation wait on a quoting bug in the controller. The cluster had formed over RDMA, nothing had been written, and teardown ran clean. After the fix (`113d583fd`), `shakedown-rdma-r00` passed: write 2.47 / 3.68 s, 23 pressure lines, 4,800 exact ×3, K94 0. Both sets of logs are kept with the raw output.

## Source, build, environment

- Source: branch `claude/elegant-ramanujan-04217e` at `9bf2848e5` = devel `826d0ef22` (wave 61) plus the two `scripts/evidence/dw/` scripts; `src/`, `include/` and `Makefile` equal devel. The controller fixes `113d583fd` (formation-wait quoting, RDMA-up count, before the counted runs) and the next commit (the per-log transport regex, which missed `transport tcp (EFS_TRANSPORT=tcp)`; the TCP lines were read from the kept logs instead) only change the controller, which runs on the Mac; the remote client script is byte-identical (sha256 below).
- Build: once, on fcstor012, from `git archive 9bf2848e5` into `/tmp/efs-taskDW/src`: `make -j24 EFS_GIT_ID=9bf2848e5… EFS_GIT_DESCRIBE=9bf2848e5… EFS_GIT_BRANCH=claude/elegant-ramanujan-04217e FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$P/lib" efsd efs-fuse efs-mgmt` with `P=~/orcd/scratch/efs/orcd-test/deps/libfuse` (libfuse 3.16.2), gcc 11.5.0, `-O3 -march=native`. Binaries copied to `~/efs-taskDW/bin` on the shared home and checked by sha256 on all five hosts. **Deviation from the task brief:** the brief said to build on node9900; node9900 is Zen 4 (EPYC 9474F) and the fcstor hosts are Zen 2 (EPYC 7402P), and the Makefile builds `-march=native` ("compile on the same arch you run on"), so a node9900 binary could stop on an illegal instruction there. The build ran on one fcstor host instead.
- Identity: `efsd 9bf2848e50d0b284c79c477d9e477b78ab0334a2 (branch claude/elegant-ramanujan-04217e, built 2026-11-07T23:45:05Z on fcstor012)`, the same for `efs-fuse`. sha256: `efsd` `970935fed68e482c01377eef87afe947d502499f29ea48ce550b1b34142c48e5`, `efs-fuse` `e8dee6d3ce37a120bd64e31edd2fbec0badcddb1b0125e25d6d20976dc20b739`, `efs-mgmt` `1e9c8dfeeff46420cb9f92b06de666bd8c9593e5a3f8e65e80f18494a420f86b`, `k38_dw_client.py` `ce3ef9073fd71d525862cd95c48c2e77e99dde2a2dffd2b7fde7841ff58d9d5c`.
- Hosts: fcstor010–014, each AMD EPYC 7402P (24 CPUs), Linux 5.14.0-687.39.1.el9_8, RHEL 9, Python 3.9.25, Mellanox `mlx5_2` InfiniBand port ACTIVE (IPoIB `ibs1f0` 172.16.223.64–68), wired `ens10f0np0` 10.1.223.64–68 unused. Load average 0.00–0.03 at the start. fcstor015 refused the `erbmi1` key, so fcstor010/011 carry the clients instead of the A4 dry run's fcstor012–015 set. fcstor010–013 are orcd-test client hosts in `~/git/cluster/orcd-test.py`; at the start and throughout no `efsd` or `efs-fuse` of any user ran on them, `/tmp/efs-mount` was not mounted, and nothing listened on 17xxx–19xxx. fcstor003–006 (orcd-test stores), node9900 and node9901 were not used.
- Preflight per run, every host: nothing listening on `:18432`, no `efsd`/`efs-fuse` of `erbmi1`, no `/tmp/efs-taskDW/run`.
- Access: ssh as `erbmi1` through `orcd-login` (the live mux socket), BatchMode, nested ssh from the login node; no password or token prompt.

## Reproduction

Prerequisites: ssh to fcstor010–014 (a wrapper `W HOST` that runs `bash -s` on HOST from stdin), the private libfuse at `~/orcd/scratch/efs/orcd-test/deps/libfuse`, port 18432 free, `/data1/01` writable.

```sh
C=$(git rev-parse HEAD)
git archive $C | ssh … fcstor012 'mkdir -p /tmp/efs-taskDW/src /tmp/efs-taskDW-tmp && tar -xf - -C /tmp/efs-taskDW/src'
# on fcstor012, in /tmp/efs-taskDW/src (P as above):
PKG_CONFIG_PATH=$P/lib/pkgconfig make -j24 EFS_GIT_ID=$C EFS_GIT_DESCRIBE=$C EFS_GIT_BRANCH=<branch> \
  FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$P/lib" efsd efs-fuse efs-mgmt
mkdir -p ~/efs-taskDW/bin && cp efsd efs-fuse efs-mgmt scripts/evidence/dw/k38_dw_client.py ~/efs-taskDW/bin/
# on the controller (any host with the wrapper):
DW_SSH=W python3 scripts/evidence/dw/k38_dw_campaign.py --arm rdma --runs 10 --out OUT
DW_SSH=W python3 scripts/evidence/dw/k38_dw_campaign.py --arm tcp --runs 4 --out OUT
```

Each run takes about 80 s (14 runs in 19 minutes). `OUT/<arm>-rNN.json` holds the per-run result; the logs land in `DW_ART/runs/<arm>-rNN/<host>/` (default `~/efs-artifacts/taskDW-20261107`). A writer error, a differing record or a harness error stops the campaign with every log kept. Cleanup: the controller removes each run's directories; afterwards remove `/tmp/efs-taskDW`, `/tmp/efs-taskDW-tmp` and `/data1/01/erbmi1/efs-taskDW` on each host and `~/efs-taskDW`, and check `ss -ltn` and `pgrep -u $USER efsd`.

## Teardown

Each run stopped its mounts and daemons by pid and removed its run and store directories (every `stop` and `unmount` returned 0). Afterwards, on fcstor010–014: no `efsd` or `efs-fuse` of `erbmi1`, nothing listening on `:18432`, and `/tmp/efs-taskDW`, `/tmp/efs-taskDW-tmp` and `/data1/01/erbmi1` removed (`edelete`) and absent (`ls`). `~/efs-taskDW` (the shared binaries) was also removed. No orcd-test or orcd-mini process, port or mount was touched.

## Limits and what remains

- **orcd-mini `ior hard` rerun (matrix O1): still owed, NOTRUN.** O1 asks for `ior hard`, 8 ranks, one shared file, RDMA, direct I/O, at least 4 runs on orcd-mini (node9901 daemons, node9900 client). It needs the user's OK because it runs on the live orcd-mini cluster; this task never touched orcd-mini. K38 stays OPEN until that rerun and the user's acceptance of this result.
- This is the test's shape (buffered FUSE I/O, 47,008-byte records, 24 writers on two clients), not `ior` and not direct I/O on the client.
- One topology: 3 daemons + 2 clients, one HCA type, one subnet; not the D46 fcstor007–011 cell layout or the 13-node shape.
- No forced trigger: the `image-base` path is not reached on purpose here (Task DG covers that); a clean run shows the natural K38 shape no longer fails on distinct hosts, not that every path is closed.
- The other half of matrix run H7 (K12 BUSY/lost-reply exclusive-create retry with timelines) is not covered. K40 names the same two-client run; its cell is not changed here (cross-reference only).
- K94 is not fixed (Task DU in parallel); its symptom count is reported only.

Raw output is outside Git: `~/efs-artifacts/taskDW-20261107/` (shared `erbmi1` home on the ORCD hosts): build log, `binaries.sha256`, every run's daemon and client logs, the per-run JSON, the controller log. `SHA256SUMS` sha256 `0097fb61914afc9f4d7071c2502739fda401d2e5fcd27aff503b5d65aeeda612` (114 files, 4.5 MB).
