# 20261011-orcd-integration — evidence summary

## Purpose and linked item

ORCD branch-integration acceptance: after merging 33 AMD/Intel/NUC commits
into devel through `eb7d449d`, run the POSIX and cross-client (peer) suites
against a freshly deployed four-daemon cluster, then repeat with a corrected
symlink-follow fixture. Linked status page:
`docs/status/orcd-integration-20261011.md`; run preserved by commit
`82f0f59d4` ("record merged rollout and unresolved repeated-load gates").
Open contention investigation tracked under W66/W23/D30.

## Verdict

**FAIL** (mixed). Original runs failed only the newly merged symlink-follow
fixtures (fixture defect, corrected in tooling `368c819f`). After the fixture
correction, all four cross-client peer suites passed (268 pass, 0 fail, rc 0),
but the repeated seven-client full POSIX suite under load showed real
failures: 1485 pass, 27 fail, 7 skips, 28 NOTRUN, rc 1 on all seven clients.
Per the status page, the load gates must not be closed on the passing smoke
or peer runs.

## Source commit and build identity

- Runtime snapshot `52bc8edb3d1a46196239b1139090eeb54d9645c3` (recorded as
  `revision` per host in `runtime-audit.json`; also `52bc8edb` in the status
  page). Compiled on all eleven hosts; passed complete Linux `make test`.
- Installed test tooling `368c819f` (corrected symlink-follow fixture).
- No `deployed-version.txt`/sha256 manifest in the run; identity comes from
  `runtime-audit.json` revisions and the status page.

## Environment

- Cluster: fcstor003–fcstor006 run four `efsd` daemons (node-ids 1–4,
  172.16.223.57–60:17432), six storage dirs each (`/data1/01..06/
  efs-orcd-test-erbmi1`), `--direct-io`.
- Clients: fcstor007–fcstor013, seven `efs-fuse` mounts.
- Transport: native RDMA on `mlx5_2` (`EFS_TRANSPORT=rdma`,
  `/dev/infiniband/uverbs0`+`uverbs2` open); commands server enabled.
- Quota ≈29.31 TiB per node; usable 2+1 logical capacity 78.15 TiB.
- User file `/001/dat01` (107374182400 bytes) verified by sampled hashes
  pre/post deployment (sampled integrity, not full checksum).

## Method

Deployment and gate procedure are described in
`docs/status/orcd-integration-20261011.md`; bulk logs/TSVs were kept outside
Git under `~/orcd/scratch/efs/orcd-test/` (`test-52bc8edb3d1a.log`,
`merged-52bc8edb-20261011/`, `linkat-gates-20261011/`). Sequence: drain seven
clients, stop four daemons, switch runtime, rebuild on eleven hosts, run Linux
`make test`, run per-client POSIX suites and four cross-client peer suites
(original), fix the symlink-follow fixture (`368c819f`), rerun native XFS/EFS
symlink controls and the corrected full + peer suites. Seven-client full suite
runs concurrently (loaded acceptance). No reproduction script is committed in
this run dir; per-suite results are in the two JSON gate files.

## Results

Original gates (`original-gates.json`):

| Suite | Pass | Fail | Skip | NOTRUN | rc |
| --- | --- | --- | --- | --- | --- |
| posix ×7 clients (each) | 218 | 2 | 1 | 0 | 1 |
| peer ×4 pairs (each) | 66 | 1 | 0 | 0 | 1 |

All original failures were `link_of_symlink_follow` /
`link_of_symlink_dangling` — a fixture bug (plain `os.link` does not follow on
XFS with Linux CPython; fixed by requesting linkat explicitly).

Corrected gates (`corrected-gates.json`):

| Suite | Pass | Fail | Skip | NOTRUN | rc |
| --- | --- | --- | --- | --- | --- |
| posix fcstor007 | 217 | 3 | 1 | 0 | 1 |
| posix fcstor008 | 210 | 3 | 1 | 7 | 1 |
| posix fcstor009 | 207 | 6 | 1 | 7 | 1 |
| posix fcstor010 | 215 | 5 | 1 | 0 | 1 |
| posix fcstor011 | 210 | 3 | 1 | 7 | 1 |
| posix fcstor012 | 215 | 5 | 1 | 0 | 1 |
| posix fcstor013 | 211 | 2 | 1 | 7 | 1 |
| peer ×4 pairs (each) | 67 | 0 | 0 | 0 | 0 |

Corrected failures are distinct real defects: EBUSY during directory
create/remove/cleanup (`dir_many_files`, `names_crazy_dirs`,
`err_rmdir_nonempty`, `perm_*`), and 30 s timeouts on deep-directory and
virtual-query cases (`dir_deep_nesting[_beyond_64]`, `virt_find_query`,
`virt_stats_readable`, `statfs_sane`). Four suites lack a complete marker
(seven NOTRUN each) after stuck workers; workers later retired naturally
(`retired-workers.json` shows none outstanding). Failure-window logs (outside
Git) show compaction activity and repeated `apply lane-sweep rc=-13` on inodes
39615 and 43395; causality not proven.

## Limitations and remaining gates

- Loaded repeated full-suite acceptance remains open (W66/W23/D30 contention
  investigation). Do not treat the passing peer/cross-client suites as load
  acceptance.
- File-integrity check is sampled (four 64 KiB hashes), not a full checksum.
- Bulk logs and per-client TSVs are not preserved in this run dir; only the
  aggregate gate JSONs are here.

## Evidence index

| File | sha256 (16) | Content |
| --- | --- | --- |
| `original-gates.json` | a720426c265c9c91 | Original posix+peer results; symlink-fixture failures |
| `corrected-gates.json` | 58b460cf3319b95e | Corrected rerun; real load failures, NOTRUN counts |
| `runtime-audit.json` | a3bf7e1e65f14d60 | Per-host process/env/revision audit (RDMA, direct-I/O) |
| `retired-workers.json` | 94dfc4a5b36f7ac9 | Final stuck-worker audit (all empty) |
