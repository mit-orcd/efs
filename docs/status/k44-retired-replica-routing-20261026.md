# K44 / W167 — routing at a retired replica (October 26–27)

`agent/devel-task-k`; tested source `00e85a299`: TOTAL 508 FAIL 4 NOTRUN 14; base: TOTAL 508 FAIL 5 NOTRUN 14. `docs/status/k44-retired-replica-routing-20261026.md`. orcd-mini RDMA formation/addition and seed sweep PASS (Oct 27; user-approved format). Fixed: retired verdict/routing/resync, nonhost MKFS, notice NET, false CFG success, clean-stop signal race. Final measurement heads: `3846ecc1c`, `6780925c9`.

## Verdict and scope

Task K, [K44](alpha-1.0-2plus1-checklist.md) / [W167](work-queue.md).
**FIXED (private TCP/RDMA and orcd-mini RDMA scope), not merged or pushed.** The
original [ORCD-test BUSY](orcd-test-devel-rdma-394f2b791-20261027.md) reproduces
on unchanged base product and the final driver, and the candidate's named
routing sweeps succeed. The gate remains **FAIL**, with four shared host
LeakSanitizer instrumentation failures; leak verification is **NOTRUN**.
Cold-directory query cases remain **INCOMPLETE**, described below. The orcd-mini fresh formation, online addition and cross-host seed sweep
now **PASS** with the user’s explicit stop/format/deploy authorization
(Task K §6; October 27). Results and live-deployment limits follow below.

The earlier private executions use **node9900**, `/tmp/efs-taskK-20261026*`
trees and private endpoints. The subsequently authorized orcd-mini execution
uses node9901’s four product daemon endpoints and node9900’s client.
fcstor003–013 and Task H’s separate node9901 fixtures remain untouched.
No devops/cluster repository was edited.

## Confirmed code changes

The base seed forwards SALT successfully but then waits for its retired
local group-2 replica to apply the remote index. Its log shows retirement
followed by `apply-sleep ... group=2 idx=6 applied=4` for about 400 ms. That
cursor can remain frozen forever. Its old Raft object was also still eligible
for local proposal admission. This is not repaired by a longer timeout.

- Retired replicas cannot supply local application admission or verdicts.
  Unhosted/retired proposals use the live leader's actual submit verdict,
  including rejections. A local wait racing retirement returns NOT_PRIMARY,
  never success inferred from a stale cursor.
- Routing uses committed target voters instead of the retired object's old
  joint voter mask. A forwarded submit at a retired replica returns
  NOT_PRIMARY with a known live hint, preserving never-forward-again.
- `raft-status` keeps `retired=1` and historical local indices; its leader
  hint comes from successful live-group routing and may be unknown until
  observed. This diagnostic hint is not a read proof. Retired resync returns
  NOT_PRIMARY; the CLI says “leader or retired,” rather than falsely claiming
  every such refusal means leader.
- A member without group 0 proxies the **whole MKFS operation** to a group-0
  voter. Proxying just the first entry fails because the operation next
  reads group 0's committed membership locally. Group 0 retains its original
  three voters in alpha; the high-level proxy terminates there. Internal
  command forwarding still follows the one-hop rule.
- Formation notices retry idempotently within one absolute 5,000 ms budget,
  bounded further by any caller deadline. Restarted peers can already answer
  Raft/status while the sender's peer pool still returns BUSY during reconnect
  backoff. Explicit identity/encoding refusals are terminal; the peer pool may
  expose only BUSY for an underlying acquisition failure. No write deadline
  or quorum rule changes.
- `raft-change` checks convergence at its live responder, not the retired
  seed's historical configuration. It prints `converged=1` only when observed;
  exhausting its existing check returns BUSY instead of false success.

### Background work

Raft receive, persistence, apply of committed history and tick remain enabled.
Retirement can precede JOINT/COLD completion: old quorum {1,3} must still
participate while node 2 is down and new quorum {3,4} forms. Campaigns are
already disabled and retirement steps down a leader, so existing leader-only
GC/rebuild work skips that group. New snapshot exports are now blocked;
already captured historical export work can complete and release its pins.
Stopping all Raft activity on retirement would strand this valid transition.

There is no format, wire, compatibility, quorum, durability or product-mode
switch change.

## Sources and exact gate results

Base `4066f74df5b997d70f263ae8dc1063b1a6049b92`; final product/test source
`00e85a299` (product changes through `6850b3d90`, then reclaim fixture fix).
Build identity:

`efsd 00e85a299 (branch agent/devel-task-k, built 2026-10-27T05:08:35Z on node9900)`.

Measurement-only scripts copied **after** its gate: routing `3846ecc1c`,
stable queries `6780925c9`. This does not relabel the binary's source.

| Run | Retained result |
|---|---|
| Untouched base full gate, same host / 8-job command | `TOTAL 508 FAIL 5 NOTRUN 14 STATUS FAIL (764s) BUILD 11s`: four LSan fatal instrumentation failures and expiry-fixture client stop failure. |
| Final source `00e85a299` | `TOTAL 508 FAIL 4 NOTRUN 14 STATUS FAIL (706s) BUILD 11s`: **only the same four LSan instrumentation failures**. |
| Separate final `ASAN_OPTIONS=detect_leaks=0` checks | All four PASS: read-index pair, host outbox, ACK window, stage-evict LRU. ASan/UBSan scope only; never a leak PASS or replacement gate tally. |
| Final default gate integration | K33 growth 92.4 s; stop-progress 28.5 s; ownership 13.7 s; reclaim 10.9 s; both management extractors 0.1 s each: all rc=0. |
| Standalone final focused checks | Reclaim under long TMPDIR, ownership, stop-progress, both management extractors: all rc=0. Busy stop: 1,500 directories, 1,337 touched shards, stop 2.0 s. Small stop 0.5 s. Stalled/failed revoke remains refused with its reason. |
| Focused K33 at `7029a6266` and `6850b3d90` | Growth, seed-loss, interruption, payload, client-view, autoid and bookkeeping: seven of seven PASS on each. |

The four failed gate commands are `test_readindex_pair.py`,
`test_host_outbox.py`, `test_raft_ack_window.py`, `test_stage_evict_lru.py`.
Their default logs say `LeakSanitizer has encountered a fatal error`.
`strace` also cannot start (`PTRACE_TRACEME: Operation not permitted`); it
provides no syscall-trace evidence. Host security settings were not changed.
The 14 NOTRUNs include absent cluster lifecycle scripts / valgrind / sanitizer
runtime, a fault-only compactor variant, and native RDMA cases in this
**TCP-configured** full gate. They do not establish absent RDMA hardware:
separate product sweeps actually use `segmented-v1 on mlx5_0`.

## Management and client results

Final routing driver `3846ecc1c`, product `00e85a299`:

| Through seed unless specified | TCP | RDMA | One-loss joint transition, TCP |
|---|---|---|---|
| MKFS at retired seed | rc=0, 0.004 s | rc=0, 0.003 s | rc=0, 1.406 s |
| MKFS at added member 4 | rc=0, 0.003 s | rc=0, 0.004 s | rc=0, 0.004 s |
| Versioned commands-policy get/set; expiry-policy/clock get | PASS | PASS | PASS |
| Status/Raft-status, sync-status, data-commit, I/O/version/GC/expiry status | PASS | PASS | PASS |
| Pack-compact summary (no compaction requested) | PASS | PASS | PASS |
| Root/file getattr, lookup/path lookup, readdir, getchunks | PASS | PASS | PASS |
| Same-voter-set CFG | observed, 0.006 s | observed, 0.025 s | observed, 0.007 s |
| Retired step-down / resync | NOT_PRIMARY, required | same | same |
| Seed-only FUSE operations and exact payloads | PASS | PASS | PASS |

Each final sweep has 26 recorded CLI calls, exits 0 and checks application
status fields independently of process exit. All these final raw reads
succeeded locally; no extra hint-follow hop was needed in them. Earlier
`6850b3d90` healthy sweeps returned valid raw-read NOT_PRIMARY hints when
group 0 led elsewhere. Those harness failures remain FAIL in the evidence.
The revised measurement permits only that documented read verdict and
requires the named member to serve the same read on one hop; other errors
remain failures. FUSE follows such redirects itself.

The one-loss fixture stops node 2 before adding node 4, proves new group-2
voters {2,3,4} using live quorum {3,4}, then restarts node 2 before its sweep.
Its notice needed 29 attempts, ending rc=0 within the original notice budget.
Every final base control still fails specifically at retired-seed MKFS with
`rc=-13`, not an unrelated NET failure.

Seed-only FUSE checks include create, fsync/fdatasync, reads, truncate,
xattrs, flock/byte locks, hardlinks/symlinks, statvfs, rename/unlink and
mkdir/rmdir. Payloads are exactly 1,048,832 bytes, then their expected first
4,099 bytes after truncate; link/rename reads compare the same bytes.

The supplemental surface (`diagnostic-2fee7e4fb/surface`, product
`7029a6266`, driver identical to `670c81b8c`) additionally exercises MKFS
alias, reducing private quota by 1 MiB, raw create/mkdir/setattr, even-authority
session create/register/establish/get/fence, holds, flock/fcntl, append,
link/rename/unlink/rmdir and idempotent directory finish. Disabled expiry
operations are explicitly refused; an unidentified REPORT is UNREGISTERED.
A stricter retained audit confirms both directory-finish replies were
process rc=0, status=0, verdict=0; no PASS relies on the early oracle's
unnecessary generic-I/O allowance (removed at `d27b2b13d`). Unchanged POSIX
core: **218/218 PASS**, TCP 113.5 s and RDMA 86.8 s, with no skips.

### Queries: cold freshness and steady-state routing are separate

`query-acba89b37` fails in setup: mask 7 is unsupported; no query cases ran.
`a17f6cd71` uses the observed supported default mask 3. Its unchanged POSIX
extras produce **1/3 pass, 2 skip** on TCP (24.1 s), and **2/3 pass, 1 skip**
on RDMA (12.6 s). An unchanged-base TCP comparison also produces
**1/3 pass, 2 skip** in 24.1 s. These remain
**INCOMPLETE**, never a query PASS. The default refresh interval is 60,000 ms
(`HOST_QUERY_T_MS`); these cases allow about eight seconds for a newly
created directory. Logs show the initial empty catalog and no next refresh
before the cases finish. Nothing changes those deadlines or the product policy.

The separate stable-fixture driver `6780925c9` uses existing
`tests/live/query_timing.py`: discover the advertised interval, require every
serving catalog to index the quiescent fixture (and every redirect to resolve
to a serving catalog), then execute stats/ls/find once under normal command
bounds. It requires two exact final payloads (4,096 + 1,024 bytes), exact
names/sizes and tree totals. **PASS on TCP and RDMA**: three commands and two exact payloads on each.
All serving catalogs (members 2 and 3) were ready after 59.25 s / 60.25 s,
within the advertised 60 s interval / existing 180 s readiness bound;
members 1 and 4 redirected to a catalog proved ready in that same round.
Each actual command is checked once, with its original internal budget and
a 30 s subprocess ceiling. No cold-case result is relabelled.

This covers the named management verbs and ordinary client operations,
not every argument combination, low-level subtype or failure interleaving.
Removed add-node/add-storage aliases have no server request to route.

## Additional gate failures reproduced and fixed

**Clean-stop race, `6850b3d90`.** The deterministic regression from
`d5fe5dca4` fails against the unchanged base helper: endpoint ownership was
valid when checked, then libfuse removed its signal handler in unmounted
teardown, then the helper's SIGTERM killed it before a final record. That is
a proven mechanism for the missing-record signature; the original failure
captures do not themselves identify the terminating signal. Clean retirement
now awaits the drained/unmounted client, preserving ownership/replacement
proof, progress/stall and absolute bounds, final-record checks, and explicit
failed-stop diagnostics. It never races flush/settle/revoke with SIGTERM.

**Reclaim fixture, `00e85a299`.** A test-only diagnostic against unchanged
base product reports item rc=-13, batch rc=-1 at inode 20000. The retained
`cleanup-proof.json` proves a 296-byte cleanup command was truncated to 255
bytes: journal directory removed, `.efs-reclaim-state` left behind. Source
inspection explains the next segment-open I/O failure. This is test setup,
not a reclaim/PUT race. Buffers now fit their bounded paths, with an explicit
state-removal assertion; the racing workload and reclaim assertions stay.

The two management C extractors now select definitions instead of matching
the added `sess_leader_address` prototype and compiling unrelated commands.
Retired-mask / hold / read-cover fixtures test real helpers with strong
behavioral assertions. The old resync diagnostic and driver inventory are fixed.

## Earlier attempts remain retained

| Attempt | Result / disposition |
|---|---|
| `a58c679b9` | Original focused TCP/RDMA assertions PASS; provisional source, superseded. |
| `d643adcd9` | Expanded sweeps exit 0; gate `TOTAL 508 FAIL 10 NOTRUN 14 STATUS FAIL (614s) BUILD 0s`. Harness/mock/stop/instrumentation defects retained. |
| `7e4ddcb5e` | One-loss TCP/RDMA sweeps FAIL NET after restart; seven focused K33 PASS. Gate `TOTAL 508 FAIL 10 NOTRUN 14 STATUS FAIL (574s) OVERBUDGET 1 BUILD 0s`. Its one-loss base control fails NET too and is not the original BUSY negative control. |
| `7029a6266` | Three core sweeps and seven focused K33 PASS. Gate `TOTAL 508 FAIL 8 NOTRUN 14 STATUS FAIL (749s) BUILD 0s`: four LSan, two management extractors, stop-progress, reclaim fixture. |
| `6850b3d90` | Seven focused K33 PASS; gate `TOTAL 508 FAIL 5 NOTRUN 14 STATUS FAIL (761s) BUILD 0s`: four LSan plus reclaim fixture. Raw-read redirect assumption fails healthy TCP/RDMA sweeps; joint PASS. |
| `surface-00d426006` | FAIL: expiry/refusal oracle and nonexistent POSIX root; no POSIX cases ran. Corrected supplemental run is reported above. |
| Early shutdown diagnostic `2fee7e4fb` | Syntax error in the new diagnostic; supplies no base-race proof. Corrected `d5fe5dca4` supplies that proof. |
| Waiting dispatches `ae0262306`, `afc6d1466`, `6cd513dc7`, `fcc8533d8`, first `00e85a299` controller | Canceled before build/test; reason receipts retained. No interrupted test is called PASS. |

No aggregate success is inferred from a controller advancing; exit receipts,
application verdicts, comparisons and verification limits determine each row.

## Reproduction and retention

Environment: RHEL 9.8 node9900, 192 visible CPUs, `/tmp` on XFS `/dev/md0`,
private libfuse `/data1/efs/deps/libfuse`, eight build/test jobs. Four private
1 GiB stores, `--writers 2 --discovery-port 0`, auto-formation at three then
one online addition. O_DIRECT fragment I/O only. TCP loopback; RDMA
`mlx5_0`, `172.16.99.0/16`, actual RDMA setup logged. No production endpoint.

1. Export `00e85a299` into a fresh private candidate tree. Set
   `EFS_GIT_ID=00e85a299 EFS_GIT_DESCRIBE=00e85a299
   EFS_GIT_BRANCH=agent/devel-task-k`, private libfuse pkg-config/library
   paths, and `FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,/data1/efs/deps/libfuse/lib64"`.
   Build `make -j8 all tests/faults/efsd tests/test_reclaim` with those flags.
2. Capture `efsd --version` and binary SHA256s. In detached screen run
   `EFS_TRANSPORT=tcp EFS_BUILD_JOBS=8 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8
   bash scripts/make_test_summary.sh`. Preserve summary, timings, progress,
   build/enumeration logs and all `tmp/fail-*.log` before any later run.
3. Copy only `scripts/task_k_routing.py` from `3846ecc1c` into that built
   tree. Run with fresh `--out` directories, `--port-min 19601`, first TCP
   loopback, then RDMA address/device above, then TCP
   `--one-loss-transition`. `scripts/task_k_qualify.sh` records the earlier
   combined build/focused/gate method; dispatch scripts in the raw directory
   record the exact serial attempts and final test-only revision.
4. Export/build unchanged base `4066f74df` separately with its own build id.
   Inject that measurement script only and require the original `rc=-13`
   assertion; move it out before base gate enumeration. Do not mix binaries
   or formats within a fixture. The reclaim negative uses the same original
   test under a long TMPDIR; only the diagnostic rc print is added in its
   recorded diagnostic run. The teardown negative uses the committed new
   test against the unchanged base helper, with only its root redirected.
5. Copy `scripts/task_k_query_warm.py` from `6780925c9` after the gate, run
   sequential TCP/RDMA with `--product-source 00e85a299 --port-min 19801`
   and new `--out` paths. Cold extras have a separate reproduction in
   `scripts/task_k_query_surface.py` at `a17f6cd71`; never substitute warm
   results for those unchanged cold-case skips.

Raw evidence is on node9900:
`/home/erbmi1/efs-artifacts/taskK-20261026/`. No raw capture/store is committed
or copied to the Mac. The final `SHA256SUMS` contains 2,655 closed retained files and was verified
with `sha256sum -c`; its own SHA256 is:

`e8c821bec6f7bd1f0736b5b5c5f10d486a2b84426ff52a0f954150f7ab961fd5`.

`retention-check.json` records original/copy full hashes and ownership/activity
checks. `PRUNED.txt` and `prune.log` record 12 scratch roots removed with
`edelete`: classified fixture stores, generated payloads and source/build
trees, including the diagnostic TMPDIRs. Copied runtime
`mdraft/log/raft.log` WAL files were also pruned rather than mistaken for
text logs. Exact failed-store replay is forfeited; the committed sources and
method reproduce each classified failure. Closed text logs, results, driver
hashes, binary hashes and all failure/verdict receipts remain.

`released-host.json` confirms no Task K directory/process/mount remains and
the existing orcd-mini mount is still listed. Cleanup's initial canonical-path
guard refusal occurred before copying/deleting and is retained; v2 checks the
canonical authorized raw root. The final manifest verification log is
`/home/erbmi1/efs-artifacts/taskK-manifest-final-check.log`.

The private-stage release above preceded the separately authorized
orcd-mini run below. `~/git/cluster/orcd-test.py init-export` should propagate
a failed format instead of silently assuming initialization; its owner
should review that separately. This task does not edit that repository.

## Authorized orcd-mini qualification — October 27

**PASS for fresh formation, online addition and the retired-seed command /
POSIX core sweep.** The user explicitly authorized stopping, reformatting
and deploying Task K, preserving evidence and leaving the cluster running.
The separate Task H private cluster on node9901 was not signalled or changed.

Product source is exactly `00e85a299531e873daefa0a40b5f781c5ec7225c`.
The source archive SHA256 is
`1fa031a84b97d8abeeef7a36e999f5cf1a0e309126a45cb45675a374d9e9f064`.
Both hosts built release `00e85a299531` independently with eight build jobs,
private libfuse under `/data1/efs/deps/libfuse` and its runtime rpath.
Build times are 18:46:27Z on node9900 and 18:46:28Z on node9901.
Measurement drivers are separate from that product source: `e56f63f82`,
`1d3638e7a`, `571594e6c`, `5cbe53168`, `e79dbb48a`. No product code changed for this run.

Environment: orcd-mini, four **processes on node9901**, endpoints
`10.1.99.1:17432–17435`, stores `/data1/efs/n1..n4`, each with `--quota 720G`;
node9900 mounts only seed `10.1.99.1:17432` at
`/data1/efs/orcd-mini-mnt`; node9901 mounts it at `/data1/efs/mnt`.
Both hosts run RHEL and native InfiniBand `mlx5_0`,
`EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0`, product O_DIRECT, ordinary
background packing and commands-policy mask 3. Actual client and daemon
logs record `RDMA transport up segmented-v1 on mlx5_0`.
This is a real cross-host client test, **not four independent storage hosts
or a physical failure-domain/capacity qualification**.

### Formation, results and full-payload scope

The remote client stopped first, then the local client, then the three
configured daemons. All three old daemon exit records report `orderly`,
`rc=0`, with PUT-sync, reclaim-journal and usage-flush barriers done.
Closed old logs were archived and verified before formatting only n1..n3.
The old product was `394f2b791`; no old store or payload migration was attempted.
The existing devops script started three fresh Task K members; formation
at 18:48:07Z committed both voter masks `0x7`, and MKFS succeeded.
Member 4 then joined online at port 17435. Seed group 2 retired at
18:48:32Z; live group 2 converged to `voters=0xe joint=0`.

| Check | Result |
|---|---|
| Seed and added-member MKFS | Both rc=0; application result checked, not just process exit. |
| Management sweep | 27 calls, including added-member live-status observation; get/set commands-policy, expiry read-only policy/clock, status/sync/data-commit/I/O/version/GC/expiry, pack summary, raw namespace/chunk reads, same-voter-set CFG: PASS. |
| Retired step-down and resync | Required NOT_PRIMARY (`rc=-15`), no resync scheduled. |
| Seed-only FUSE surface | Create, write, fsync/fdatasync, exact read, xattrs, chmod, flock/fcntl, truncate, links, rename, readdir, unlink and mkdir/rmdir: PASS. |
| Unchanged POSIX core, remote client, one job | **218/218 PASS**, zero fail/skip/NOTRUN, 137.3 s. |
| Unchanged two-client POSIX, node9901 ↔ node9900 | **68/68 PASS**, zero fail, 282.9 s. The 500 disjoint pwrite+fsync operations per client complete in the 53.36 s concurrent step; this is correctness evidence, not a throughput acceptance gate. |
| Exact retained payloads through both mounts | Every byte of `large.bin` (1,048,832 B) and `tail.bin` (4,099 B), **1,052,931 B total**, matches the deterministic final expected contents. |
| Stable `.efs` stats / ls / find | PASS after every serving catalog was ready: nodes 2 and 3, 60 s advertised refresh, 32.181 s readiness using the existing 180 s precondition bound; normal per-command budget unchanged. Exact two-file names/sizes, 5,120 B tree total and both full payloads checked. |

Full reads above cover the named fixtures, not every file ever created in
POSIX/two-client tests and not the old, explicitly reformatted stores.
The earlier private cold-query skips remain **INCOMPLETE**; these warm
results do not replace them. The previous default full gate remains
508 commands / 4 shared LSan FAIL / 14 NOTRUN; no new full-gate PASS is claimed.

### Reproduction and operational ownership

Prerequisites: authenticated SSH master to ORCD, both hosts' libfuse/RDMA
runtime, idle configured orcd-mini mounts, and **explicit authorization for
this cluster's fresh format**. The following procedure is destructive to
only its named stores and must not be run as an unattended regression:

1. Export `git archive 00e85a299531` to each host's
   `/tmp/efs-taskK-mini-20261027/source`; run
   `scripts/task_k_mini_build.sh` detached on each host. Inspect both
   `build-exit` records and retain binary hashes.
2. On node9900, use the new source's `scripts/client.sh stop` for
   `/data1/efs/orcd-mini-mnt`, verify mount absence, archive the old client
   logs, move the built source to
   `/data1/efs/orcd-mini-client/releases/00e85a299531` and switch its `src`
   symlink. This uses the clean-stop helper fix, without forced discard.
3. On node9901, run `scripts/task_k_mini_deploy.sh` detached. It records old
   identities/status/config, stops the configured local client and daemons,
   closes/verifies the log archive, checks no process owns the product
   stores, uses `edelete` for n1..n3, switches the built release and invokes
   `EFS_CLUSTER=orcd-mini EFS_START_TIMEOUT=900 ./start.sh` from the existing
   `/data1/efs/devops`. Inspect `deployment-exit` and formed-three status.
4. Start member 4 with `scripts/task_k_mini_fourth.sh start` on node9901;
   record its owned PID. Start node9900's existing client wrapper. Observe
   seed retirement and live mask `0xe` before proceeding.
5. On node9900, with the RDMA environment above:

   ```sh
   python3 mini-sweep.py --source /data1/efs/orcd-mini-client/src \
     --mount /data1/efs/orcd-mini-mnt --out NEW_EVIDENCE/rdma-sweep
   python3 mini-queries.py --source /data1/efs/orcd-mini-client/src \
     --mount /data1/efs/orcd-mini-mnt --out NEW_EVIDENCE/stable-queries
   ```

   These are the committed `task_k_mini_sweep.py` and
   `task_k_mini_queries.py` copied outside the product tree. Output paths
   must not exist. The query fixture is separate and remains quiescent.
6. From the driver, run `scripts/task_k_mini_two_clients.py NEW_RESULT.tsv`;
   it uses `task_k_mini_ssh.sh` without BatchMode, the unchanged POSIX
   two-client cases and the existing orcd-mini 120 s remote-step bound.
7. Run the same sweep on node9901 with `--source /data1/efs/src
   --mount /data1/efs/mnt --verify-only --out NEW_EVIDENCE/peer-full-read`.
   Then capture closed snapshots with `scripts/task_k_mini_capture.py` on
   each host, hash/verify the retained files and leave the services running.

The devops repository was **not edited**. Its checked-in orcd-mini arrays
still name three members. Until its owner extends PORTS/NODE_ADDRS/STORDIRS
with member 4, do not treat its three-member start/stop wrapper as a complete
four-member lifecycle. The retained `fourth-member.sh` is the committed
ownership-checked helper: stop both clients cleanly first, then stop member 4
with it, then stop the original three using devops. A restart must bring
back all four stores with the same membership; never silently revert to a
three-member cluster or reformat on a startup refusal.

### Closed evidence and final running state

Raw evidence on **each** of node9900 and node9901 is retained at:
`/data1/efs/evidence/taskK-orcd-mini-20261027/`.
The source/build/result manifest is `run-manifest.json`; exact binary hashes,
closed test logs/TSVs, old log archives, deployment/stop records, copied
measurement drivers and final health/process/mount snapshots are included.
Archive listing and every SHA256 entry were verified successfully.

| Host | Closed files in `SHA256SUMS` | SHA256 of that manifest |
|---|---:|---|
| node9900 | 49 | `1eda435d14edfca41dc0cbdcbc33455503d141e82b6a5c8f9c2493f63d9568e0` |
| node9901 | 58 | `bd567cf21d5727db2db672594bedf3d35963068436cd508fc4ec1e6689972153` |

Verification receipts are outside each closed tree at
`/data1/efs/evidence/taskK-orcd-mini-20261027-verify.log`.
`PRUNED.txt` and prune logs state that the explicitly reformatted old stores
are gone, the completed two-client synthetic namespace was removed with
`edelete`, and finished `/tmp/efs-taskK-mini-20261027` staging was removed.
The built sources live in their versioned infrastructure releases; they
were not deleted. No copied stores or generated payload bulk is retained
inside the evidence trees. The two small exact-check namespaces remain on
the live cluster for inspection.

At **18:58:19–21Z**, both final snapshots report:

- `Cluster state: OK (4 up, 0 down)`; `00e85a299531`, 4/4 identical.
- Zero degraded chunks and zero degraded/unrecoverable segments.
- Group 0 on voters 1–3; group 2 on voters 2–4; seed group 2 remains
  explicitly retired with its historical local cursor.
- Both configured product mounts serving their Task K client binaries.
- 312 abandoned uploads pending, zero collection failures. This run does
  not claim that orphan grace periods/collection have fully drained or
  qualify reclaim throughput.

All four product daemons and both product mounts are **left running**.
Other agents’ private Task H / Task I mounts visible in the host mount tables
are not part of this deployment and were left alone. No product process is
running from the deleted staging tree. K44’s real-host gate is satisfied in
this scope; the parent still owes merged-tree qualification. The shared
host LSan failures and historical cold-query INCOMPLETE verdicts remain
explicit rather than being relabelled by this result.
