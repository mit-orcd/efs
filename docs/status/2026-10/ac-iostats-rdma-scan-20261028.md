# AC — routine I/O logging and the RDMA registry

`agent/devel-task-ac` tested head `5f9ddae71`, base `1d2852da4`: TOTAL 546 FAIL 0 NOTRUN 12 (intel1); native RDMA PASS. Base scans36→0; speedup unproven. Evidence: docs/status/2026-10/ac-iostats-rdma-scan-20261028.md. NOTRUN: absent cluster scripts/Valgrind/RDMA and sanitizer attach refusal. Product defects: none found; unused routine scan removed.

## Purpose and source

Task AC in the [Stage 1 observability tasks](../alpha-1.0-2plus1-checklist.md)
identified a source-level cost: the periodic five-second `iostats:` line
requests all RDMA totals although it prints only I/O counters. The management
`io-stats` reply needs the full live-plus-retired totals. Base is current
devel `1d2852da4`, after Task S's merged repair; its original K48 attribution
remains separate. No existing cluster, client or store is operated by AC.

Candidate uses a private `snapshot_io` helper for both callers; only the
management snapshot aggregates the RDMA registry. No public API, wire field,
setting, logging format, counter ownership or connection lifetime changes.
The direct production-code unit requires routine/forced/idle dumps to avoid
the aggregator, while full snapshots retain every I/O and RDMA field.
Native transport coverage checks totals while two/one/zero connections are
live, retirement preserves all fields, and repeated totals do not recount.

Graph navigation (exact seed `efs_iostats_dump`, call context) found the
pump→dump→snapshot→totals path; source inspection confirms the mutex/scan.
The graph is navigation only, never measurement or runtime correctness proof.

## Reproduction and environment

Assigned TCP host intel1 (Task S code is merged; no Task S fixture process
found) and assigned hardware RDMA host node9901 (`mlx5_0`, private FUSE at
`/data1/efs/deps/libfuse`). Both hosts also have existing real clusters/clients;
those remain untouched, and their identities are saved in environment.txt.
These captures **are not quiet-host measurements**. Interference can widen
spread; no extrapolated isolated performance improvement will be claimed.

Exact exports: original base `1d2852da4`, clean candidate `5f9ddae71`.
`ac_instrument.py` creates separate measurement-only overlays for base/split;
identical bounded atomic histograms and clocks are used in both. No new EFS
setting or production probe is introduced. SIGUSR2 is sent only to the exact
new private instrumented daemon PIDs to delimit the load window. The overlay
is not used by the product gate/native tests.

```sh
# intel1, private HOME trees
bash ~/efs-task-ac/candidate/src/scripts/evidence/ac/ac_campaign.sh \
  /home/maik/efs-task-ac /home/maik/efs-artifacts/ac-20261028-5f9ddae71 5f9ddae71
# node9901, private /tmp trees, raw evidence under infrastructure evidence/
bash /tmp/efs-task-ac/candidate/src/scripts/evidence/ac/ac_campaign.sh \
  /tmp/efs-task-ac /data1/efs/evidence/ac-20261028-5f9ddae71 5f9ddae71
```

Each campaign runs under its own SCREENDIR (`ROOT/screen`), session
`task-ac-5f9`. It builds instrumented baseline and split exports, then six
fresh three-member/one-FUSE fixtures in balanced order base/split/split/base/
base/split: three repeats per variant, sixty seconds, four writers at ten
rewrites/s each, 32 fixed names per writer, 64KiB exact payloads, O_TRUNC +
full replacement + fsync, O_DIRECT storage, quota1GiB/member. A stat loop at
10Hz uses FUSE's zero attribute TTL. No failed operation is retried; every
final payload is exact. Missed offered-rate ticks and actual throughput are
recorded and insufficient sustained rate fails rather than silently reducing
load. Background work and instrument overhead remain limitations.

Recorded metrics: daemon CPU delta over the load window, stat p50/p99,
write throughput, registry wait classified by completion dispatch/totals/
lifetime caller, harvest-processing duration and pump work excluding idle
poll. Bounded log2 histogram quantiles are upper bounds, not exact percentiles;
maxima are exact clock differences. Raft printed hold/wait high-water values
also include fixture setup and are labelled accordingly. Results are summarized
with `python3 scripts/evidence/ac/ac_summarize.py CAPTURE` as median and min/max over all
three trials. An improvement is claimed only outside the measured spread.

After measurement, intel1 builds the uninstrumented candidate and runs the
snapshot unit and full8-job TCP gate. Node9901 uses the uninstrumented candidate
for the snapshot unit and existing xprt/segmented native RDMA commands.
Gate failures require matching untouched-base comparison or a fix with fresh
evidence; NOTRUN remains explicit. The full intel gate completed with zero failures and twelve explicit
host NOTRUNs; closed results are reviewed below. Raw fixtures are retained until reviewed, then only
inactive owned bulk is removed with edelete after log/manifest preservation.

## Three-repeat comparison

Each cell is median [minimum, maximum]. All twelve trials completed their
fixed-rate load and exact final payload checks without operation retries.
Raw results and analysis.json remain on their originating hosts:
`intel1:/home/maik/efs-artifacts/ac-20261028-5f9ddae71` and
`node9901:/data1/efs/evidence/ac-20261028-5f9ddae71`.

| Host / metric | Original base | Split |
|---|---:|---:|
| TCP daemon CPU, s | 34.90 [22.53,35.55] | 31.80 [24.72,35.65] |
| TCP stat p50, µs | 629.423 [425.070,851.149] | 713.341 [450.514,747.305] |
| TCP stat p99, µs | 1612.513 [1605.720,2157.868] | 42540.315 [1082.712,48545.356] |
| TCP rewrite MiB/s | 2.499151 [2.497528,2.499989] | 2.490194 [2.489272,2.495170] |
| RDMA daemon CPU, s | 26.72 [26.49,27.36] | 25.81 [25.33,27.09] |
| RDMA stat p50, µs | 528.567 [495.768,535.463] | 364.181 [277.567,557.853] |
| RDMA stat p99, µs | 46019.703 [42212.242,46040.563] | 561.633 [515.513,46370.255] |
| RDMA rewrite MiB/s | 2.499988 [2.499987,2.499991] | 2.499988 [2.496367,2.499989] |
| Routine totals acquisitions, either host | 36 [36,36] | 0 [0,0] |
| TCP routine totals wait p99 upper bound, µs | 2.047 [.511,4.095] | no acquisitions |
| RDMA routine totals wait p99 upper bound, µs | 8.191 [.511,16.383] | no acquisitions |
| TCP dump p50 upper bound, µs | 131.071 [65.535,131.071] | 65.535 [32.767,65.535] |
| TCP dump p99 upper bound, µs | 262.143 [131.071,1048.575] | 131.071 [131.071,131.071] |
| RDMA dump p50 upper bound, µs | 32.767 [32.767,32.767] | 32.767 [32.767,32.767] |
| RDMA dump p99 upper bound, µs | 65.535 [65.535,65.535] | 65.535 [65.535,1048.575] |
| TCP late rate ticks | 8 [0,8] | 20 [17,21] |
| RDMA late rate ticks | 0 [0,0] | 0 [0,4] |
| RDMA poll registry wait p99 upper bound, µs | .511 [.511,.511] | .511 [.511,.511] |
| RDMA harvest processing p99 upper bound, µs | 16.383 [16.383,16.383] | 16.383 [16.383,16.383] |
| RDMA pump work p99 upper bound, µs | 524.287 [524.287,524.287] | 524.287 [524.287,1048.575] |
| TCP pump work p99 upper bound, µs | 8388.607 [4194.303,8388.607] | 8388.607 [4194.303,8388.607] |
| RDMA whole-fixture Raft hold high water, µs | 21838 [5262,21854] | 14688 [4894,21747] |
| RDMA whole-fixture Raft wait high water, µs | 1883 [1568,2487] | 5868 [2270,6895] |
| TCP whole-fixture Raft hold high water, µs | 52660 [50452,69379] | 34058 [33576,40766] |
| TCP whole-fixture Raft wait high water, µs | 9390 [9107,16498] | 10731 [7282,12664] |

The deterministic result is removal of unused registry acquisitions, confirmed
by the production-code unit and identical measurement probes. No consistent
poller or end-to-end improvement is demonstrated. Latency/CPU ranges overlap;
RDMA wait high water is higher in the split median. TCP throughput ranges do
not overlap: the split median is about 0.36% lower. Offered-rate late ticks,
background occupancy and three short repeats limit attribution; this small
negative result is retained, not presented as a speedup. TCP has no RDMA
completion poller, so its poller latency is NOTAPPLICABLE.

## Snapshot correctness and explicit status cost

The candidate snapshot unit PASS on both hosts checks all five I/O classes
and all six RDMA wire fields. Linking this same test against untouched
`1d2852da4:src/server/iostats.c` on node9901 fails the intended `scans == 0`
assertion (exit134, core disabled); candidate recheck PASS. This is the
expected negative control, retained in base-unit-negative.log and
base-unit-negative-exit, not an unexplained gate failure.

All eight clean native RDMA commands on node9901 PASS without skips:
xprt, segmented normal, interrupt, tcp-violation, deadline, burst, burst-asym,
and churn. The xprt reports 68 application RDMA frames and no large TCP
fallback; segmented normal reports 9/9 application messages over RDMA.
The xprt checks live-two/live-one/retired-zero totals and repeated totals
without double counting. Churn covers sixteen reconnects and zero final
resources. The existing 50ms deadline and interruption/refusal checks pass.

The supplementary `ac_status_cost.c` uses actual native test connections and
the real full management snapshot, 1,000 snapshots per repeat, three repeats
at each nonzero count and after retirement, plus one initial-zero run on mlx5_0. Median p50: zero-live2040ns, two-live2040ns,
eight-live2050ns, 32-live2080ns, retired-zero2040ns. Corresponding p99 ranges:
2051ns (initial zero), 2050–2060ns, 2060–2150ns, 2090–2290ns and
2050–2070ns. This exercises 0/2/8/32 real connections, not 4,096 live
connections. The registry capacity remains 4,096 slots. Full totals and
management reply fields retain the original aggregation semantics.

Reproduce the negative control by linking tests/test_iostats_snapshot.c with
original iostats.c exported by `git show 1d2852da4:src/server/iostats.c` and
the same include flags as its Makefile target; expect nonzero/assertion.
For status cost, compile scripts/evidence/ac/ac_status_cost.c with src/server/iostats.c,
libefs.a, project include paths, pthread/math/dl/ibverbs, then run with
`EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0`. The committed source includes the
native test handshake; no external payload capture is required.

The management RPC smoke also PASS on a separate private node9901 TCP
fixture (`python3 tests/live/io_stats_smoke.py`): exact disk/PUT success and
error counters, refused unticketed PUT, missing GET after unlink and single
reclaim removal. Raw management-smoke.log/exit are retained. Expected refusal
and missing-object operations belong to this explicit negative test, not the
fixed-rate measurement workload.

Clean node9901 build identity is `efsd 5f9ddae71`, branch
`agent/devel-task-ac`, build `2026-10-28T22:40:31Z`; identity and SHA256
receipts are retained beside raw outputs. All 72 closed evidence files were
hashed: SHA256SUMS root `061980f5be668f98ef794a41221ae5576a349f62aa854da74b64bdc4b7799b5a`,
with verification log retained separately. No private daemon/FUSE executable
or private mount remained at closure. Inactive owned `/tmp/efs-task-ac`
source/build/fixture/payload bulk was then pruned with edelete (errors0);
PRUNED.txt and prune.log remain with the evidence. Existing real cluster processes were
not stopped or exempted from the occupancy disclosure.

The intel gate runner executed **546 complete commands**. The static recipe
inventory counts 961 shell/recipe statements, a different measure, not 961
executed tests. All 546 timing receipts have exit0. Campaign receipts
include gate-exit separately: the tested campaign originally wrote controller0
regardless of a completed gate's status; review must use gate-exit/summary.
The current script propagates that status too, without changing product code
or the already-running gate.

## Final verdict, gate limits and preservation

**AC implementation and assigned-host qualification PASS within the executed
scope. Full release gate remains INCOMPLETE, not an all-host/all-feature PASS.**
The uninstrumented intel1 gate returned3: `TOTAL 546 FAIL 0 NOTRUN 12 STATUS
INCOMPLETE (748s) OVERBUDGET 5 BUILD 10s`. No failing command needs an
untouched-base failure reproduction. The base comparison is the six-trial
measurement and expected failing scan-control unit, not a base full-gate run.
No new product correctness defect was found. The removal is justified by
unneeded shared-lock work; no end-to-end performance gain is established.

The twelve NOTRUNs are cluster-stop/remount scripts absent; Valgrind
memcheck.h absent; declaration RDMA frame path and eight xprt/segmented
commands without an intel1 IB device; sanitizer host attach refusal.
The eight native commands ran separately on mlx5_0 with no skips; declaration
frame-path coverage on intel1 remains NOTRUN. Sanitizer errors/findings or
timeouts were not relabelled as host limits. Five advisory budget overruns
remain: shared-chunk pressure38.8/35s, stop-progress56.5/50s,
coordinator-kill54.2/25s, leader-admission40.3/30s and KV LSM10.4/10s.
Another campaign was observed testing on intel1 during this gate; it was not
operated by AC. This is shared-load qualification, not a quiet-host capture.

Intel1 clean identity: `efsd 5f9ddae71`, branch `agent/devel-task-ac`, build
`2026-10-28T22:40:49Z`. Post-gate binary hashes match the pre-gate receipt.
Its 65-file SHA256SUMS root is
`9fb3b21bf6f665e45e5448c0ab2d961c6a8e146d635b2b2c987a184169e3860d`;
verification, pruning and PRUNED.txt receipts remain alongside it. After
checking no private executable or mount remained, inactive owned
`/home/maik/efs-task-ac` scratch bulk was pruned with edelete (errors0).
Neither origin evidence directory was deleted. Expected baseline assertion
failure evidence remains on node9901. No existing real client/store/cluster
was stopped, modified or cleaned up.

Tested product, Makefile and native/unit test source remain exactly those
at5f9ddae71. Later branch commits add analysis/status-cost tooling,
documentation and propagation of the gate receipt from the campaign; these
do not change the gated production code. The coordinator still owns merge
and merged qualification; no merge or push was performed.

Local documentation validation: architecture generation and all five checks,
recipe inventory, settings inventory and script syntax PASS. Canonical graph
was rebuilt with the repository script and its nine tests PASS. Graph parser
warnings (protocol.h region,49 files without symbols, unresolved call edges)
remain coverage gaps; the graph is not runtime proof.
