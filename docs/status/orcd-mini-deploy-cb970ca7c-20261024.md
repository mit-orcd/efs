# orcd-mini fresh-format deploy of devel `cb970ca7c`, POSIX suites (RDMA, direct I/O) — 2026-10-24

**Deployed; core, extras, durability, `.efs` and two-client suites PASS.
The expiry suite is NOTRUN: its fixture stops at `raft-mkfs`, which returns
NOT_PRIMARY because it is sent before group 0 has a leader (harness bug
since W153, below).** User request: deploy latest devel to orcd-mini with
RDMA and direct I/O and run all POSIX suites, including two-client. W153
refuses pre-W153 stores, so the user authorized a wipe and fresh format.

## Build and deploy

- **Source:** devel `cb970ca7c87a` (`v0.3.2-pre-alpha-1190-gcb970ca7c`),
  including K31 (`24e58fb1d`, RDMA reply loss) and W153 (`4026e254d`,
  membership committed at formation). Clean `git archive`, shared build
  identity, built 2026-10-24T00:28:57Z, private libfuse 3.16.
- **node9901** `/data1/efs/releases/cb970ca7c87a`:
  - efsd `ca3030fe8a069d6e68ba9de1592ed38e8abd1ddf6082cffc9108d97bf5e844ba`;
  - efs-fuse `5ea616b60448fb5651faff3af18e16d1ba0d1f04523f5008a1a39fc1a36d86ab`;
  - efs-mgmt `85e6f6136f7df447b53e7e6502c83080aa6ec610f005a3c030ca05aae8952473`.
- **node9900** `/data1/efs/orcd-mini-client/releases/cb970ca7c87a`: efs-fuse
  `ad95dae61d579660db364467e0e47745dc4bc5bd7d713256567c88fd1a9c6d01`.
- **devops:** the copy on node9901 was synced to devops `c05eb59`, which no
  longer sets `EFS_MD_RAFT_N` (efsd refuses that variable since W153).
- **Deploy steps:**
  1. Both mounts were idle (`fuser`); both clients and the daemons stopped
     cleanly.
  2. `n1`–`n3` (389 files, 0 errors) and `.mkfs-done` were wiped, and
     the `src` links switched.
  3. `start.sh` formed the cluster with 3 members. Its first `raft-mkfs`
     returned NOT_PRIMARY; the retry succeeded.
- **Environment:** three efsd on node9901 (10.1.99.1:17432–17434), RDMA on
  `mlx5_0` (segmented-v1 transport up), direct I/O, 720 GiB quota per node,
  `.efs` commands on. Clients: node9901 `/data1/efs/mnt` and node9900
  `/data1/efs/orcd-mini-mnt`.

## Results

| Suite | Command | Result |
| --- | --- | --- |
| Core POSIX | devops `smoke.sh` (`EFS_POSIX_JOBS=4`) | 216/216 PASS (73 s) |
| POSIX extras | `posix_extras.py --jobs 4 --timeout-s 150` | 4/5 PASS, 1 SKIP (`mmap_write_read`, W104) |
| Durability across remount | devops `smoke-persist.sh` | 27/27 prepare + 27/27 verify PASS |
| `.efs` virtual interfaces | `tests/live/virtual_interfaces.py --freshness-s 150`, temporary peer mount | 10/10 PASS |
| Two-client (node9901 ↔ node9900) | `scripts/orcd_mini_posix2.py` from the Mac | 68/68 PASS (314 s) |
| Expiry lifecycle (private four-node fixture, /tmp, port 24410) | `posix_expiry.py --suite expiry` | **NOTRUN**: fixture stopped at `raft-mkfs` (below) |

## Harness defect

`tests/live/gc_reclamation.py`, which builds the expiry fixture, calls
`efs-mgmt raft-mkfs` once as soon as every member has joined. Since W153,
formation needs group 0 to have a leader, and right after the members join
it may not have one. The call then returns -15 (NOT_PRIMARY), and the
harness treats that as fatal. devops `start.sh` hits the same response
and retries. Fix: retry `raft-mkfs` on NOT_PRIMARY within a bounded
deadline, as `start.sh` does.

## Reproduce

- **Deploy:** as in [the D25 deploy](orcd-mini-d25-deploy-20261022.md), with the
  fresh-start steps.
- **Suites:** run on node9901 in a screen session, in order: smoke.sh,
  posix_extras.py, smoke-persist.sh, virtual_interfaces.py (with
  `EFS_QUERY_TEST_HOST=10.1.99.1 EFS_QUERY_TEST_PORT=17432` and a peer mount
  under /tmp), then posix_expiry.py (roots and mounts under /tmp).
- **Two-client:** `python3 scripts/orcd_mini_posix2.py RESULTS.tsv` from the Mac.
- **Raw output:** on node9901 under `/data1/efs/evidence/orcd-mini-posix-cb970ca7c-20261024-0033`, plus the two-client TSV on the
  driver Mac.

## Remaining

- Rerun the expiry suite once the fixture retries formation.
- Not run: ior, `make test-all`.
