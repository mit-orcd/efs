# Task V — four distinct host TCP driver rehearsal

`agent/devel-task-v`, code `b9dfff609` / live `578e2064b`:
**TOTAL 522 FAIL 1 NOTRUN 12 STATUS FAIL (744s)**. Live: 8/9 requested gates
PASS, K52 FAIL. Product C/header equals base `b2d57e421`; no paired live A/B.
Evidence: `docs/status/a4-lab-topology-dryrun-20261028.md`.
RDMA/full A4 NOTRUN. K52 read-back mismatch OPEN; K48-shaped listing omission
OPEN. No product fix or passing release gate claimed.

The user selected a private additional cluster on fcstor012–015 instead of
reserving the four lab hosts and required destruction afterward. All four
private trees are now removed with verified cleanup receipts; existing clusters
were untouched. Topology defined and dry-run, A4 decision open; this is not
an A4 release PASS.

## Scope and source changes

`fcstor-4x1-tcp` describes one member per host on wired addresses
10.1.223.66–69, port 19340, two mounts (fcstor012/013), TCP, O_DIRECT,
packing and default commands policy enabled, expiry off, 4 GiB/member.
Stores/mounts are under `/tmp/efs-taskV-20261028/cluster`; source and build
scratch are siblings. Fixture sizes match `loopback-5`, including 2,000
small files and two 32 MiB large files per member. No user cluster is used.

Driver fixes committed: two-client remote execution receives each host's
actual script path; teardown requires clean client stops and daemon stops
before removing stores and propagates cleanup failure into the run verdict.
The Task V SSH wrapper restricts destinations to the four authorized hosts
and does not use BatchMode. Candidate placeholders resolve only to the exact
explicit `--candidate` build.

## Prerequisites and initial inventory

All four machines are reachable through the ORCD login, have 24 CPUs,
RHEL 9.8 kernels, `/dev/fuse`, and about 404 GiB free on local `/tmp`.
No efsd process or listener in the proposed range was found. Wired interface
`ens10f0np0` has 10.1.223.66–69; InfiniBand 172.16.223.66–69 is excluded
from this TCP rehearsal. System libfuse 3.10.2 is too old; an existing private
libfuse 3.16.2 is available at
`/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse` (read-only dependency).
No packages are installed. Exact build identities, gate walls, raw paths,
hashes, failures and cleanup receipts will be recorded after execution.

## Gates

`identity`, `topology`, `b-checks`, `posix-core`, `posix-2client`,
`c5-protocol-live`, `e1-manifest` (orderly restart of this owned cluster),
`d3-efs-load`, and `e4-scale`. No timeout is relaxed and every failure stays
reported. RDMA, crash/power-loss and full A4 release acceptance are NOTRUN.

## Reproduction (current branch)

Prerequisites: SSH access through `orcd-login` with agent forwarding, free
private ports 19340/19400+, local `/tmp` headroom, Python 3.9+, screen,
compiler dependencies and libfuse >=3.12 on every host. The wrapper accepts
only fcstor012–015, forwards stdin for manifests and never supplies credentials.
Stop if authentication requests a password or token. The four dated scratch
paths must be absent before fresh source copies; never replace another run.

1. Freeze `C=$(git rev-parse HEAD)` on this branch and export `git archive "$C"`
   into `/tmp/efs-taskV-20261028/src` on each host using
   `scripts/task_v_ssh.sh HOST 'mkdir ...; tar -xf - -C ...'`. Build separately
   on every host (native CPU flags) with identical `EFS_GIT_ID=$C`,
   `EFS_GIT_DESCRIBE=$C`, `EFS_GIT_BRANCH=agent/devel-task-v`.
   Set `PKG_CONFIG_PATH` to the compatible private libfuse prefix and link
   with that prefix's `-Wl,-rpath,.../lib`. Use a detached screen for builds.
2. Put build logs/identities and a full source SHA-256 manifest outside scratch,
   under `/home/erbmi1/orcd/scratch/efs/taskV-20261028/HOST/`. Check every
   source file on every host. Create each host's `/tmp/efs-taskV-20261028/tmp`.
3. Run the following from the frozen source on fcstor012, in a detached screen
   with its forwarded SSH agent kept available (the same command also works
   from a controller with the wrapper and matching source paths). The full
   matrix plan additionally retains `soak` as E4's endurance companion;
   explicitly select these nine gates for Task V, not that full plan:

```sh
export TMPDIR=/tmp/efs-taskV-20261028/tmp
export PATH="$HOME/bin:$PATH"
python3 scripts/qualify.py run \
  --matrix scripts/qualify_matrix.json --topology fcstor-4x1-tcp \
  --candidate "$C" \
  --gates identity,topology,b-checks,posix-core,posix-2client,c5-protocol-live,e1-manifest,d3-efs-load,e4-scale \
  --out "$HOME/orcd/scratch/efs/taskV-20261028/NEW-RUN" \
  --work /tmp/efs-taskV-20261028/controller-work
```

The first run used `578e2064b`, whose default topology already selected exactly
these nine gates. The current full plan adds the endurance companion rather
than weakening `test_soak_driver.py`'s coverage invariant. The first intel2
fast-suite result at `4622c4ed8` failed that invariant; its original `fast.log`
and exit 2 remain retained. No product failure is attributed to that plan error.
The final code gate tests `b9dfff609`, including cleanup-failure and richer
immediate-mismatch diagnostics regressions.

4. After the controller exits, verify `qualify.py verify-sums RUN`, inspect
   `_teardown/result.json`, verify no private process/mount remains, and
   remove only `/tmp/efs-taskV-20261028` on each host with
   `~/bin/edelete --force --delete`. Keep finalized results, original logs,
   source/binary hashes and a separate cleanup receipt. Do not delete shared
   libfuse or any pre-existing `/tmp/efs*` or `/data1/efs` tree.
5. On idle intel2, use a separate `~/efs-taskV/src` and run the frozen code
   gate in screen with `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8`
   through `scripts/make_test_summary.sh`. Preserve original failures,
   summary, timings, commands and NOTRUN reasons before removing that tree.

## Final live gate results

| Gate | Verdict | Wall seconds | Checks |
| --- | --- | ---: | --- |
| identity | PASS | 1.969 | four daemons, two clients, one candidate |
| topology | PASS | 0.664 | four members up on four distinct hosts |
| b-checks | PASS | 70.807 | placement/capacity/decode probes |
| posix-core | PASS | 29.669 | 218/218, four jobs; topology's original 150 s case budget |
| posix-2client | PASS | 133.730 | 68/68; suite body 128.3 s |
| e1-manifest | PASS | 53.378 | all 240 acknowledged files, 140,120,143 bytes exact before/after orderly restart |
| c5-protocol-live | PASS | 5.924 | wire 44/44, settings refusals 12/12 per host; owned fence/revoke checks |
| d3-efs-load | **FAIL** | 790.069 | 8 immediate read-back comparisons failed; 8,659 queries, no query errors/deadline violations |
| e4-scale | PASS | 239.246 | all 8,008 payloads exact on cold reader; 4,008 deleted/reclaimed, all 4,000 remaining exact |
| end identity | PASS | 1.921 | original candidate still in every daemon/client |
| teardown | PASS | 10.554 | clean client/server stops, original logs fetched before store deletion |

All-pairs connectivity succeeded (16/16 including self-connects), with source
addresses on 10.1.223.66–69. Every host reports `NTPSynchronized=yes`; this
records synchronization, not a measured zero clock offset. No firewall/package
change was needed for the tested TCP paths. System libfuse was the only build
prerequisite gap encountered and was satisfied by the existing private prefix.
D3 queries use the first mount; remote-client behavior is covered separately
by the two-client suite and E4 cold-reader gate. The dedicated full-soak,
unclean-crash, power-loss, RDMA and remaining A4 matrix cells are not run here.

Product `src/` and `include/` have no differences from base `b2d57e421` in
this branch. There is no paired unchanged-base four-host run, so no controlled
latency A/B or causal claim about the driver changes. K52 counts against live
acceptance; it is not waived as a base-only failure.

## K52 — immediate read-after-write mismatches (OPEN)

D3 wrote fresh exclusive-create files with eight writers for 300 seconds and
four concurrent query workers. `write_exact` completed write, fsync and close;
eight subsequent exact `read_bytes() != expected` comparisons failed. The
helper raised synthetic EIO for that comparison; this is not evidence of a
kernel EIO return. Operations were `f00170`, `f00628`, `f00679`, `f00781`,
`f01174`, `f01282`, `f01528`, `f02582` (timestamps retained in result JSON).
No I/O deadline exceeded 10 s: measured maximum 0.317 s, p99 0.1867 s.

The original helper did not capture the writer id, returned length or returned
hash at failure. That limits the original finding to a reported exact-comparison
failure; it does not establish byte pattern, short-read versus wrong-content,
persistent corruption, or packing involvement. Before pruning, each of two
clients read all 64 possible writer/name candidates against the single final
expected payload: **64/64 exact on each client**. These later reads do not
qualify the earlier operations. The never-renamed `w5/f00679` and `w5/f01174`
identify two failing paths because successful operations at those indices
rename to `.r`. Their later mappings were a packed record and an ordinary
span respectively; neither proves the mapping at the original failure.
No attribution to W148's old modify bug or to packing is made.

The other 37,457 records passed D3's later exact verification on the first
mount. The final catalog counted 37,465 files, eight more than the helper's
successful-operation list. That difference is explained by the eight
write-complete/read-failed files omitted from that list; it is **not a separate
proven catalog-convergence defect**. D3 queries answered across six generations:
8,659 calls, zero query errors, two bounded BUSY episodes (maximum 17.31 s),
query p99 0.3146 s and maximum 16.971 s, within the original 30 s limit.

New helper diagnostics capture full path, worker id, expected/observed lengths,
hashes and first mismatch offset at the immediate failed comparison. They
retain failure, never retry, and never accept any version. The unit negative
control confirms that a later exact read cannot rewrite the saved mismatch.
No product fix or root cause is claimed. A focused reproduction with these
richer observations is required before K52 can close; it blocks alpha.

E4 independently passed: 8,000 small files plus eight 32 MiB files,
306,323,172 actual bytes, all exact on the other host under 404 `.efs` queries.
After 4,008 deletes (287,482,988 bytes), usable usage fell by 273,547,264 bytes,
passing the unchanged 80% reclaim criterion; all 4,000 retained payloads matched.
This does not erase D3. No gate was rerun to manufacture a PASS.

## Closed evidence and cleanup

Stable raw root on ORCD (shared filesystem):
`/home/erbmi1/orcd/scratch/efs/taskV-20261028/`
(canonical `/orcd/scratch/orcd/001/erbmi1/efs/taskV-20261028/`).
The finalized top-level `SHA256SUMS` covers **313 files**, SHA-256
`16cb6e720c06b8b2e8972ee1a8726f3b988563c0530d01a732a3bfd205f351c5`.
Inner `driver-run1/SHA256SUMS` hash:
`eca60b565c1822776cdfc88d808c722d6e8f6ad161b486936723ab0ba4a4c85a`.
Exact 2,267-file source manifest hash:
`507c7b44ba77f443bca16093ae99017b431f36376f040201633c768c420d47f2`.
Every host verified it before pruning; per-host binary hashes are retained.
All normal binaries report `578e2064b`, branch `agent/devel-task-v`, built
2026-10-28 UTC at 15:50:17/18/20/19 on fcstor012/013/014/015 respectively.
Each host independently built/tested the frozen source; no binary was shared.

Per-host `cleanup-receipt.json`, `PRUNED.txt`, `cleanup.log`, and
`driver-sums-before-prune.log` record completed approved removal of only
`/tmp/efs-taskV-20261028` after original manifests, process and mount checks.
Independent post-cleanup checks found no tree, Task V mount or port-19340
listener on any host. Stores, payloads and builds were removed as the user
requested; store replay is consequently forfeited. Original build/daemon/client
logs, exact results, candidate read/mapping captures and hashes remain.
Nothing raw is committed to Git. The shared libfuse and existing clusters
were not removed, stopped or redeployed.

Intel2 results and initial fast-suite failure remain separately under
`/home/maik/efs-artifacts/taskV-20261028/`; final code-gate details and completed
scratch cleanup are below.

## Code-gate listing failure (K48-shaped, not waived)

The `b9dfff609` intel2 full gate also failed unchanged
`tests/test_w130_dir_fault.py`: its serve-first W165 subcase passed, the
96-file target listed correctly after the deliberate halt, and the later
**post-resync** listing returned 95 names, missing `f0057`. This is the
existing K48 omission shape at a later phase, not a demonstrated identical
root cause and not a new payload-loss claim. Product source and this test
are unchanged from base; no paired base run is used to waive the failure.

The test closed its mounts/daemons and retained its work directory. Before
build-tree pruning, the closed 54 MiB fixture was moved on-host to
`intel2:/home/maik/efs-artifacts/taskV-20261028/open-k48-w130/` for the open
investigation. All **716 files** were hashed before and after the move;
`open-k48-SHA256SUMS` SHA-256 is
`1c1b24d0428cd12e4006da1f0da16644a9f3ee66efb3e54779fc7d4573b77977`.
`open-k48-preservation.json` records the original path and checked process/mount
absence. This is the explicit bulk-retention exception for OPEN K48;
no cluster is running from it. Analyze copies, not these finalized originals.
The full gate completed with this one failure, which remains counted. Task V's
build/test scratch was removed after retaining this closed investigation fixture.

## Final full code gate and intel2 cleanup

Frozen code candidate `b9dfff609`, intel2 (eight CPUs), Linux
`6.12.0-211.67.1.el10_2.x86_64`, GCC 14.3.1, Python 3.12.15, FUSE 3.16.2,
XFS home filesystem with about 838 GiB free. Binary identity:
`efsd b9dfff609 (branch agent/devel-task-v, built 2026-10-28T16:17:22Z on intel2)`.
The final source includes the remote-path/prepare/cleanup fixes, the matrix
endurance companion correction and immediate mismatch diagnostics. The original
live run remains pinned to `578e2064b`; it was not rerun with different verdicts.

```text
TOTAL 522 FAIL 1 NOTRUN 12 STATUS FAIL (744s) OVERBUDGET 5 BUILD 29s
```

The only failing command is `tests/test_w130_dir_fault.py` (index 483), described
above. Driver bookkeeping, remote-script-path, cleanup refusal, mismatch-evidence
and plan-invariant regressions pass in the complete run. No budget or deadline
was increased. Overruns, which remain reported rather than waived:

| Command | Seconds / budget |
| --- | ---: |
| `test_k38_shared_chunk_pressure.py` | 42.3 / 35 |
| `test_stop_progress.py` | 61.2 / 50 |
| `test_k43_coordinator_kill.py` | 53.7 / 25 |
| `test_query_delta` | 11.6 / 10 |
| `test_k22_leader_admission.py` | 40.1 / 30 |

The 12 NOTRUN commands are: missing external cluster stop/remount scripts (1),
missing Memcheck headers (1), unusable RDMA device paths (9: declaration, xprt,
seven segmented modes), and the normal-build stalled-compactor subcase requiring
a fault build (1). No package installation or real RDMA qualification is implied.

Final inner manifest `full-gate-b9dfff609/SHA256SUMS` hash:
`9d44ead3cd7df08012928b0a534808fcc80a6e8a47ffb55cb46d1ed136527e7c`.
After source/evidence checks and no live Task V process/mount, only
`/home/maik/efs-taskV` was removed with `edelete`. `cleanup-receipt.json`,
`pre-prune-check.json`, `scratch-cleanup.log` and `PRUNED.txt` record it.
All 2,267 frozen source files matched. The finalized intel2 top manifest covers
**746 files**, including the explicit OPEN K48 fixture exception, hash:
`b2065d0455d753059160496c585050ff3de04bb4953b182845413ccf53c4f301`.
No Task V cluster or build tree remains running. The closed 54 MiB K48 fixture
is evidence, not another deployment.

## Disposition

Task V's preparation is complete: the private topology validates, all nine
requested gates ran on four real hosts with recorded walls, remote driver defects
are corrected and private cluster/build scratch is cleaned. **The dry run and
full code gate both FAIL**, with K52 new and K48-shaped evidence retained.
A4 says "topology defined and dry-run, decision open"; no release checkbox closes.
The next correctness work is a focused K52 reproduction with the new observations,
and Task S's K48 investigation using copies of the new closed fixture. Neither
failure is relabelled PASS, NOTRUN, harmless or fixed by later matching reads.
