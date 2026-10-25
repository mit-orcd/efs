# Stage 2 gates A13, C5, D1/D2, D3 and E4 in `scripts/qualify.py`, Oct 24–25

**Verdict: PASS (scoped, loopback dry run): no stub gate is left; every
Stage 2 item maps to a real gate. On a driver-owned loopback-3 cluster on
xorinox the five new gates PASS end to end in one full topology run.
Real-host runs NOTRUN (parent). The code gate is not clean: xorinox `make -k
test-all` at `babc80ced` exits 2 on one test, `test_b6_unavailable.py`
(refuse mode), which fails 3 of 3 on the base `646a43041` too.** Branch
`claude/devel-stage2-gates-359865` on `merge-wave6` (`646a43041`); not
merged, not pushed.

## Purpose and items

[A11](a11-qualification-matrix-20261024.md) left five Stage 2 gates as
NOTRUN stubs. This branch replaces them with bounded gates that run on real
distinct hosts over ssh and on a private loopback topology, so Stage 2 can
run end to end. Items: [checklist](alpha-1.0-2plus1-checklist.md) A13, C5,
D1, D2 (on every topology, beside `gc-reclamation` once), D3 and E4.

## The gates

Each gate records every command (host, start, end, deadline, exit), its
fixture and counts, and PASS/FAIL/NOTRUN with the reason in
`gates/NAME/result.json`, like the existing gates. The work on a mount host
runs from the candidate tree as `scripts/stage2_helpers.py` (one JSON line
out). Fixture defaults are declared in `scripts/qualify_matrix.json`
(`fixture`; the driver's `DEFAULT_FIXTURE` must hold the same values, which
`qualify.py plan` checks).

| Gate | Items | Needs | What it checks |
| --- | --- | --- | --- |
| `a13-node-addition` | A13 | `--allow restart` | Forms its own cluster (seed + `--join`, 2G quota each) on the topology's hosts, member *i* on the host of topology node *i* mod N at that node's port + 100 (store and mount under the private cluster root; on loopback under the work directory). A client load runs through its own mount (4 writers, write + fsync + read-back, 30 s per-op deadline). 3 → 4: member 4 joins through member 2, is SIGKILLed 0.1 s after it starts and joins again with the same store and command: it must be member 4 (not 5). SHARD2 voters move `{1,2,3}` → `{2,3,4}` and no hosting member is left in a joint configuration (a retired replica, `retired=1`, does not count). 4 → 5 with the seed stopped: member 5 joins through member 3, the seed restarts, member 5 hosts no metadata group. Usable capacity at 5 = 2/3 of every member's quota (status and the client's `statvfs`), new writes put bytes on members 4 and 5, every manifest (before, after, the load's acknowledged files) exact. Without a driver-owned cluster or an `a13` block naming five endpoints: NOTRUN. |
| `c5-protocol-live` | C5 | — | W133 over the wire on every member: undeclared HEARTBEAT served, an undeclared request closed without a reply, a protocol 4 declaration answered `EFS_ERR_PROTO` with version 5 then closed, a bad magic refused, protocol 5 accepted. W140: an unknown tag (254) and a reserved tag (46) proposed to groups 0 and 2 (following NOT_PRIMARY hints to the leader) are refused `EFS_ERR_PROTO`; the cluster still serves. A6/A7 on every node and mount host: the candidate `efsd` and `efs-fuse` refuse 12 removed settings (`EFS_COMMANDS`, `EFS_MD_RAFT_N`, `EFS_READ_VERIFY`, `EFS_METADATA_BALANCE`, `EFS_UNMOUNT_REVOKE`, `EFS_FUSE_EXPORT`, `EFS_EXPIRY`, `--no-direct-io`, `--direct-io`, `--no-persist`, `--node-id`) naming them, without creating a store or mounting. K5/W134: an extra mount of the topology cluster is fenced (`raft-session fence`, up to 12 attempts); 10 mutations then fail EIO, metadata reads answer, a peer mount mutates and sees the pre-fence state, the client logs the fence; the fenced mount is detached with `--force-discard`. W72: a second extra mount writes 3 MiB without fsync and stops cleanly (no force); its session epoch has advanced and the bytes read back exact through the topology mount. |
| `d1-packing` | D1, D2 | partial: `--allow restart` | 400 small files (1 B–8 KiB, fsync) read back exact through every mount; the packer switches all of them to packed rows (`raft-getchunks`, wait up to 600 s); fewer stored objects than files (inventory saving); exact again. D2: overwrite, truncate (to 0 or half) and unlink one in eight each; exact (unlinked names gone); an explicit `pack-compact` must move records within 300 s; every remaining file packed again (an overwrite overlays spans on its packed base until it is repacked; a logically truncated file may keep a longer reference, D25 clips reads; a reference shorter than the size fails); exact. Cold restart: client stops (no force), orderly cluster stop and start, remount, every file exact and still packed. Without `--allow restart` the restart part is NOTRUN. |
| `d3-efs-load` | D3 | — | A fresh fixture (200 files in 4 directories: initial catalog creation) must be indexed (`.efs/stats` exit 0 within 600 s) and published exactly. Then for 300 s: 8 writers (create + fsync + read-back + stat + list, every fifth renamed; 10 s per-op deadline) and 4 query loops through the second mount (`.efs/find` suffix, `.efs/ls`, `.efs/stats`, and an invalid request that must be refused at once, exit 2). Answers must be complete and exact; every query within 30 s. EBUSY while publications run is a bounded refusal: retried, and the same kind must answer exactly within 30 s of the first refusal. "Not indexed yet" from a member whose catalog has not refreshed is tolerated for one settle window (2 × refresh + 60 s) after the fixture was created. New catalog generations must be published under load; the load tree converges to its exact file count and bytes; every load file reads back exact. |
| `e4-scale` | E4 | partial: `--allow remount` (one mount only) | Declared fixture sized by the member count N: N × `e4_small_per_member` small files (1 B–16 KiB) and N × `e4_large_per_member` files of `e4_large_mib` MiB, 16 writers, fsync each; NOTRUN unless twice the declared size is free. Completed vs target counts. Cold reads: every file exact on the second mount (or on this one after a clean remount) while a `.efs/stats` query loop runs (30 s deadline each); the catalog converges to the exact count and bytes; all large files and every second small file deleted; usable used bytes must fall by at least 0.8 × the deleted payload within 900 s; the rest exact. |

Destructive steps (all listed in `run.json` and the summary): A13 formats,
stops, kills and restarts only the daemons of its own cluster (`restart`);
D1's cold restart and E4's remount are E1's and E2's steps behind
`--allow restart` / `--allow remount`; C5 fences only the session of the
extra mount it started (listed as `session` steps). The topology matrix:
`loopback-3` now runs all new gates (`loopback_quota` 4G: at 1G per daemon
the whole sequence ran out of space in `d3-efs-load`), the bare-metal and
`nuc-private-3` topologies already listed them, and the two live clusters
add `c5-protocol-live` (they already had D1, D3, E4; A13 needs a
driver-owned cluster). The E4 defaults are 5,000 small and 2 × 256 MiB
files per member; `loopback-3` uses 2,000 and 2 × 32 MiB.

## Candidate and environment

- Code: `52a4a72ce` (gates), `057f58acb1cf` (dry-run fixes), `babc80ced`
  (full-run fixes, `test_get_integrity.py`); base `merge-wave6`
  `646a43041`. Product code is unchanged.
- Host: xorinox (16 CPU, 60 GB, Rocky 10). Private tree from `git archive`
  under `~/efs-stage2`, built with `EFS_GIT_ID=EFS_GIT_DESCRIBE=babc80ced`
  (identity gate PASS: one identity across daemons, clients and tree).
- Topology `loopback-3`: three efsd on 127.0.0.1:19340–19342 (4G quota
  each), two `client.sh` mounts, TCP, `O_DIRECT`, packing and `.efs` at
  their defaults; live harness ports from 19300; the A13 cluster on
  19400–19404 (private port base + 100). One host: A4 not applicable,
  regression evidence only.

## Commands

```bash
git archive HEAD | ssh xorinox 'mkdir -p ~/efs-stage2/gate && tar -x -C ~/efs-stage2/gate'
ssh xorinox 'cd ~/efs-stage2/gate && EFS_GIT_ID=babc80ced EFS_GIT_DESCRIBE=babc80ced \
  EFS_GIT_BRANCH=claude/devel-stage2-gates-359865 make -j16 && make -k test-all'
ssh xorinox 'cd ~/efs-stage2/gate && TMPDIR=$HOME/efs-stage2/tmp-full python3 scripts/qualify.py run \
  --matrix scripts/qualify_matrix.json --topology loopback-3 --port-base 19340 --private-port-base 19300 \
  --out ~/efs-artifacts/stage2-gates-20261024/loopback-3-full --work ~/efs-stage2/work-full'
```

Single-gate dry runs were the same with `--gates identity,NAME` and their
own port ranges (19340/19300, 19440/19400, 19540/19500). Every run ran in
`screen`. Real hosts: `qualify.py run --matrix scripts/qualify_matrix.json
--topology bare-metal-3-tcp|bare-metal-4-tcp|bare-metal-5-tcp` (blocked,
A11) or a live topology with `--user-approved`.

## Results

Full run `loopback-3-full` at `babc80ced` (41.8 min, 18 PASS, 1 FAIL, 1
NOTRUN of 20 gates; deadlines in seconds):

| Gate | Verdict | Seconds / deadline | Result |
| --- | --- | --- | --- |
| `a13-node-addition` | PASS | 34 / 3600 | 3 → 4 → 5; ids 1–5 in order; the killed join had already committed (members after the kill: 4) and the retry was the same member 4; SHARD2 `{1,2,3}` → `{2,3,4}`, member 5 storage only; usable 4,294,967,296 → 7,158,278,826 B (2/3 of 5 × 2 GiB; client `statvfs` the same); members 4 and 5 took new bytes; 293 load ops, 0 errors, max gap 1.3 s; every manifest exact |
| `c5-protocol-live` | PASS | 2.7 / 1800 | wire 33/33 checks on 3 members; 12/12 refusals; fence in 1 attempt, 10/10 mutations EIO, reads and the peer mount fine; clean unmount 0.7 s, session epoch 1 → 2, data exact |
| `d1-packing` | PASS | 181 / 3600 | 400 files: 400 → 19 stored objects (19 segments); mutations 50/50/50 (250 kept); compaction moved 379 records; all 350 remaining packed again (25 with clipped references), 13 segments; exact through both mounts at every stage and after the cold restart (still 350 packed) |
| `d3-efs-load` | PASS | 604 / 2400 | fixture indexed in 44 s; 2,847 queries exact over 6 generations, p99 10.0 s, max 24.4 s (≤ 30); 17 EBUSY refusals, longest streak 26.7 s (≤ 30); 0 "not indexed yet"; 18,607 I/O ops, p99 0.28 s, max 3.0 s (≤ 10); converged in 59 s; 18,607 files exact |
| `e4-scale` | PASS | 137 / 14400 | 6,000/6,000 small + 6/6 × 32 MiB written in 44 s (135 small/s); exact on the second mount in 53 s under 225 queries (p99 0.06 s); catalog exact; 3,006 deleted (215.6 MB), usable used 2,657 → 2,093 MB; the rest exact |
| `gc-reclamation` | FAIL | 35 / 7200 | `fsync acknowledged after unclean stop: four-server crash`; fails the same on the base (finding 2) |
| `topology` | NOTRUN | — | 3 members on 1 host (A4) |
| other 13 gates | PASS | — | identity, b-checks, posix-core/extras/2client/durability, append-failover, k14, k13, fragment-integrity, e1, e2, e3 |

Single-gate dry runs (in order): `c5-r1` PASS; `d1-r1` FAIL (my check: a
truncated packed file keeps a longer reference) → `d1-r2` PASS; `a13-r1`
FAIL (my check: a retired replica counted as a voter) → `a13-r2` FAIL
(`status` exits nonzero while DEGRADED and was discarded) → `a13-r3` PASS;
`d3-r1` FAIL (EBUSY treated as an error; 23 of 1,786 queries) → `d3-r2`
PASS; `e4-r1` PASS. The first full run (`loopback-3-full-r1`, `057f58acb1cf`)
failed `d1-packing` (an overwrite's span overlay on a packed base; fixed),
`d3-efs-load` (ENOSPC at 1G per daemon and a transient "not indexed yet";
fixed) and left `e4-scale` NOTRUN (185 MiB free).

Code gate: xorinox `make -k test-all` at `babc80ced`: 420 commands, exit 2,
1 FAIL (`test_b6_unavailable.py`, below), 12 NOTRUN host limits (no RDMA
device, no `~/git/cluster`, no `EFS_FAULTS` build for the stalled
compactor). `tests/test_qualify_driver.py` (in `make test`, about 5 s) now
covers the gates stub-free: no `STUBS`/`g_stub`, the new gates' plan
command lines, their NOTRUN reasons without nodes or mounts, the status,
raft-status (retired replicas) and getchunks parsers, the wire probe against
fake daemons (a correct one passes every check and the follower redirect is
followed; a lax one fails the tag checks), the refusal probe against fake
binaries (strict passes, lax fails), the load, small-file, D2 mutation and E4
helpers on a plain directory verified with `_manifest verify`, `_node kill`,
and the matrix fixture defaults equal to the driver's.

## Findings

1. **`test_b6_unavailable.py` refuse mode fails on `merge-wave6`.** While
   node 3 refuses every fragment, files are packed into segments
   (`packed_during_fault` 3 on the branch; 13, 4 and 4 in three runs on the
   base `646a43041`); the test requires 0 because the packer needs all
   three destinations. Down mode PASSes. Not changed here (product code;
   B6/K37 owners).
2. **`gc-reclamation` fails on the base too:** `tests/live/gc_reclamation.py`
   asserts `fsync acknowledged after unclean stop: four-server crash`
   (`gc-base` run on `646a43041`, identical). Probably the harness's
   expectation against K36 (fragments a crashed run acknowledged stay
   unproven in their client's fsync); not triaged here.
3. **Fixed: `tests/test_get_integrity.py` did not compile on `merge-wave6`:**
   it stubs `read_reprobe_down` and also copies the production function, so
   the generated C defines it twice (identical on the base). The stub is
   removed; the production copy only calls the already-stubbed
   `efs_client_node_is_down`/`reprobe`.
4. A13's interrupted join hit "after its commit" in every loopback run (0.5 s
   and 0.1 s): on loopback a join commits within 0.1 s, so the pre-commit
   retry is exercised only by the K33 fault-hook tests in the code gate
   (`test_k33_interrupt.py`). A product build interrupts by timing only; on
   real hosts the delay may land before the commit, and the verdict says
   which.
5. D3 under load on loopback: queries take up to 24 s (p99 10 s) and EBUSY
   streaks up to 27 s, inside the 30 s deadlines but close; on real hosts
   watch the margin.

## Limitations and remaining gates

- Every new gate ran only on one host (loopback); A4 and RDMA NOTRUN. The
  parent runs the real-host topologies.
- A13 needs a driver-owned cluster (or an `a13` block); on `bare-metal-3-tcp`
  members 4 and 5 share hosts with members 1 and 2 (the verdict says so).
- C5 does not run authorization or a second uid; it covers the wire, tag,
  removed-setting, fence and revoke gates. The W127–W134 apply checks stay
  in the code gate.
- D2's "space becomes reusable" is shown by E4's reclaim (usable bytes fall)
  and `gc-reclamation`; D1 checks compaction and exact retained files.
- E4 at its defaults (5,000 small + 2 × 256 MiB per member) has not run; the
  loopback fixture was 2,000 + 2 × 32 MiB per member.

## Raw evidence and cleanup

- xorinox `~/efs-artifacts/stage2-gates-20261024/`: every run directory
  (with its own `SHA256SUMS`, each rechecked PASS with `qualify.py
  verify-sums`), the driver logs, the build and `make test-all` logs, the
  base B6 and gc runs. `SHA256SUMS` over 1,963 files, sha256
  `78ec26cca8913654ff3a1375803398de97aba24e7e3735683fcca7f70860d0a4`
  (46 MB, text only: no stores or payloads).
- The private tree `~/efs-stage2` (sources, builds, work and stores) was
  deleted with `edelete --force --delete`; the driver had already deleted
  each run's stores, mounts and scratch.
